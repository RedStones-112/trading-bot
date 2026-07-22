#pragma once
#include "broker.hpp"
#include <deque>
#include <map>
#include <random>

// Local, offline stand-in for KisClient: no network, no account, no KIS 모의투자
// signup needed. Prices are a random walk per symbol; orders are just recorded,
// not sent anywhere. Good enough to exercise main.cpp's scan/pick/monitor pipeline
// end to end -- not a real backtest (no historical accuracy, no fees/slippage).
class MockBroker : public IBroker {
public:
    explicit MockBroker(double startPrice = 70000.0, unsigned seed = std::random_device{}());

    void authenticate() override {} // nothing to do

    std::string getStockName(const std::string& code) override;
    std::vector<StockInfo> getTopVolumeStocks(int count) override;
    double getCurrentPrice(const std::string& code) override;
    std::vector<double> getDailyCloses(const std::string& code, int count) override;
    std::string placeMarketOrder(const std::string& code, Side side, int qty) override;

private:
    struct Series {
        std::deque<double> history;
        double lastPrice;
    };
    Series& seriesFor(const std::string& code);
    double advance(Series& s); // one random-walk step, appended to history

    double startPrice_;
    std::map<std::string, Series> series_;
    std::mt19937 rng_;
    int orderSeq_ = 0;
};
