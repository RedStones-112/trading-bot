#include "broker.hpp"
#include "event_calendar.hpp"
#include "kis_client.hpp"
#include "mock_broker.hpp"
#include "news_crawler.hpp"
#include "sim_broker.hpp"
#include "stock_tags.hpp"
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

// Reconstructs today's cumulative realized P&L from trades.log so the daily loss limit
// survives a restart within the same calendar day (no separate persistence needed --
// trades.log already has everything, one row per completed trade).
static double loadTodaysRealizedPnl(const std::string& today) {
    std::ifstream in("trades.log");
    std::string line;
    double sum = 0.0;
    while (std::getline(in, line)) {
        if (line.size() < 10 || line.substr(0, 10) != today) continue;
        std::stringstream ss(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(ss, field, ',')) fields.push_back(field);
        if (fields.size() < 9 || fields[1] != "SELL") continue;
        try { sum += std::stod(fields[8]); } catch (const std::exception&) {}
    }
    return sum;
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

// v1 heuristic for lazily auto-tagging an unfamiliar stock (see stock_tags.hpp): count
// which of a small fixed list of common sector/theme words show up in headlines that
// also mention `stockName`, return the top few by hit count. Crude keyword co-occurrence,
// not real topic modeling -- same "heuristic, not NLP" spirit as scoreSentiment. Meant to
// be refined later (manually, or with the KRX 업종 field from getFundamentals when that's
// usable) -- flagged in PROGRESS.md as an approximation to revisit.
static std::vector<std::string> extractCooccurringKeywords(const std::vector<NewsItem>& headlines,
                                                             const std::string& stockName) {
    static const std::vector<std::string> knownThemes = {
        "반도체", "AI", "전기전자", "전기·전자", "바이오", "제약", "2차전지", "자동차", "조선",
        "화학", "금융", "건설", "에너지", "게임", "인터넷", "방산", "철강", "로봇", "우주항공"
    };
    std::map<std::string, int> counts;
    for (auto& item : headlines) {
        std::string text = item.title + " " + item.description;
        if (text.find(stockName) == std::string::npos) continue;
        for (auto& theme : knownThemes)
            if (text.find(theme) != std::string::npos) counts[theme]++;
    }
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::vector<std::string> result;
    for (size_t i = 0; i < sorted.size() && i < 3; i++) result.push_back(sorted[i].first);
    return result;
}

struct Position {
    std::string code, name;
    int qty = 0;
    double avgBuyPrice = 0.0;
    double lastKnownPrice = 0.0;
    std::vector<double> baseCloses; // historical closes for `code`, not including today
    std::vector<DailyBar> baseBars; // historical OHLCV for `code`, not including today -- volume-profile input
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
    double stopLossPct = cfg.value("stop_loss_pct", 0.03); // 수수료/세금 차감 후 이 이상 손실이면 즉시 매도
    // 고정비율 리스크 모델(fixed-fractional risk model) -- 매수 수량을 "이 트레이드가
    // stop_loss_pct에서 손절되면 총자산의 이 비율만큼만 잃는다"는 조건으로 역산함.
    // position_cash_fraction/max_position_value_fraction은 그 위에 안전판으로 계속 적용됨
    // (risk_per_trade_pct로 계산한 수량이 더 크면 그쪽으로 잘림).
    double riskPerTradePct = cfg.value("risk_per_trade_pct", 0.01);
    // 오늘 누적 실현손익(매도 체결분 합)이 총자산의 이 비율 이하로 내려가면 그날은 신규
    // 매수/추가매수/교체매도를 전부 멈춤 -- 보유 종목 모니터링/매도는 계속함(손절/익절은
    // 계속 작동, 새 리스크만 안 늚). trades.log에서 오늘 날짜분을 합산해 복원되므로
    // 재시작해도 같은 날이면 한도 계산이 끊기지 않음.
    double dailyLossLimitPct = cfg.value("daily_loss_limit_pct", 0.05);
    // 이 서킷브레이커 자체를 끄고 싶을 때(예: 백테스트/짧은 검증 실행)를 위한 on/off 스위치.
    bool dailyLossLimitEnabled = cfg.value("daily_loss_limit_enabled", true);
    std::vector<std::string> newsFeeds = cfg.value("news_feeds", std::vector<std::string>{
        "https://www.mk.co.kr/rss/50200011/",
        "https://www.hankyung.com/feed/economy",
        "https://www.yna.co.kr/rss/economy.xml",
    });
    // Optional -- free registration at developers.naver.com. Blank means Naver search is
    // silently skipped (logged once below, not per-scan, so a missing key doesn't look
    // identical to "no results today").
    std::string naverClientId = cfg.value("naver_client_id", "");
    std::string naverClientSecret = cfg.value("naver_client_secret", "");
    NewsCrawler news(newsFeeds, naverClientId, naverClientSecret);
    StockTagStore tagStore("stock_tags.json");
    // 배당일/법률·정치 이벤트 등 "날짜가 정해진" 재료 -- 무료 API로 안정적으로 커버가
    // 안 돼서(DART 배당, 선거/입법 일정 전부 별도 키/스크래핑 필요) 수동 큐레이션 파일로
    // 둠. events.json.example 참고, 파일 없으면 그냥 빈 목록(배수 항상 1.0).
    auto eventCalendar = loadEventCalendar("events.json");
    log("이벤트 캘린더 " + std::to_string(eventCalendar.size()) + "건 로드");

    log("starting trading bot: watchlist=" + std::to_string(watchlistSize) + " sma(" +
        std::to_string(shortPeriod) + "," + std::to_string(longPeriod) + ") take_profit=" +
        std::to_string(takeProfitPct * 100) + "% mode=" + mode);
    if (naverClientId.empty() || naverClientSecret.empty())
        log("네이버뉴스 비활성화: naver_client_id/naver_client_secret 미설정");

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

    // Daily loss circuit breaker state -- restored from trades.log so a restart on the
    // same calendar day doesn't reset the counter and let more risk back in.
    std::string todayDate = timestamp().substr(0, 10);
    double todayRealizedPnl = loadTodaysRealizedPnl(todayDate);
    if (todayRealizedPnl != 0.0)
        log("오늘(" + todayDate + ") 누적 실현손익 복원: " + krw(todayRealizedPnl));
    auto rolloverIfNewDay = [&]() {
        std::string nowDate = timestamp().substr(0, 10);
        if (nowDate != todayDate) {
            todayDate = nowDate;
            todayRealizedPnl = 0.0;
            log("날짜 변경 감지 (" + todayDate + ") -- 일일 손실 한도 카운터 초기화");
        }
    };

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
                auto bars = retryUntilSuccess([&] { return client->getDailyBars(h.code, longPeriod + 5); },
                                              "  " + label + " 일봉 조회");
                std::this_thread::sleep_for(apiPause);
                Position p;
                p.code = h.code;
                p.name = h.name;
                p.qty = h.qty;
                p.avgBuyPrice = h.avgBuyPrice;
                p.lastKnownPrice = h.avgBuyPrice;
                for (auto& bar : bars) p.baseCloses.push_back(bar.close);
                p.baseBars = bars;
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

    // Shared by Phase A (dead-cross/take-profit/stop-loss sells) and Phase B (rotation
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
        todayRealizedPnl += netProfit;

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
            rolloverIfNewDay();

            if (needsMarketHours && !isMarketOpen()) {
                log("정규장 시간(평일 09:00~15:30) 외 -- 스캔 건너뜀");
                std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
                continue;
            }

            // Phase A: refresh every held position, sell on dead-cross/take-profit/stop-loss.
            // Positions that survive get their freshly computed EV cached in heldEv so a
            // Phase B rotation decision can reuse it instead of re-fetching price/technicals.
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

                    // 뉴스 감성 대신 기술적 신호(거래량/추세/매물대 위치)로 확률 추정 --
                    // 단타인데 뉴스는 장기 재료인 경우가 많아 오히려 손실 유발(사용자 확인,
                    // 2026-07-24). p.baseBars는 매수 시점에 캐시된 값(매 poll 재조회 안 함,
                    // baseCloses와 같은 방식).
                    double trendPct = p.baseCloses.empty() ? 0.0 : (current - p.baseCloses.front()) / p.baseCloses.front();
                    double belowRatio = belowPriceVolumeRatio(p.baseBars, current);

                    log(label + " 현재가=" + krw(current) + " 시그널=" + signalStr(sig) + " 보유=" +
                        std::to_string(p.qty) + "주, 평단가 " + krw(p.avgBuyPrice) + ", 평가손익 " + krw(pnl) +
                        ", 순손익률 " + std::to_string(netPct * 100) + "%" +
                        ", 매물대비중(하단) " + std::to_string(belowRatio) +
                        ", 추세 " + std::to_string(trendPct * 100) + "%");

                    { std::lock_guard<std::mutex> lock(shared.mtx);
                      auto it = shared.positions.find(p.code);
                      if (it != shared.positions.end()) it->second.lastKnownPrice = current; }

                    bool takeProfitHit = netPct >= takeProfitPct;
                    bool stopLossHit = netPct <= -stopLossPct;
                    if (sig == Signal::Sell || takeProfitHit || stopLossHit) {
                        std::string reason = takeProfitHit ? "익절" : stopLossHit ? "손절" : "데드크로스";
                        sellPosition(p, current, reason);
                    } else {
                        // 100.0 = "거래량 변화 없음"(vol_inrt 중립값) -- 보유 종목은 거래량순위
                        // 랭킹 데이터를 다시 조회하지 않으므로(스캔에서 벗어났을 수도 있어서
                        // 애초에 못 구함) 거래량 급증 신호 없이 매물대/추세만으로 판단.
                        double probability = probabilityFromTechnicals(100.0, trendPct, belowRatio);
                        double gain = current * takeProfitPct;
                        heldEv[p.code] = gain * probability;
                    }
                } catch (const std::exception& e) {
                    log(label + " 모니터링 실패: " + e.what());
                }
            }

            // Daily loss circuit breaker: if today's cumulative realized P&L has already
            // breached the limit, skip Phase B entirely (including the expensive scan) --
            // no new risk today, but Phase A above still runs every poll so existing
            // positions keep getting monitored/sold normally (take-profit/stop-loss/dead
            // cross/bad-news all still fire).
            double buyableCash = client->getBuyableCash();
            std::this_thread::sleep_for(apiPause);
            double otherValue = 0.0;
            { std::lock_guard<std::mutex> lock(shared.mtx);
              for (auto& [code, p] : shared.positions) otherValue += p.qty * p.lastKnownPrice; }
            double totalEquity = buyableCash + otherValue;

            if (dailyLossLimitEnabled && todayRealizedPnl <= -dailyLossLimitPct * totalEquity) {
                log("일일 손실 한도 도달 (오늘 실현손익 " + krw(todayRealizedPnl) + ", 한도 -" +
                    krw(dailyLossLimitPct * totalEquity) + ") -- 오늘은 신규 매수 중단, 모니터링/매도만 계속");
                std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
                continue;
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
                // EV 정교화용: probability는 감성 전용으로 그대로 두고(안 건드림), 나머지
                // 3개 신호는 gain에 곱하는 독립적인 배수로 적용 -- 하나의 clamp된
                // probability에 다 합치면 강한 신호가 다른 신호를 집어삼키거나 클램프에서
                // 정보가 사라지는 문제가 있어서(랭킹 동점 처리가 엉망이 됨), 이렇게 분리.
                double gain = 0.0, probability = 0.0;
                double per = 0.0, pbr = 0.0;
                std::vector<DailyBar> bars; // for Position.baseBars once bought (volume-profile input)
                std::string sector;
                double valuationMult = 1.0;  // ①: 동종업계 대비 저평가, buyCandidates 확정 후 2차 계산
                double momentumMult = 1.0;   // ③: 연관종목(태그) 모멘텀 전이
                double interestMult = 1.0;   // ⑤: 뉴스 언급량 + 거래량 급증률
                double eventMult = 1.0;      // 배당일/법률/정치 이벤트 캘린더 (수동 큐레이션)
            };
            std::vector<BuyCandidate> buyCandidates;

            for (auto& c : candidates) {
                std::string label = c.name + "(" + c.code + ")";
                try {
                    // c.price came from the ranking call itself -- no separate quote needed.
                    auto bars = client->getDailyBars(c.code, longPeriod + 5);
                    std::this_thread::sleep_for(apiPause);
                    std::vector<double> closes;
                    closes.reserve(bars.size() + 1);
                    for (auto& bar : bars) closes.push_back(bar.close);
                    closes.push_back(c.price);

                    Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);

                    if (sig == Signal::Buy) {
                        // 기댓값 = 얻을 이득 x 얻을 확률. 뉴스 감성 대신 기술적 신호로 확률
                        // 추정(사용자 확인, 2026-07-24): 단타 봇인데 뉴스는 장기 재료인 경우가
                        // 많아 오히려 손실을 유발했음 -- 거래량 급증률(이미 있는 c.volumeSurgePct),
                        // 최근 구간 추세, 매물대(가격대별 거래량) 위치로 교체. 매물대 위치가
                        // "실제로 거래된 물량이 지금 가격보다 위/아래 어디 쏠려있나"를 가장
                        // 직접적으로 보여줘서 가장 크게 반영됨(strategy.hpp 참고).
                        double trendPct = bars.empty() ? 0.0 : (c.price - bars.front().close) / bars.front().close;
                        double belowRatio = belowPriceVolumeRatio(bars, c.price);
                        double probability = probabilityFromTechnicals(c.volumeSurgePct, trendPct, belowRatio);
                        double gain = c.price * takeProfitPct;

                        // ① PER + 업종(fundamentals) -- BUY 후보에게만 조회(비용 통제).
                        // 밸류에이션 배수는 buyCandidates가 다 모인 뒤 동종업계 평균과
                        // 비교해서 계산(아래 2차 패스).
                        Fundamentals fund;
                        try {
                            fund = client->getFundamentals(c.code);
                            std::this_thread::sleep_for(apiPause);
                        } catch (const std::exception& e) {
                            log("  " + label + " 재무정보 조회 실패: " + e.what());
                        }

                        // ③ 태그 지연 채우기 -- 처음 보는 종목이면 업종 + 헤드라인
                        // co-occurrence로 휴리스틱 태그를 붙여서 저장(근사치, PROGRESS.md 참고).
                        const StockTags* existingTags = tagStore.find(c.code);
                        std::vector<std::string> tagsForC;
                        if (existingTags) {
                            tagsForC = existingTags->tags;
                        } else {
                            if (!fund.sector.empty()) tagsForC.push_back(fund.sector);
                            for (auto& kw : extractCooccurringKeywords(headlines, c.name))
                                if (std::find(tagsForC.begin(), tagsForC.end(), kw) == tagsForC.end())
                                    tagsForC.push_back(kw);
                            tagStore.set(c.code, StockTags{c.name, tagsForC, "", timestamp().substr(0, 10)});
                            if (!tagsForC.empty()) {
                                std::string joined;
                                for (auto& t : tagsForC) joined += (joined.empty() ? "" : ",") + t;
                                log("  " + label + " 휴리스틱 태그 부여: [" + joined + "]");
                            }
                        }

                        // ③ 모멘텀 전이 -- 같은 스캔의 다른 종목 중 오늘 이미 급등했고(임계값
                        // 이상 dayChangePct, 이미 공짜로 있음) 태그가 겹치는 종목이 있으면
                        // 그만큼 가중치를 더함. 태그를 모르는 종목은(아직 안 만난 종목)
                        // 기여분 없음 -- 오류가 아니라 정보 없음으로 취급.
                        const double kSurgeThresholdPct = 5.0; // "오늘 이미 오른" 기준, 튜닝 대상
                        const int kEventLookaheadDays = 14; // 이 안에 든 이벤트만 반영, 튜닝 대상
                        double momentumScore = 0.0;
                        if (!tagsForC.empty()) {
                            for (auto& other : candidates) {
                                if (other.code == c.code || other.dayChangePct < kSurgeThresholdPct) continue;
                                auto* otherTags = tagStore.find(other.code);
                                if (!otherTags) continue;
                                int overlap = 0;
                                for (auto& t : tagsForC)
                                    if (std::find(otherTags->tags.begin(), otherTags->tags.end(), t) !=
                                        otherTags->tags.end())
                                        overlap++;
                                if (overlap > 0) momentumScore += overlap * other.dayChangePct;
                            }
                        }
                        double momentumMult = std::clamp(1.0 + momentumScore * 0.002, 0.8, 1.3);

                        // ⑤ 관심도 -- 이번 스캔에서 이미 모은 헤드라인 풀에서 언급 횟수(단순
                        // 카운트, 롤링 추세는 2단계) + 거래량 급증률(vol_inrt, 100 = 전일과
                        // 동일, 그 이상이면 급증 -- 역시 이미 공짜로 있음).
                        int mentionCount = 0;
                        for (auto& item : headlines) {
                            std::string text = item.title + " " + item.description;
                            if (text.find(c.name) != std::string::npos) mentionCount++;
                        }
                        double interestMult = std::clamp(
                            1.0 + 0.03 * mentionCount + 0.0005 * (c.volumeSurgePct - 100.0), 0.8, 1.3);

                        // 이벤트 배수 -- 종목코드/업종·테마 태그/"ALL" 중 하나로 매칭되는
                        // 예정된 이벤트(배당일/법률/정치 일정)가 event_lookahead_days 이내면
                        // 날짜에 가까울수록 크게 반영(events.json.example 참고).
                        double eventMult = eventMultiplier(eventCalendar, c.code, tagsForC,
                                                            timestamp().substr(0, 10), kEventLookaheadDays);

                        log("  " + label + " 현재가=" + krw(c.price) + " 시그널=" + signalStr(sig) +
                            " 매물대비중(하단)=" + std::to_string(belowRatio) + " 추세=" +
                            std::to_string(trendPct * 100) + "% PER=" + std::to_string(fund.per) +
                            " PBR=" + std::to_string(fund.pbr) +
                            " 업종=" + (fund.sector.empty() ? "?" : fund.sector) + " 모멘텀×" +
                            std::to_string(momentumMult) + " 관심도×" + std::to_string(interestMult) +
                            "(언급 " + std::to_string(mentionCount) + "건, 거래량비 " +
                            std::to_string(c.volumeSurgePct) + "%) 이벤트×" + std::to_string(eventMult));

                        BuyCandidate cand;
                        cand.code = c.code; cand.name = c.name; cand.current = c.price;
                        cand.closes = closes; cand.bars = bars; cand.gain = gain; cand.probability = probability;
                        cand.per = fund.per; cand.pbr = fund.pbr; cand.sector = fund.sector;
                        cand.momentumMult = momentumMult; cand.interestMult = interestMult;
                        cand.eventMult = eventMult;
                        cand.ev = gain * probability; // 밸류에이션 배수 반영 전 임시값, 2차 패스에서 확정
                        buyCandidates.push_back(std::move(cand));
                    } else {
                        log("  " + label + " 현재가=" + krw(c.price) + " 시그널=" + signalStr(sig));
                    }
                } catch (const std::exception& e) {
                    log("  " + label + " 조회 실패: " + e.what());
                }
            }

            // ① 밸류에이션 배수 2차 패스 -- 후보들이 다 모여야 "동종업계 평균"을 낼 수 있음.
            // 같은 업종을 공유하는 다른 후보가 2개 이상(본인 포함 3개 이상)일 때만 적용 --
            // 그 미만이면 "평균"이라는 말이 무의미해서 배수 1.0(미적용).
            // PER(이익 기준)만 보면 "PER이 높다 = 비싸다"로 단정하게 되는데, 이익 기준과
            // 자산 기준(PBR)이 서로 반대로 갈리는 경우를 놓침 -- 예: 이익이 일시적으로
            // 눌려서 PER은 동종업계보다 높아 보이지만, 순자산 대비로는(PBR) 동종업계보다
            // 훨씬 싸게 거래되는 종목. 그래서 PER 비율과 PBR 비율을 각각 구해서(둘 다
            // "peer 평균 ÷ 본인", 크면 저평가) 평균낸 값을 최종 밸류에이션 배수로 씀 --
            // 한쪽 지표만으로 비싸다고 판정됐어도 다른 쪽이 충분히 싸면 상쇄됨.
            for (auto& cand : buyCandidates) {
                if (cand.sector.empty()) continue;
                std::vector<double> peerPers, peerPbrs;
                for (auto& other : buyCandidates) {
                    if (other.code == cand.code || other.sector != cand.sector) continue;
                    if (other.per > 0) peerPers.push_back(other.per);
                    if (other.pbr > 0) peerPbrs.push_back(other.pbr);
                }
                double ratioSum = 0.0;
                int ratioCount = 0;
                if (cand.per > 0 && peerPers.size() >= 2) {
                    double peerAvg = 0.0;
                    for (double p : peerPers) peerAvg += p;
                    peerAvg /= peerPers.size();
                    double perRatio = peerAvg / cand.per;
                    ratioSum += perRatio; ratioCount++;
                    log("  " + cand.name + "(" + cand.code + ") 동종업계(" + cand.sector + ") 평균PER " +
                        std::to_string(peerAvg) + " vs 본인 " + std::to_string(cand.per) + " -> PER비율 " +
                        std::to_string(perRatio));
                }
                if (cand.pbr > 0 && peerPbrs.size() >= 2) {
                    double peerAvg = 0.0;
                    for (double p : peerPbrs) peerAvg += p;
                    peerAvg /= peerPbrs.size();
                    double pbrRatio = peerAvg / cand.pbr;
                    ratioSum += pbrRatio; ratioCount++;
                    log("  " + cand.name + "(" + cand.code + ") 동종업계(" + cand.sector + ") 평균PBR " +
                        std::to_string(peerAvg) + " vs 본인 " + std::to_string(cand.pbr) + " -> PBR비율 " +
                        std::to_string(pbrRatio));
                }
                if (ratioCount == 0) {
                    log("  " + cand.name + "(" + cand.code + ") 동종업계(" + cand.sector +
                        ") PER/PBR 비교 대상 부족 -- 밸류에이션 배수 미적용");
                    continue;
                }
                cand.valuationMult = std::clamp(ratioSum / ratioCount, 0.8, 1.3);
                log("  " + cand.name + "(" + cand.code + ") 밸류에이션×" + std::to_string(cand.valuationMult) +
                    " (PER/PBR " + std::to_string(ratioCount) + "개 신호 평균)");
            }
            // 네 배수를 gain에 곱해 최종 기댓값 확정. probability는 감성 전용으로 안 건드림.
            for (auto& cand : buyCandidates) {
                double adjustedGain = cand.gain * cand.valuationMult * cand.momentumMult *
                                       cand.interestMult * cand.eventMult;
                cand.ev = adjustedGain * cand.probability;
                log("  " + cand.name + "(" + cand.code + ") 최종 기댓값=" + std::to_string(cand.ev) +
                    " (밸류에이션×" + std::to_string(cand.valuationMult) + " 모멘텀×" +
                    std::to_string(cand.momentumMult) + " 관심도×" + std::to_string(cand.interestMult) +
                    " 이벤트×" + std::to_string(cand.eventMult) + ")");
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
                // `buyableCash` was already fetched above for the daily-loss-limit check --
                // reused here (refreshed only if this cycle ends up evicting a position).
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
                    double totalEquity = buyableCash + otherValue;

                    // 고정비율 리스크 모델(fixed-fractional risk model): 이 트레이드가
                    // stop_loss_pct에서 손절되면 총자산의 risk_per_trade_pct만 잃도록 수량을
                    // 역산 -- 변동성/가격이 큰 종목일수록 더 적게 사게 됨. position_cash_fraction
                    // 은 그 위에 안전판으로 계속 적용(둘 중 더 작은 쪽으로 결정) -- 손절선이
                    // 아주 타이트하게 설정된 경우에도 한 트레이드가 계좌를 과도하게 잠식하지
                    // 않도록.
                    double riskBudget = totalEquity * riskPerTradePct;
                    double lossPerShare = cand.current * stopLossPct;
                    int riskQty = lossPerShare > 0 ? (int)std::floor(riskBudget / lossPerShare) : 0;
                    int cashFractionQty = (int)std::floor(positionCashFraction * buyableCash / unitCost);
                    int buyQty = std::min(riskQty, cashFractionQty);
                    if (buyQty < 1 && buyableCash >= unitCost) buyQty = 1;

                    if (existed && existing.topUps >= maxTopupsPerSymbol) {
                        log("추가매수 보류: " + label + " 종목당 추가매수 횟수 상한(" +
                            std::to_string(maxTopupsPerSymbol) + "회) 도달 -- 다음 후보로");
                        continue;
                    }
                    if (buyQty >= 1) {
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
                                p.baseBars = cand.bars;
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
