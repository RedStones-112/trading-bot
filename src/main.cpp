#include "broker.hpp"
#include "kis_client.hpp"
#include "mock_broker.hpp"
#include "news_crawler.hpp"
#include "sim_broker.hpp"
#include "strategy.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <cmath>
#include <windows.h>

using json = nlohmann::json;

static std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// KRX regular session: weekdays 09:00-15:30, local system clock. Doesn't know about
// market holidays -- orders on a holiday will still get rejected by KIS itself, just
// after wasting a scan's worth of API calls first.
static bool isMarketOpen() {
    std::time_t t = std::time(nullptr);
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    if (tmBuf.tm_wday == 0 || tmBuf.tm_wday == 6) return false; // 일/토
    int minutes = tmBuf.tm_hour * 60 + tmBuf.tm_min;
    return minutes >= 9 * 60 && minutes <= 15 * 60 + 30;
}

static void log(const std::string& msg) {
    std::string line = "[" + timestamp() + "] " + msg;
    std::cout << line << std::endl;
    std::ofstream f("trading.log", std::ios::app);
    f << line << "\n";
}

// One line per completed trade -- a clean audit trail separate from the noisy scan log.
static void logTrade(const std::string& side, const std::string& code, const std::string& name,
                      int qty, double price, double feeRate, double taxRate, double netProfit) {
    std::ofstream f("trades.log", std::ios::app);
    f << timestamp() << "," << side << "," << code << "," << name << "," << qty << ","
      << price << "," << feeRate << "," << taxRate << "," << netProfit << "\n";
}

// Local record of order ids that have been placed (paper/live) but not yet confirmed
// resolved (filled, or its unfilled remainder cancelled). KIS's own "list pending orders"
// endpoint doesn't work on paper accounts (confirmed live), so this is the only way to
// recover and clean up an order that outlives the run that placed it -- e.g. the process
// gets killed mid-fill-confirmation-poll. Plain one-id-per-line text; nothing fancier needed.
static const char* kPendingLedgerFile = "pending_orders.txt";

static void ledgerAppend(const std::string& odno) {
    std::ofstream f(kPendingLedgerFile, std::ios::app);
    f << odno << "\n";
}

static void ledgerRemove(const std::string& odno) {
    std::ifstream in(kPendingLedgerFile);
    if (!in) return;
    std::vector<std::string> remaining;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line != odno) remaining.push_back(line);
    }
    in.close();
    std::ofstream out(kPendingLedgerFile, std::ios::trunc);
    for (auto& l : remaining) out << l << "\n";
}

static std::vector<std::string> ledgerLoad() {
    std::vector<std::string> result;
    std::ifstream in(kPendingLedgerFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) result.push_back(line);
    }
    return result;
}

static std::string krw(double v) {
    return std::to_string((long long)std::llround(v)) + "원";
}

static std::string signalStr(Signal s) {
    return s == Signal::Buy ? "BUY" : s == Signal::Sell ? "SELL" : "HOLD";
}

// Crude mapping from a keyword-count sentiment score to a pseudo-probability of the
// trade working out: each net keyword hit nudges it 5%, clamped to a sane range. This
// is a heuristic, not a calibrated statistical estimate -- there's no backtest behind it.
static double probabilityFromSentiment(double sentiment) {
    return std::clamp(0.5 + sentiment * 0.05, 0.05, 0.95);
}

struct Position {
    std::string code, name;
    int qty = 0;
    double avgBuyPrice = 0.0;
    double lastKnownPrice = 0.0;
    std::vector<double> baseCloses; // historical closes for `code`, not including today
    int topUps = 0; // number of buys into this position after the one that opened it
};

struct SharedState {
    std::mutex mtx;
    std::map<std::string, Position> positions; // keyed by code -- multiple concurrent holdings
};

