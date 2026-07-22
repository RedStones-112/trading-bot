#include "mock_broker.hpp"
#include <algorithm>

MockBroker::MockBroker(double startPrice, unsigned seed) : lastPrice_(startPrice), rng_(seed) {
    std::normal_distribution<double> step(0.0, startPrice * 0.01);
    double p = startPrice;
    for (int i = 0; i < 60; i++) {
        p = std::max(1.0, p + step(rng_));
        history_.push_back(p);
    }
    lastPrice_ = history_.back();
}

double MockBroker::getCurrentPrice(const std::string&) {
    std::normal_distribution<double> step(0.0, lastPrice_ * 0.005);
    lastPrice_ = std::max(1.0, lastPrice_ + step(rng_));
    history_.push_back(lastPrice_);
    if (history_.size() > 500) history_.pop_front();
    return lastPrice_;
}

std::vector<double> MockBroker::getDailyCloses(const std::string&, int count) {
    int n = (int)history_.size();
    int take = std::min(count, n);
    return std::vector<double>(history_.end() - take, history_.end());
}

std::string MockBroker::placeMarketOrder(const std::string&, Side, int qty) {
    (void)qty;
    return "MOCK" + std::to_string(++orderSeq_);
}
