#pragma once
#include <string>
#include <vector>

// Client for Korea Investment & Securities (KIS) Developers REST API.
// Paper trading (모의투자) by default: openapivts.koreainvestment.com.
// Docs: https://apiportal.koreainvestment.com -- verify tr_id / field names against
// current docs before relying on this; the API has changed shape before.
class KisClient {
public:
    KisClient(std::string appkey, std::string appsecret,
              std::string cano, std::string acntPrdtCd,
              bool paperTrading = true);

    void authenticate(); // fetches OAuth access token, must call before other methods

    // Latest traded price for a 6-digit KRX code (e.g. "005930" = Samsung Electronics).
    double getCurrentPrice(const std::string& code);

    // Most recent `count` daily close prices, oldest first.
    std::vector<double> getDailyCloses(const std::string& code, int count);

    enum class Side { Buy, Sell };
    // Market order (시장가). Returns the broker's order number, or throws on rejection.
    std::string placeMarketOrder(const std::string& code, Side side, int qty);

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
