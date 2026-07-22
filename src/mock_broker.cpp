#include "mock_broker.hpp"
#include <algorithm>

namespace {
// Fixed pretend universe so mock mode can exercise the scan/pick logic without any API.
const std::vector<StockInfo> kMockUniverse = {
    {"000001", "가상전자"}, {"000002", "가상바이오"}, {"000003", "가상화학"},
    {"000004", "가상은행"}, {"000005", "가상건설"},
};
}

MockBroker::MockBroker(double startPrice, unsigned seed) : startPrice_(startPrice), rng_(seed) {}

MockBroker::Series& MockBroker::seriesFor(const std::string& code) {
    auto it = series_.find(code);
    if (it != series_.end()) return it->second;

    Series s;
    std::normal_distribution<double> step(0.0, startPrice_ * 0.01);
    double p = startPrice_;
    for (int i = 0; i < 60; i++) {
        p = std::max(1.0, p + step(rng_));
        s.history.push_back(p);
    }
    s.lastPrice = s.history.back();
    return series_[code] = s;
}

std::string MockBroker::getStockName(const std::string& code) {
    for (auto& s : kMockUniverse) if (s.code == code) return s.name;
    return "가상종목";
}

std::vector<StockInfo> MockBroker::getTopVolumeStocks(int count) {
    int take = std::min(count, (int)kMockUniverse.size());
    return std::vector<StockInfo>(kMockUniverse.begin(), kMockUniverse.begin() + take);
}

double MockBroker::getCurrentPrice(const std::string& code) {
    Series& s = seriesFor(code);
    std::normal_distribution<double> step(0.0, s.lastPrice * 0.005);
    s.lastPrice = std::max(1.0, s.lastPrice + step(rng_));
    s.history.push_back(s.lastPrice);
    if (s.history.size() > 500) s.history.pop_front();
    return s.lastPrice;
}

std::vector<double> MockBroker::getDailyCloses(const std::string& code, int count) {
    auto& history = seriesFor(code).history;
    int n = (int)history.size();
    int take = std::min(count, n);
    return std::vector<double>(history.end() - take, history.end());
}

std::string MockBroker::placeMarketOrder(const std::string&, Side, int) {
    return "MOCK" + std::to_string(++orderSeq_);
}
