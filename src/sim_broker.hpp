#pragma once
#include "broker.hpp"
#include "kis_client.hpp"

// Real KIS market data (live host, real appkey -- no 모의투자 signup needed for quotes),
// but orders never leave the machine: placeMarketOrder just fills virtually at the
// current market price. So "실시간 시세 + 로컬 가상 매매" without touching KIS's own
// paper-trading account setup.
class SimBroker : public IBroker {
public:
    SimBroker(std::string appkey, std::string appsecret);

    void authenticate() override { kis_.authenticate(); }
    std::string getStockName(const std::string& code) override { return kis_.getStockName(code); }
    double getCurrentPrice(const std::string& code) override { return kis_.getCurrentPrice(code); }
    std::vector<double> getDailyCloses(const std::string& code, int count) override { return kis_.getDailyCloses(code, count); }
    std::string placeMarketOrder(const std::string& code, Side side, int qty) override;

private:
    KisClient kis_; // live host, quotes only -- kis_.placeMarketOrder() is never called
    int orderSeq_ = 0;
};
