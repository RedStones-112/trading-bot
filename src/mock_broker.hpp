#pragma once
#include "broker.hpp"
#include <deque>
#include <random>

// Local, offline stand-in for KisClient: no network, no account, no KIS 모의투자
// signup needed. Prices are a random walk; orders are just recorded, not sent
// anywhere. Good enough to exercise main.cpp's polling/strategy/logging pipeline
// end to end -- not a real backtest (no historical accuracy, no fees/slippage).
class MockBroker : public IBroker {
public:
    explicit MockBroker(double startPrice = 70000.0, unsigned seed = std::random_device{}());

    void authenticate() override {} // nothing to do

    double getCurrentPrice(const std::string& code) override;
    std::vector<double> getDailyCloses(const std::string& code, int count) override;
    std::string placeMarketOrder(const std::string& code, Side side, int qty) override;

private:
    std::deque<double> history_;
    double lastPrice_;
    std::mt19937 rng_;
    int orderSeq_ = 0;
};
