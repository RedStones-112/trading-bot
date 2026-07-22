#pragma once
#include "broker.hpp"
#include <string>
#include <vector>

// Client for Korea Investment & Securities (KIS) Developers REST API.
// Paper trading (모의투자) by default: openapivts.koreainvestment.com.
// Docs: https://apiportal.koreainvestment.com -- verify tr_id / field names against
// current docs before relying on this; the API has changed shape before.
class KisClient : public IBroker {
public:
    KisClient(std::string appkey, std::string appsecret,
              std::string cano, std::string acntPrdtCd,
              bool paperTrading = true);

    void authenticate() override; // fetches OAuth access token, must call before other methods

    std::string getStockName(const std::string& code) override;
    double getCurrentPrice(const std::string& code) override;
    std::vector<StockInfo> getTopVolumeStocks(int count) override;

    std::vector<double> getDailyCloses(const std::string& code, int count) override;

    std::string placeMarketOrder(const std::string& code, Side side, int qty) override;

private:
    std::string request(const std::string& path, const std::string& method,
                         const std::string& trId, const std::string& queryOrBody,
                         bool isQuery);
    std::string hashKey(const std::string& jsonBody);

    std::string appkey_, appsecret_, cano_, acntPrdtCd_;
    std::wstring host_;
    int port_;
    bool paper_;
    std::string accessToken_;
};
