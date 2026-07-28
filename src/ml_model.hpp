#pragma once
#include "broker.hpp"
#include <map>
#include <string>
#include <utility>
#include <vector>

// Per-symbol input signals fed to the neural net -- the same technical features
// probabilityFromTechnicals (strategy.hpp) already computes from data a scan fetches for
// free, plus SMA momentum and today's % change, so switching ml_enabled on doesn't need any
// signal that wasn't already available.
struct MlFeatures {
    double trendPct = 0.0;
    double belowRatio = 0.5;
    double volumeSurgePct = 100.0;
    double smaMomentumVal = 0.0;
    double dayChangePct = 0.0;
};

// Builds (features, label) samples by walking historical daily bars: anchored at bar i,
// label=1 if the price's high reaches +takeProfitPct before its low reaches -stopLossPct
// within the next `lookaheadDays` bars, label=0 otherwise (stop-loss first, both same day
// -- ambiguous, treated conservatively as no clean win --, or neither within the horizon).
// `windowBars` bars ending at each anchor feed trend/volume-profile, same window size the
// live scan uses (sma_long + 5); needs windowBars >= smaLong. Pure function, no I/O -- easy
// to unit-test independently of the neural net itself.
std::vector<std::pair<MlFeatures, double>> buildTrainingSet(
    const std::vector<DailyBar>& bars, double takeProfitPct, double stopLossPct,
    int lookaheadDays, int windowBars, int smaShort, int smaLong);

// One feedforward net (third_party/genann.h/.c) per stock code -- a symbol's price behavior
// doesn't generalize to another's, so each gets its own model instead of one shared model
// across all candidates. Weights persist as plain text under `<dir>/<code>.nn` (genann's own
// read/write format) plus `<dir>/index.json` tracking each symbol's last-trained date, so a
// restart doesn't retrain everything and a symbol only retrains once every `retrainDays`
// (see ml_retrain_days in config.json) -- same "file-backed, tolerant of a missing/corrupt
// file" spirit as stock_tags.json / event_calendar.hpp.
class MlModelStore {
public:
    explicit MlModelStore(std::string dir);
    ~MlModelStore();
    MlModelStore(const MlModelStore&) = delete;
    MlModelStore& operator=(const MlModelStore&) = delete;

    // True if `code` has no model yet, or its last training is >= retrainDays calendar days old.
    bool needsRetrain(const std::string& code, int retrainDays, const std::string& todayIso) const;

    // (Re)trains code's model from `bars` and persists both the weights and the index.
    // Skips (no state change) if there aren't enough bars for a useful training set, so
    // needsRetrain() will still report it as due next time it's asked. Returns the number
    // of training samples used (0 = skipped).
    int train(const std::string& code, const std::vector<DailyBar>& bars, double takeProfitPct,
              double stopLossPct, int lookaheadDays, int smaShort, int smaLong,
              const std::string& todayIso);

    // Probability estimate in [0.05, 0.95] (same clamp shape as probabilityFromTechnicals,
    // so it drops straight into the existing EV formula), or -1 if `code` has no model yet.
    double predict(const std::string& code, const MlFeatures& features) const;

private:
    std::string dir_;
    std::map<std::string, std::string> lastTrained_; // code -> "YYYY-MM-DD"
    mutable std::map<std::string, struct genann*> loaded_; // lazily loaded from disk, cached (may cache nullptr)

    void saveIndex() const;
    struct genann* get(const std::string& code) const;
};
