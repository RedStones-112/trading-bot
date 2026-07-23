#pragma once
#include <string>
#include <vector>

struct StockInfo {
    std::string code;
    std::string name;
    double price; // current price, as reported by the ranking query itself
    double dayChangePct = 0.0; // 전일대비율, also free from the ranking query -- used as a cheap pre-filter
};

// A stock already sitting in the real account, as reported by the broker itself --
// used to rebuild in-memory positions after a restart (the bot has no local persistence,
// so anything bought in a previous run is otherwise invisible until this is queried).
struct HeldStock {
    std::string code;
    std::string name;
    int qty;
    double avgBuyPrice;
};

// An order still sitting unfilled (or partially filled) in the account, as reported by
// the broker's pending-order listing (live only -- see IBroker::getPendingOrders).
struct PendingOrder {
    std::string odno;
    std::string code;
    std::string name;
    int qty; // cancellable (unfilled) quantity
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

    // Cash currently available to spend on new buys (매수가능현금). Real for
    // paper/live (KIS account balance); a locally-tracked virtual balance for mock/sim.
    virtual double getBuyableCash() = 0;

    // Stocks already held in the account right now (매입평균가 included), so the caller
    // can rebuild its in-memory position state after a restart. Real for paper/live;
    // always empty for mock/sim (their "account" is just this process's memory, so a
    // restart legitimately starts from flat -- there's nothing external to reconcile with).
    virtual std::vector<HeldStock> getHoldings() = 0;

    // Orders still outstanding (unfilled/partially filled) in the account, keyed by order
    // id -- only live supports this listing (KIS 모의투자 rejects the query outright,
    // confirmed via a live call: "모의투자에서는 해당업무가 제공되지 않습니다"). Always
    // empty for mock/sim/paper.
    virtual std::vector<PendingOrder> getPendingOrders() = 0;
    // Cancels an order by id, whatever quantity of it is still unfilled. Unlike the
    // listing above, this works fine on paper accounts too (confirmed live). No-op for
    // mock/sim (their orders fill synchronously, so nothing is ever left pending).
    virtual void cancelOrder(const std::string& odno) = 0;

    enum class Side { Buy, Sell };
    // Market order (시장가). Returns an order id, or throws on rejection.
    // feeRate/taxRate are only consumed by brokers that track their own virtual cash
    // balance (mock/sim) to debit/credit it correctly; KisClient ignores them since the
    // real account balance already reflects KIS's own fee/tax handling.
    virtual std::string placeMarketOrder(const std::string& code, Side side, int qty,
                                          double feeRate = 0.0, double taxRate = 0.0) = 0;
};
