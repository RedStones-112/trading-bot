#include "ml_model.hpp"
#include "date_util.hpp"
#include "strategy.hpp"
#include "../third_party/genann.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cstdio>
#include <direct.h>
#include <fstream>
#include <random>

using json = nlohmann::json;

namespace {
constexpr int kHiddenNeurons = 8;
constexpr int kEpochs = 300;
constexpr double kLearningRate = 0.05;
constexpr int kMinTrainingSamples = 20; // fewer than this and a fresh net just memorizes noise

// Scales raw features to roughly unit range -- genann starts weights in [-0.5, 0.5], so
// keeping inputs O(1) helps the sigmoid hidden units actually differentiate examples instead
// of saturating on a handful of large-magnitude ones (volumeSurgePct/dayChangePct are % values).
std::vector<double> toVector(const MlFeatures& f) {
    return {f.trendPct, f.belowRatio, (f.volumeSurgePct - 100.0) / 100.0, f.smaMomentumVal,
            f.dayChangePct / 100.0};
}
} // namespace

std::vector<std::pair<MlFeatures, double>> buildTrainingSet(
    const std::vector<DailyBar>& bars, double takeProfitPct, double stopLossPct,
    int lookaheadDays, int windowBars, int smaShort, int smaLong) {
    std::vector<std::pair<MlFeatures, double>> samples;
    if (windowBars < smaLong) return samples;
    int n = (int)bars.size();
    for (int i = windowBars - 1; i + lookaheadDays < n; i++) {
        std::vector<DailyBar> window(bars.begin() + (i - windowBars + 1), bars.begin() + i + 1);
        std::vector<double> closes;
        closes.reserve(window.size());
        for (auto& b : window) closes.push_back(b.close);

        double anchor = bars[i].close;
        MlFeatures f;
        f.trendPct = window.front().close > 0 ? (anchor - window.front().close) / window.front().close : 0.0;
        f.belowRatio = belowPriceVolumeRatio(window, anchor);
        f.volumeSurgePct = (i > 0 && bars[i - 1].volume > 0) ? bars[i].volume / bars[i - 1].volume * 100.0 : 100.0;
        f.smaMomentumVal = smaMomentum(closes, smaShort, smaLong);
        f.dayChangePct = (i > 0 && bars[i - 1].close > 0) ? (anchor - bars[i - 1].close) / bars[i - 1].close * 100.0 : 0.0;

        double label = 0.0;
        for (int d = 1; d <= lookaheadDays; d++) {
            const DailyBar& fut = bars[i + d];
            bool tpHit = fut.high > 0 && fut.high >= anchor * (1 + takeProfitPct);
            bool slHit = fut.low > 0 && fut.low <= anchor * (1 - stopLossPct);
            if (tpHit && slHit) break; // same-day cross both ways -- ambiguous, no clean win
            if (tpHit) { label = 1.0; break; }
            if (slHit) break;
        }
        samples.push_back({f, label});
    }
    return samples;
}

MlModelStore::MlModelStore(std::string dir) : dir_(std::move(dir)) {
    _mkdir(dir_.c_str()); // best-effort; ignored if it already exists (matches this repo's tolerant-of-missing-state file stores)
    std::ifstream in(dir_ + "/index.json");
    if (!in) return;
    try {
        json j = json::parse(in, nullptr, true, true);
        for (auto it = j.begin(); it != j.end(); ++it) lastTrained_[it.key()] = it.value().get<std::string>();
    } catch (const std::exception&) {
        // corrupt index -- start empty rather than crash the bot over this, same tolerance as stock_tags.json
    }
}

MlModelStore::~MlModelStore() {
    for (auto& [code, ann] : loaded_)
        if (ann) genann_free(ann);
}

bool MlModelStore::needsRetrain(const std::string& code, int retrainDays, const std::string& todayIso) const {
    auto it = lastTrained_.find(code);
    if (it == lastTrained_.end()) return true;
    try {
        return isoDayNumber(todayIso) - isoDayNumber(it->second) >= retrainDays;
    } catch (const std::exception&) {
        return true; // corrupt stored date -- treat as due rather than get stuck never retraining
    }
}

int MlModelStore::train(const std::string& code, const std::vector<DailyBar>& bars, double takeProfitPct,
                         double stopLossPct, int lookaheadDays, int smaShort, int smaLong,
                         const std::string& todayIso) {
    int windowBars = smaLong + 5; // same window size the live scan fetches for trend/volume-profile
    auto samples = buildTrainingSet(bars, takeProfitPct, stopLossPct, lookaheadDays, windowBars, smaShort, smaLong);
    if ((int)samples.size() < kMinTrainingSamples) return 0;

    genann* ann = genann_init(5, 1, kHiddenNeurons, 1);
    std::vector<int> order(samples.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::mt19937 rng(std::random_device{}());
    for (int epoch = 0; epoch < kEpochs; epoch++) {
        std::shuffle(order.begin(), order.end(), rng);
        for (int idx : order) {
            auto in = toVector(samples[idx].first);
            double out = samples[idx].second;
            genann_train(ann, in.data(), &out, kLearningRate);
        }
    }

    FILE* f = fopen((dir_ + "/" + code + ".nn").c_str(), "w");
    if (f) { genann_write(ann, f); fclose(f); }

    auto old = loaded_.find(code);
    if (old != loaded_.end() && old->second) genann_free(old->second);
    loaded_[code] = ann;
    lastTrained_[code] = todayIso;
    saveIndex();
    return (int)samples.size();
}

struct genann* MlModelStore::get(const std::string& code) const {
    auto it = loaded_.find(code);
    if (it != loaded_.end()) return it->second;
    if (!lastTrained_.count(code)) return nullptr; // never trained -- no file to load
    FILE* f = fopen((dir_ + "/" + code + ".nn").c_str(), "r");
    genann* ann = f ? genann_read(f) : nullptr;
    if (f) fclose(f);
    loaded_[code] = ann; // cache even a load failure so we don't retry a corrupt file every poll
    return ann;
}

double MlModelStore::predict(const std::string& code, const MlFeatures& features) const {
    genann* ann = get(code);
    if (!ann) return -1.0;
    auto in = toVector(features);
    double out = *genann_run(ann, in.data());
    return std::clamp(out, 0.05, 0.95);
}

void MlModelStore::saveIndex() const {
    json j;
    for (auto& [code, date] : lastTrained_) j[code] = date;
    std::ofstream out(dir_ + "/index.json");
    out << j.dump(2);
}
