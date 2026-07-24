#include "mock_broker.hpp"
#include <algorithm>
#include <stdexcept>

namespace {
// Fixed pretend universe so mock mode can exercise the scan/pick logic without any API.
const std::vector<std::pair<std::string, std::string>> kMockUniverse = {
    {"000001", "가상전자"}, {"000002", "가상바이오"}, {"000003", "가상화학"},
    {"000004", "가상은행"}, {"000005", "가상건설"},
};

// Synthesized fundamentals so the valuation/momentum multipliers have something to chew
// on in mock mode -- three stocks deliberately share a sector so the peer-comparison path
// (which requires >=3 same-sector candidates) can actually be exercised in tests. 000005
// is shaped to have the highest PER of the three (looks "expensive" by earnings) but the
// lowest PBR (cheapest by book value) -- exercises the blended PER+PBR valuation path.
struct MockFundamental { double basePer; double basePbr; std::string sector; };
const std::map<std::string, MockFundamental> kMockFundamentals = {
    {"000001", {18.0, 2.5, "전기전자"}}, {"000002", {30.0, 4.0, "바이오"}}, {"000003", {14.0, 3.0, "전기전자"}},
    {"000004", {8.0, 0.8, "금융"}},     {"000005", {22.0, 1.0, "전기전자"}},
};
}

MockBroker::MockBroker(double startPrice, double initialCash, unsigned seed)
    : startPrice_(startPrice), cash_(initialCash), rng_(seed) {}

MockBroker::Series& MockBroker::seriesFor(const std::string& code) {
    auto it = series_.find(code);
    if (it != series_.end()) return it->second;

    Series s;
    std::normal_distribution<double> step(0.0, startPrice_ * 0.01);
    std::uniform_real_distribution<double> spread(0.995, 1.005); // synthetic intraday range around each close
    std::uniform_real_distribution<double> volRand(1000.0, 100000.0);
    double p = startPrice_;
    for (int i = 0; i < 60; i++) {
        double prev = p;
        p = std::max(1.0, p + step(rng_));
        DailyBar b;
        b.close = p;
        b.high = std::max(prev, p) * spread(rng_);
        b.low = std::min(prev, p) / spread(rng_);
        b.volume = volRand(rng_);
        s.history.push_back(b);
    }
    s.lastPrice = s.history.back().close;
    return series_[code] = s;
}

std::string MockBroker::getStockName(const std::string& code) {
    for (auto& s : kMockUniverse) if (s.first == code) return s.second;
    return "가상종목";
}

std::vector<StockInfo> MockBroker::getTopVolumeStocks(int count) {
    int take = std::min(count, (int)kMockUniverse.size());
    std::vector<StockInfo> result;
    // Occasionally synthesize a volume-surge spike so the interest-multiplier arithmetic
    // has something to react to in mock mode, same idea as the price random walk.
    std::uniform_real_distribution<double> volSurge(30.0, 400.0);
    for (int i = 0; i < take; i++) {
        const auto& [code, name] = kMockUniverse[i];
        // Real KIS's ranking response reflects the live market on every call, so advance
        // the mock's random walk here too -- otherwise scan-time prices never move.
        double price = advance(seriesFor(code));
        result.push_back({code, name, price, 0.0, volSurge(rng_)});
    }
    return result;
}

double MockBroker::advance(Series& s) {
    std::normal_distribution<double> step(0.0, s.lastPrice * 0.005);
    std::uniform_real_distribution<double> spread(0.995, 1.005);
    std::uniform_real_distribution<double> volRand(1000.0, 100000.0);
    double prev = s.lastPrice;
    s.lastPrice = std::max(1.0, s.lastPrice + step(rng_));
    DailyBar b;
    b.close = s.lastPrice;
    b.high = std::max(prev, s.lastPrice) * spread(rng_);
    b.low = std::min(prev, s.lastPrice) / spread(rng_);
    b.volume = volRand(rng_);
    s.history.push_back(b);
    if (s.history.size() > 500) s.history.pop_front();
    return s.lastPrice;
}

double MockBroker::getCurrentPrice(const std::string& code) {
    return advance(seriesFor(code));
}

std::vector<DailyBar> MockBroker::getDailyBars(const std::string& code, int count) {
    auto& history = seriesFor(code).history;
    int n = (int)history.size();
    int take = std::min(count, n);
    return std::vector<DailyBar>(history.end() - take, history.end());
}

Fundamentals MockBroker::getFundamentals(const std::string& code) {
    auto it = kMockFundamentals.find(code);
    if (it == kMockFundamentals.end()) return {15.0, 1.5, "기타"};
    std::normal_distribution<double> perJitter(0.0, it->second.basePer * 0.05);
    std::normal_distribution<double> pbrJitter(0.0, it->second.basePbr * 0.05);
    return {std::max(1.0, it->second.basePer + perJitter(rng_)),
            std::max(0.1, it->second.basePbr + pbrJitter(rng_)), it->second.sector};
}

std::string MockBroker::placeMarketOrder(const std::string& code, Side side, int qty,
                                          double feeRate, double taxRate) {
    double price = seriesFor(code).lastPrice;
    if (side == Side::Buy) {
        double cost = qty * price * (1 + feeRate);
        if (cost > cash_) throw std::runtime_error("insufficient cash: need " + std::to_string(cost) +
                                                     ", have " + std::to_string(cash_));
        cash_ -= cost;
    } else {
        cash_ += qty * price * (1 - feeRate - taxRate);
    }
    return "MOCK" + std::to_string(++orderSeq_);
}
