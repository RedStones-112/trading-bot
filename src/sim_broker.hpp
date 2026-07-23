#pragma once
#include "broker.hpp"
#include "kis_client.hpp"

// Real KIS market data (live host, real appkey -- no 모의투자 signup needed for quotes),
// but orders never leave the machine: placeMarketOrder just fills virtually at the
// current market price. So "실시간 시세 + 로컬 가상 매매" without touching KIS's own
// paper-trading account setup.
class SimBroker : public IBroker {
public:
    SimBroker(std::string appkey, std::string appsecret, double initialCash = 10000000.0);

    void authenticate() override { kis_.authenticate(); }
    std::string getStockName(const std::string& code) override { return kis_.getStockName(code); }
    std::vector<StockInfo> getTopVolumeStocks(int count) override { return kis_.getTopVolumeStocks(count); }
    double getCurrentPrice(const std::string& code) override { return kis_.getCurrentPrice(code); }
    std::vector<double> getDailyCloses(const std::string& code, int count) override { return kis_.getDailyCloses(code, count); }
    double getBuyableCash() override { return cash_; }
    std::vector<HeldStock> getHoldings() override { return {}; }
    std::vector<PendingOrder> getPendingOrders() override { return {}; }
    void cancelOrder(const std::string&) override {}
    std::string placeMarketOrder(const std::string& code, Side side, int qty,
                                  double feeRate, double taxRate) override;

private:
    KisClient kis_; // live host, quotes only -- kis_.placeMarketOrder() is never called
    double cash_;
    int orderSeq_ = 0;
};
