#pragma once
#include <string>
#include <vector>

struct StockInfo {
    std::string code;
    std::string name;
    double price; // current price, as reported by the ranking query itself
};

// Common interface so main.cpp can run against a real broker (KisClient) or a
// local synthetic one (MockBroker) without caring which.
class IBroker {
public:
    virtual ~IBroker() = default;

    virtual void authenticate() = 0;

    // Human-readable name for a 6-digit KRX code (e.g. "005930" -> "삼성전자").
    virtual std::string getStockName(const std::string& code) = 0;

    // Top `count` stocks by trading volume -- the candidate universe to scan for a pick.
    virtual std::vector<StockInfo> getTopVolumeStocks(int count) = 0;

    // Latest traded price for a 6-digit KRX code (e.g. "005930" = Samsung Electronics).
    virtual double getCurrentPrice(const std::string& code) = 0;

    // Most recent `count` daily close prices, oldest first.
    virtual std::vector<double> getDailyCloses(const std::string& code, int count) = 0;

    enum class Side { Buy, Sell };
    // Market order (시장가). Returns an order id, or throws on rejection.
    virtual std::string placeMarketOrder(const std::string& code, Side side, int qty) = 0;
};