// Rewrites holdings.txt every 5 seconds regardless of poll_seconds, so "what does the
// bot hold right now" is always answerable by just looking at a file.
static void statusWriterLoop(SharedState* shared, std::atomic<bool>* stop) {
    while (!stop->load()) {
        {
            std::lock_guard<std::mutex> lock(shared->mtx);
            std::ofstream f("holdings.txt", std::ios::trunc);
            f << "갱신 시각: " << timestamp() << "\n";
            if (shared->positions.empty()) {
                f << "보유 종목 없음 (스캔 중)\n";
            } else {
                for (auto& [code, pos] : shared->positions) {
                    double pnl = (pos.lastKnownPrice - pos.avgBuyPrice) * pos.qty;
                    f << "---\n";
                    f << "보유 종목: " << pos.name << "(" << pos.code << ")\n";
                    f << "수량: " << pos.qty << "주\n";
                    f << "평단가: " << krw(pos.avgBuyPrice) << "\n";
                    f << "현재가: " << krw(pos.lastKnownPrice) << "\n";
                    f << "평가손익(세전): " << krw(pnl) << "\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8); // source strings are UTF-8; console defaults to the system codepage otherwise

    std::ifstream cfgFile("config.json");
    if (!cfgFile) {
        std::cerr << "config.json not found. Copy config.example.json and fill in your credentials.\n";
        return 1;
    }
    json cfg;
    try {
        cfg = json::parse(cfgFile, /*cb=*/nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        std::cerr << "config.json is empty or not valid JSON: " << e.what() << "\n";
        return 1;
    }

    // mode: "mock" (offline synthetic data, no API/account at all)
    //     | "sim"  (real KIS market data, but orders fill locally -- no 모의투자 signup needed)
    //     | "paper" (KIS 모의투자, real order-simulation server)
    //     | "live" (KIS real trading, real money)
    std::string mode = cfg.value("mode", "mock");
    // mock/sim have no real KIS account to query a balance from, so they track a local
    // virtual cash ledger seeded from this. paper/live ignore it -- they query the real
    // account balance via getBuyableCash() instead.
    double initialCash = cfg.value("initial_cash", 10000000.0);
    std::unique_ptr<IBroker> client;
    if (mode == "mock") {
        client = std::make_unique<MockBroker>(cfg.value("mock_start_price", 70000.0), initialCash);
    } else if (mode == "sim") {
        client = std::make_unique<SimBroker>(cfg.at("appkey"), cfg.at("appsecret"), initialCash);
    } else {
        client = std::make_unique<KisClient>(cfg.at("appkey"), cfg.at("appsecret"),
                                              cfg.at("cano"), cfg.at("acnt_prdt_cd"),
                                              mode == "paper");
    }

    int watchlistSize = cfg.value("watchlist_size", 5);
    // Only the top `deep_scan_limit` candidates by 전일대비율(dayChangePct, free from the
    // ranking call) get the expensive per-candidate daily-close fetch; a golden cross is
    // far more likely on a stock already trending up today. Defaults to watchlistSize
    // (no behavior change) -- lower it to speed up scans over large watchlists.
    int deepScanLimit = cfg.value("deep_scan_limit", watchlistSize);
    // Max concurrent distinct holdings. Raising this diversifies the portfolio but also
    // lengthens every poll cycle -- each held position gets its own price+news fetch
    // (apiPause-spaced) in addition to the scan itself.
    int maxPositions = cfg.value("max_positions", 3);
    // Each new buy (new position or top-up) spends at most this fraction of whatever cash
    // is available *at that moment* -- keeps a single trade from spending the whole
    // account and leaves room for later top-ups/diversification.
    double positionCashFraction = cfg.value("position_cash_fraction", 0.3);
    // Two caps against one candidate winning "best EV" forever and slowly absorbing the
    // whole account through repeated top-ups (its BUY signal can legitimately stay lit for
    // many consecutive polls -- that's not a bug, it just means the crossover condition is
    // still true, so something else has to stop it from monopolizing the portfolio):
    // stop topping up a symbol after this many top-ups (the opening buy doesn't count)...
    int maxTopupsPerSymbol = cfg.value("max_topups_per_symbol", 3);
    // ...and never let one symbol's cost basis exceed this fraction of total account value
    // (buyable cash + all positions marked at last known price).
    double maxPositionValueFraction = cfg.value("max_position_value_fraction", 0.5);
    // KIS accepting an order (a valid ODNO) only means it was submitted, not that it
    // matched -- an illiquid stock's market order can sit unfilled indefinitely. After
    // paper/live orders we poll the account's real holdings for up to this long, 2s apart,
    // to confirm an actual fill before touching position state or logging a trade.
    int fillConfirmTimeoutSeconds = cfg.value("fill_confirm_timeout_seconds", 10);
    int shortPeriod = cfg.value("sma_short", 5);
    int longPeriod = cfg.value("sma_long", 20);
    int pollSeconds = cfg.value("poll_seconds", 60);
    double feeRate = cfg.value("fee_rate", 0.00015);       // 매매수수료, 매수/매도 양쪽 -- 실제 요율은 증권사/상품에 따라 다름, 확인 필요
    double taxRate = cfg.value("tax_rate", 0.0018);        // 증권거래세, 매도 시에만 -- 시장/시기에 따라 바뀌므로 확인 필요
    double takeProfitPct = cfg.value("take_profit_pct", 0.02); // 수수료/세금 차감 후 이 이상 순이익이면 매도
    double badNewsThreshold = cfg.value("bad_news_sentiment_threshold", -2.0); // 보유 종목 뉴스감성이 이 이하면 즉시 매도
    std::vector<std::string> newsFeeds = cfg.value("news_feeds", std::vector<std::string>{
        "https://www.mk.co.kr/rss/50200011/",
        "https://www.hankyung.com/feed/economy",
        "https://www.yna.co.kr/rss/economy.xml",
    });
    NewsCrawler news(newsFeeds);

    log("starting trading bot: watchlist=" + std::to_string(watchlistSize) + " sma(" +
        std::to_string(shortPeriod) + "," + std::to_string(longPeriod) + ") take_profit=" +
        std::to_string(takeProfitPct * 100) + "% mode=" + mode);

    try {
        client->authenticate();
    } catch (const std::exception& e) {
        log(std::string("authentication failed, check appkey/appsecret in config.json: ") + e.what());
        return 1;
    }
    if (mode == "mock") log("using local mock broker (no network, no account)");
    else if (mode == "sim") log("authenticated with KIS API (live quotes, orders fill locally, no real money)");
    else log("authenticated with KIS API");

    if (mode != "mock") {
        try {
            log("매수가능금액: " + krw(client->getBuyableCash()));
        } catch (const std::exception& e) {
            log(std::string("매수가능금액 조회 실패: ") + e.what());
        }
    }

    // KIS (especially 모의투자) rate-limits to roughly 1 call/sec; space out requests generously.
    // Mock mode is local computation, not a real API, so there's nothing to pace.
    const auto apiPause = std::chrono::milliseconds(mode == "mock" ? 0 : 1100);

    SharedState shared;

    // KIS (especially 모의투자) rate-limits aggressively (EGW00201, "초당 거래건수를
    // 초과하였습니다") -- transient, resolves on its own within a couple seconds. Retry
    // indefinitely rather than giving up, since this call runs once at startup and the
    // trading loop must not start against a stale/empty view of the account.
    auto retryUntilSuccess = [&](auto&& fn, const std::string& what) {
        while (true) {
            try {
                return fn();
            } catch (const std::exception& e) {
                log(what + " 실패, 재시도: " + e.what());
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
    };

    // Orders from a previous run that never got confirmed (see fill-confirmation below)
    // can still be sitting unfilled in the account -- cancel them before doing anything
    // else, so a restart doesn't start with old orders lurking that could fill later on
    // their own and silently create a position the bot never decided to open. KIS has no
    // API to *list* pending orders on a paper account (confirmed live: rejected outright),
    // so the local ledger of "orders this bot placed but never resolved" is the only way
    // to know what to clean up there; live accounts get that cross-checked against the
    // real listing API too, in case something outside this ledger (e.g. a manual order)
    // is also outstanding.
    if (mode == "paper" || mode == "live") {
        auto toCancel = ledgerLoad();
        if (mode == "live") {
            auto listed = retryUntilSuccess([&] { return client->getPendingOrders(); }, "미체결 주문 조회");
            std::this_thread::sleep_for(apiPause);
            for (auto& order : listed)
                if (std::find(toCancel.begin(), toCancel.end(), order.odno) == toCancel.end())
                    toCancel.push_back(order.odno);
        }

        if (toCancel.empty()) {
            log("취소할 미체결 주문 없음");
        } else {
            log("미체결 주문 " + std::to_string(toCancel.size()) + "건 취소 시도...");
            for (auto& odno : toCancel) {
                bool cancelled = false;
                for (int attempt = 0; attempt < 5 && !cancelled; attempt++) {
                    try {
                        client->cancelOrder(odno);
                        cancelled = true;
                    } catch (const std::exception& e) {
                        log("  주문번호 " + odno + " 취소 시도 실패(" + std::to_string(attempt + 1) +
                            "/5): " + e.what());
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                }
                std::this_thread::sleep_for(apiPause);
                if (cancelled) {
                    log("  취소: 주문번호 " + odno);
                    ledgerRemove(odno);
                } else {
                    log("  주문번호 " + odno + " 취소 최종 실패 -- 이미 체결/취소됐을 수 있음, "
                        "장부에는 남겨두고 다음 실행에서 재시도");
                }
            }
        }
    }

    // The bot keeps no local persistence -- a restart otherwise starts from a blank
    // `positions` map even though the real KIS account still holds whatever a previous
    // run bought. Rebuild in-memory positions from the account itself before the main
    // loop starts, so a restart doesn't orphan (stop monitoring/selling) existing holdings.
    if (mode == "paper" || mode == "live") {
        auto holdings = retryUntilSuccess([&] { return client->getHoldings(); }, "실계좌 보유 종목 조회");
        std::this_thread::sleep_for(apiPause);
        if (holdings.empty()) {
            log("실계좌 보유 종목 없음 (빈 상태로 시작)");
        } else {
            log("실계좌 보유 종목 " + std::to_string(holdings.size()) + "개 발견 -- 포지션 복원 중...");
            for (auto& h : holdings) {
                std::string label = h.name + "(" + h.code + ")";
                auto closes = retryUntilSuccess([&] { return client->getDailyCloses(h.code, longPeriod + 5); },
                                                 "  " + label + " 일봉 조회");
                std::this_thread::sleep_for(apiPause);
                Position p;
                p.code = h.code;
                p.name = h.name;
                p.qty = h.qty;
                p.avgBuyPrice = h.avgBuyPrice;
                p.lastKnownPrice = h.avgBuyPrice;
                p.baseCloses = closes;
                { std::lock_guard<std::mutex> lock(shared.mtx); shared.positions[h.code] = p; }
                log("  복원: " + label + " " + std::to_string(h.qty) + "주, 평단가 " + krw(h.avgBuyPrice));
            }
        }
    }

    std::atomic<bool> stopStatusWriter{false};
    std::thread statusThread(statusWriterLoop, &shared, &stopStatusWriter);

    // Only paper/live actually submit orders to KIS, which rejects everything outside
    // regular hours ("모의투자 장종료 입니다") -- gate on it so a closed market doesn't
    // burn a full scan's worth of API calls (and pacing time) on a guaranteed rejection.
    bool needsMarketHours = (mode == "paper" || mode == "live");

    // Only meaningful for paper/live -- mock/sim fill synchronously inside
    // placeMarketOrder itself, and their getHoldings() always returns empty (their
    // "account" is just this process's memory, nothing external to poll). Confirms
    // whether an order actually matched (not just got accepted) by polling the real
    // account's held qty for `code` every 2s until it reaches `targetQty` or time runs
    // out. Returns the qty actually observed (may equal, be between, or still equal the
    // pre-order qty -- caller interprets full/partial/no fill from that), or -1 if every
    // single poll's query itself failed (network trouble, not a fill-status answer).
    auto pollForQty = [&](const std::string& code, int targetQty, int timeoutSeconds) -> int {
        int observed = -1;
        for (int elapsed = 0; elapsed < timeoutSeconds; elapsed += 2) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            try {
                auto holdings = client->getHoldings();
                observed = 0;
                for (auto& h : holdings) {
                    if (h.code == code) { observed = h.qty; break; }
                }
                if (observed == targetQty) return observed;
            } catch (const std::exception&) {
                // transient query failure -- leave `observed` as-is and try again next tick
            }
        }
        return observed;
    };

    // Shared by Phase A (dead-cross/take-profit/bad-news sells) and Phase B (rotation
    // evictions) -- places the sell order, confirms the fill (paper/live), logs it, and
    // updates/removes the position + frees cash for whatever Phase B decides to do next.
    auto sellPosition = [&](const Position& p, double current, const std::string& reason) {
        auto odno = client->placeMarketOrder(p.code, IBroker::Side::Sell, p.qty, feeRate, taxRate);
        std::string label = p.name + "(" + p.code + ")";
        bool tracked = (mode == "paper" || mode == "live");
        if (tracked) ledgerAppend(odno);

        int remainingQty = 0; // mock/sim: synchronous, always fully filled
        if (tracked) {
            int observed = pollForQty(p.code, 0, fillConfirmTimeoutSeconds);
            remainingQty = observed < 0 ? p.qty : observed; // couldn't confirm -> assume unsold
        }
        int soldQty = p.qty - remainingQty;

        if (tracked) {
            if (remainingQty > 0) {
                // Anything still unfilled after the confirmation window doesn't get left
                // dangling -- cancel it now rather than letting it sit and maybe fill
                // asynchronously later, invisibly to whatever the bot decides next.
                try {
                    client->cancelOrder(odno);
                    ledgerRemove(odno);
                } catch (const std::exception& e) {
                    log("  " + label + " 잔여 미체결분 취소 실패(다음 실행에서 재시도): " + e.what());
                }
            } else {
                ledgerRemove(odno);
            }
        }

        if (soldQty <= 0) {
            log("매도 미체결: " + label + " " + std::to_string(p.qty) + "주 주문했으나 " +
                std::to_string(fillConfirmTimeoutSeconds) + "초 내 체결 확인 안 됨 (주문번호 " + odno +
                "), 포지션 유지 -- 다음 poll에서 다시 판단");
            return;
        }

        double netPct = netProfitPct(p.avgBuyPrice, current, feeRate, taxRate);
        double netProfit = netPct * p.avgBuyPrice * soldQty;
        std::string fillNote = soldQty < p.qty
            ? " (부분체결 " + std::to_string(soldQty) + "/" + std::to_string(p.qty) + "주)" : "";
        log("<<< 매도 체결: " + label + " " + std::to_string(soldQty) + "주 @ " + krw(current) +
            " (주문번호 " + odno + ", 사유 " + reason + ")" + fillNote +
            ", 수수료/세금 차감 순손익 " + krw(netProfit));
        logTrade("SELL", p.code, p.name, soldQty, current, feeRate, taxRate, netProfit);

        std::lock_guard<std::mutex> lock(shared.mtx);
        if (soldQty >= p.qty) {
            shared.positions.erase(p.code);
        } else {
            auto it = shared.positions.find(p.code);
            if (it != shared.positions.end()) it->second.qty = remainingQty;
        }
    };

    while (true) {
        try {
            if (needsMarketHours && !isMarketOpen()) {
                log("정규장 시간(평일 09:00~15:30) 외 -- 스캔 건너뜀");
                std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
                continue;
            }

            // Phase A: refresh every held position, sell on dead-cross/take-profit/bad-news.
            // Positions that survive get their freshly computed EV cached in heldEv so a
            // Phase B rotation decision can reuse it instead of re-fetching price/news.
            std::vector<Position> heldSnapshot;
            { std::lock_guard<std::mutex> lock(shared.mtx);
              for (auto& [code, p] : shared.positions) heldSnapshot.push_back(p); }

            std::map<std::string, double> heldEv;
            for (auto& p : heldSnapshot) {
                std::string label = p.name + "(" + p.code + ")";
                try {
                    double current = client->getCurrentPrice(p.code);
                    std::this_thread::sleep_for(apiPause);
                    auto closes = p.baseCloses;
                    closes.push_back(current);

                    Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
                    double pnl = (current - p.avgBuyPrice) * p.qty;
                    double netPct = netProfitPct(p.avgBuyPrice, current, feeRate, taxRate);

                    // Held stock might fall out of the scan's watchlist entirely (it's ranked
                    // by volume, not by "do we own it") -- so don't rely on the scan's news
                    // pool for it. Search by name directly instead, every poll.
                    double newsSentiment = 0.0;
                    if (mode != "mock") {
                        auto heldNews = NewsCrawler::searchHeadlines(p.name);
                        std::this_thread::sleep_for(apiPause);
                        newsSentiment = NewsCrawler::scoreSentiment(heldNews, p.name);
                    }
                    bool badNews = mode != "mock" && newsSentiment <= badNewsThreshold;

                    log(label + " 현재가=" + krw(current) + " 시그널=" + signalStr(sig) + " 보유=" +
                        std::to_string(p.qty) + "주, 평단가 " + krw(p.avgBuyPrice) + ", 평가손익 " + krw(pnl) +
                        ", 순손익률 " + std::to_string(netPct * 100) + "%" +
                        (mode != "mock" ? ", 뉴스감성 " + std::to_string(newsSentiment) : ""));

                    { std::lock_guard<std::mutex> lock(shared.mtx);
                      auto it = shared.positions.find(p.code);
                      if (it != shared.positions.end()) it->second.lastKnownPrice = current; }

                    bool takeProfitHit = netPct >= takeProfitPct;
                    if (sig == Signal::Sell || takeProfitHit || badNews) {
                        std::string reason = badNews ? "악재" : takeProfitHit ? "익절" : "데드크로스";
                        sellPosition(p, current, reason);
                    } else {
                        double probability = probabilityFromSentiment(newsSentiment);
                        double gain = current * takeProfitPct;
                        heldEv[p.code] = gain * probability;
                    }
                } catch (const std::exception& e) {
                    log(label + " 모니터링 실패: " + e.what());
                }
            }

            // Phase B: scan for this cycle's single best opportunity and act on it -- top up
            // an existing position, open a new one if there's room, or evict the
            // worst-performing holding when a clearly better candidate needs the room.
            log("종목 스캔 중 (거래량 상위 " + std::to_string(watchlistSize) + "개)...");
            auto candidates = client->getTopVolumeStocks(watchlistSize);
            std::this_thread::sleep_for(apiPause);
            if ((int)candidates.size() < watchlistSize) {
                log("참고: " + std::to_string(candidates.size()) + "개만 확보됨 (KIS 거래량순위 API는 세그먼트당 "
                    "최대 약 30개까지만 주고, 3개 세그먼트를 합쳐도 최대 약 90개 -- 거기서 레버리지/인버스 "
                    "ETF·ETN을 필터링으로 뺀 나머지만 후보가 됨. watchlist_size를 더 키워도 이 이상은 못 늘어남)");
            }

            // Cheap pre-filter using data the ranking call already gave us for free:
            // only the top `deepScanLimit` by today's % change get the expensive
            // per-candidate daily-close fetch (a golden cross is far more likely on a
            // stock already trending up today than one that's flat or down).
            if ((int)candidates.size() > deepScanLimit) {
                std::sort(candidates.begin(), candidates.end(),
                          [](const StockInfo& a, const StockInfo& b) { return a.dayChangePct > b.dayChangePct; });
                log("전일대비율 기준 상위 " + std::to_string(deepScanLimit) + "개만 정밀 분석 (" +
                    std::to_string(candidates.size() - deepScanLimit) + "개 스킵)");
                candidates.resize(deepScanLimit);
            }

            // mock mode stays fully offline -- no real HTTP calls, including news.
            auto headlines = (mode == "mock") ? std::vector<NewsItem>{} : news.fetchHeadlines();
            log("뉴스 " + std::to_string(headlines.size()) + "건 수집");

            struct BuyCandidate {
                std::string code, name;
                double current;
                double ev;
                std::vector<double> closes;
            };
            std::vector<BuyCandidate> buyCandidates;

            for (auto& c : candidates) {
                std::string label = c.name + "(" + c.code + ")";
                try {
                    // c.price came from the ranking call itself -- no separate quote needed.
                    auto closes = client->getDailyCloses(c.code, longPeriod + 5);
                    std::this_thread::sleep_for(apiPause);
                    closes.push_back(c.price);

                    Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
                    double sentiment = NewsCrawler::scoreSentiment(headlines, c.name);

                    if (sig == Signal::Buy) {
                        // 기댓값 = 얻을 이득 x 얻을 확률. 이득은 익절 목표치, 확률은 뉴스 감성에서 추정.
                        double probability = probabilityFromSentiment(sentiment);
                        double gain = c.price * takeProfitPct;
                        double ev = gain * probability;
                        log("  " + label + " 현재가=" + krw(c.price) + " 시그널=" + signalStr(sig) +
                            " 뉴스감성=" + std::to_string(sentiment) + " 기댓값=" + std::to_string(ev));
                        buyCandidates.push_back({c.code, c.name, c.price, ev, closes});
                    } else {
                        log("  " + label + " 현재가=" + krw(c.price) + " 시그널=" + signalStr(sig) +
                            " 뉴스감성=" + std::to_string(sentiment));
                    }
                } catch (const std::exception& e) {
                    log("  " + label + " 조회 실패: " + e.what());
                }
            }
            // BUY 시그널이 여러 개 떠도 사이클당 실제 매수는 하나만 실행하되, 기댓값 1등이
            // 상한(추가매수 횟수/투입비율/자금부족)에 막히면 2등, 3등으로 계속 넘어가며
            // 시도함 -- 안 그러면 1등 종목 하나가 계속 막혀있는 동안 다른 후보들이 전부
            // 무시돼서 max_positions로 분산할 기회 자체가 없어짐.
            std::sort(buyCandidates.begin(), buyCandidates.end(),
                      [](const BuyCandidate& a, const BuyCandidate& b) { return a.ev > b.ev; });

            if (buyCandidates.empty()) {
                log("매수 신호를 보이는 종목 없음, 다음 스캔까지 대기");
            } else {
                double buyableCash = client->getBuyableCash();
                std::this_thread::sleep_for(apiPause);
                bool evictedThisCycle = false; // at most one eviction per cycle, however many candidates we walk through
                bool acted = false;

                for (size_t ci = 0; ci < buyCandidates.size() && !acted; ci++) {
                    auto& cand = buyCandidates[ci];
                    std::string label = cand.name + "(" + cand.code + ")";

                    bool alreadyHeld;
                    int positionCount;
                    { std::lock_guard<std::mutex> lock(shared.mtx);
                      alreadyHeld = shared.positions.count(cand.code) > 0;
                      positionCount = (int)shared.positions.size(); }

                    bool canAct = alreadyHeld || positionCount < maxPositions;
                    if (!canAct) {
                        if (evictedThisCycle) {
                            log("이번 사이클에 이미 교체매도 1건 진행함, " + label + " 은(는) 다음 사이클로 미룸");
                            continue;
                        }
                        // At capacity with a brand-new symbol -- only make room by evicting
                        // the worst current holding if the candidate clearly beats it net of
                        // the cost of switching (sell fee+tax out, buy fee in). Candidates are
                        // EV-sorted, so if the best-EV one can't clear this bar, none of the
                        // weaker ones behind it can either -- but they may still be eligible
                        // via the alreadyHeld top-up path above, so keep walking the list.
                        if (heldEv.empty()) {
                            log("최대 보유 종목수(" + std::to_string(maxPositions) + ") 도달, 교체 판단할 보유 종목 없음 -- " +
                                label + " 건너뜀");
                            continue;
                        }
                        auto worstIt = std::min_element(heldEv.begin(), heldEv.end(),
                            [](const auto& a, const auto& b) { return a.second < b.second; });
                        Position worstPos;
                        bool found;
                        { std::lock_guard<std::mutex> lock(shared.mtx);
                          auto it = shared.positions.find(worstIt->first);
                          found = it != shared.positions.end();
                          if (found) worstPos = it->second; }
                        if (!found) continue;
                        double switchCost = worstPos.qty * worstPos.lastKnownPrice * (feeRate * 2 + taxRate);
                        if (cand.ev > worstIt->second + switchCost) {
                            log("교체매도 판단: " + worstPos.name + "(" + worstPos.code + ") 기댓값 " +
                                std::to_string(worstIt->second) + " < 신규후보 " + label + " 기댓값 " +
                                std::to_string(cand.ev) + " (교체비용 " + krw(switchCost) + " 반영)");
                            sellPosition(worstPos, worstPos.lastKnownPrice, "교체매도");
                            evictedThisCycle = true;
                            buyableCash = client->getBuyableCash(); // refreshed post-eviction
                            std::this_thread::sleep_for(apiPause);
                            canAct = true;
                        } else {
                            log("최대 보유 종목수 도달, 신규후보 " + label + " 기댓값(" + std::to_string(cand.ev) +
                                ")이 최저 보유종목 기댓값+교체비용(" +
                                std::to_string(worstIt->second + switchCost) + ")을 못 넘어 교체 안 함");
                            continue;
                        }
                    }

                    // Snapshot the existing position (if any) and total account value up
                    // front, so a symbol that keeps winning "best EV" cycle after cycle
                    // (a legitimately persistent BUY signal, not a bug) can't quietly
                    // absorb the whole account through unlimited top-ups.
                    Position existing;
                    bool existed;
                    double otherValue = 0.0;
                    { std::lock_guard<std::mutex> lock(shared.mtx);
                      for (auto& [code, p] : shared.positions) otherValue += p.qty * p.lastKnownPrice;
                      auto it = shared.positions.find(cand.code);
                      existed = it != shared.positions.end();
                      if (existed) existing = it->second; }

                    double unitCost = cand.current * (1 + feeRate);
                    // Spend at most `positionCashFraction` of whatever cash is available
                    // right now -- keeps one trade from spending the whole account and
                    // leaves room for later top-ups/diversification (never buy just 1
                    // share by default, but still fall back to 1 if that's all that fits).
                    int buyQty = (int)std::floor(positionCashFraction * buyableCash / unitCost);
                    if (buyQty < 1 && buyableCash >= unitCost) buyQty = 1;

                    if (existed && existing.topUps >= maxTopupsPerSymbol) {
                        log("추가매수 보류: " + label + " 종목당 추가매수 횟수 상한(" +
                            std::to_string(maxTopupsPerSymbol) + "회) 도달 -- 다음 후보로");
                        continue;
                    }
                    if (buyQty >= 1) {
                        double totalEquity = buyableCash + otherValue;
                        double currentPositionValue = existed ? existing.avgBuyPrice * existing.qty : 0.0;
                        double room = maxPositionValueFraction * totalEquity - currentPositionValue;
                        int roomQty = room > 0 ? (int)std::floor(room / unitCost) : 0;
                        if (roomQty < buyQty) {
                            log("종목당 총 투입비율 상한(" + std::to_string(maxPositionValueFraction * 100) +
                                "%) 근접: " + label + " 매수 수량 " + std::to_string(buyQty) +
                                " -> " + std::to_string(roomQty) + "주로 축소");
                            buyQty = roomQty;
                        }
                    }

                    if (buyQty < 1) {
                        log("자금 부족 또는 투입비율 상한으로 매수 불가: " + label + " 필요 " + krw(unitCost) +
                            ", 매수가능금액 " + krw(buyableCash) + " -- 다음 후보로");
                        continue;
                    }

                    auto odno = client->placeMarketOrder(cand.code, IBroker::Side::Buy, buyQty, feeRate, taxRate);
                    bool tracked = (mode == "paper" || mode == "live");
                    if (tracked) ledgerAppend(odno);

                    int filledQty = buyQty; // mock/sim: synchronous, always fully filled
                    if (tracked) {
                        int priorQty = existed ? existing.qty : 0;
                        int observed = pollForQty(cand.code, priorQty + buyQty, fillConfirmTimeoutSeconds);
                        filledQty = observed < 0 ? 0 : std::max(0, observed - priorQty);
                    }

                    if (tracked) {
                        if (filledQty < buyQty) {
                            // Whatever didn't fill within the window doesn't get left
                            // dangling -- cancel the remainder now.
                            try {
                                client->cancelOrder(odno);
                                ledgerRemove(odno);
                            } catch (const std::exception& e) {
                                log("  " + label + " 잔여 미체결분 취소 실패(다음 실행에서 재시도): " + e.what());
                            }
                        } else {
                            ledgerRemove(odno);
                        }
                    }

                    if (filledQty < 1) {
                        log("매수 미체결: " + label + " " + std::to_string(buyQty) + "주 주문했으나 " +
                            std::to_string(fillConfirmTimeoutSeconds) + "초 내 체결 확인 안 됨 (주문번호 " +
                            odno + "), 포지션 반영 안 함");
                    } else {
                        bool toppedUp = false;
                        {
                            std::lock_guard<std::mutex> lock(shared.mtx);
                            auto& p = shared.positions[cand.code];
                            if (p.qty > 0) {
                                p.avgBuyPrice = (p.avgBuyPrice * p.qty + cand.current * filledQty) / (p.qty + filledQty);
                                p.qty += filledQty;
                                p.topUps += 1;
                                toppedUp = true;
                            } else {
                                p.code = cand.code;
                                p.name = cand.name;
                                p.qty = filledQty;
                                p.avgBuyPrice = cand.current;
                                p.baseCloses = std::vector<double>(cand.closes.begin(), cand.closes.end() - 1);
                            }
                            p.lastKnownPrice = cand.current;
                        }
                        std::string fillNote = filledQty < buyQty
                            ? " (부분체결 " + std::to_string(filledQty) + "/" + std::to_string(buyQty) + "주)" : "";
                        log(">>> " + std::string(toppedUp ? "추가매수" : "매수") + " 체결: " + label + " " +
                            std::to_string(filledQty) + "주 @ " + krw(cand.current) + fillNote + " (주문번호 " +
                            odno + ", 기댓값 " + std::to_string(cand.ev) + ")");
                        logTrade("BUY", cand.code, cand.name, filledQty, cand.current, feeRate, 0.0, 0.0);
                    }
                    acted = true;
                }
            }
        } catch (const std::exception& e) {
            log(std::string("ERROR: ") + e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
    }
}
