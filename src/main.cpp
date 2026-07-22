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
    bool holding = false;
    std::string code, name;
    int qty = 0;
    double avgBuyPrice = 0.0;
    double lastKnownPrice = 0.0;
    std::vector<double> baseCloses; // historical closes for `code`, not including today
};

struct SharedState {
    std::mutex mtx;
    Position pos;
};

// Rewrites holdings.txt every 5 seconds regardless of poll_seconds, so "what does the
// bot hold right now" is always answerable by just looking at a file.
static void statusWriterLoop(SharedState* shared, std::atomic<bool>* stop) {
    while (!stop->load()) {
        {
            std::lock_guard<std::mutex> lock(shared->mtx);
            std::ofstream f("holdings.txt", std::ios::trunc);
            f << "갱신 시각: " << timestamp() << "\n";
            if (shared->pos.holding) {
                double pnl = (shared->pos.lastKnownPrice - shared->pos.avgBuyPrice) * shared->pos.qty;
                f << "보유 종목: " << shared->pos.name << "(" << shared->pos.code << ")\n";
                f << "수량: " << shared->pos.qty << "주\n";
                f << "평단가: " << krw(shared->pos.avgBuyPrice) << "\n";
                f << "현재가: " << krw(shared->pos.lastKnownPrice) << "\n";
                f << "평가손익(세전): " << krw(pnl) << "\n";
            } else {
                f << "보유 종목 없음 (스캔 중)\n";
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
    json cfg = json::parse(cfgFile);

    // mode: "mock" (offline synthetic data, no API/account at all)
    //     | "sim"  (real KIS market data, but orders fill locally -- no 모의투자 signup needed)
    //     | "paper" (KIS 모의투자, real order-simulation server)
    //     | "live" (KIS real trading, real money)
    std::string mode = cfg.value("mode", "mock");
    std::unique_ptr<IBroker> client;
    if (mode == "mock") {
        client = std::make_unique<MockBroker>(cfg.value("mock_start_price", 70000.0));
    } else if (mode == "sim") {
        client = std::make_unique<SimBroker>(cfg.at("appkey"), cfg.at("appsecret"));
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
    int qty = cfg.value("qty", 1);
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

    // KIS (especially 모의투자) rate-limits to roughly 1 call/sec; space out requests generously.
    // Mock mode is local computation, not a real API, so there's nothing to pace.
    const auto apiPause = std::chrono::milliseconds(mode == "mock" ? 0 : 1100);

    SharedState shared;
    std::atomic<bool> stopStatusWriter{false};
    std::thread statusThread(statusWriterLoop, &shared, &stopStatusWriter);

    // Only paper/live actually submit orders to KIS, which rejects everything outside
    // regular hours ("모의투자 장종료 입니다") -- gate on it so a closed market doesn't
    // burn a full scan's worth of API calls (and pacing time) on a guaranteed rejection.
    bool needsMarketHours = (mode == "paper" || mode == "live");

    while (true) {
        try {
            if (needsMarketHours && !isMarketOpen()) {
                log("정규장 시간(평일 09:00~15:30) 외 -- 스캔 건너뜀");
                std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
                continue;
            }

            bool holding;
            { std::lock_guard<std::mutex> lock(shared.mtx); holding = shared.pos.holding; }

            if (!holding) {
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

                std::string bestCode, bestName;
                double bestEv = -1e18;
                double bestCurrent = 0.0;
                std::vector<double> bestCloses;

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
                            // 이 스캔에서 BUY 시그널이 여러 개 떠도 기댓값이 가장 높은 종목 하나만 매수함.
                            double probability = probabilityFromSentiment(sentiment);
                            double gain = c.price * takeProfitPct;
                            double ev = gain * probability;
                            log("  " + label + " 현재가=" + krw(c.price) + " 시그널=" + signalStr(sig) +
                                " 뉴스감성=" + std::to_string(sentiment) + " 기댓값=" + std::to_string(ev));
                            if (ev > bestEv) {
                                bestEv = ev;
                                bestCode = c.code;
                                bestName = c.name;
                                bestCurrent = c.price;
                                bestCloses = closes;
                            }
                        } else {
                            log("  " + label + " 현재가=" + krw(c.price) + " 시그널=" + signalStr(sig) +
                                " 뉴스감성=" + std::to_string(sentiment));
                        }
                    } catch (const std::exception& e) {
                        log("  " + label + " 조회 실패: " + e.what());
                    }
                }

                if (!bestCode.empty()) {
                    auto odno = client->placeMarketOrder(bestCode, IBroker::Side::Buy, qty);
                    {
                        std::lock_guard<std::mutex> lock(shared.mtx);
                        shared.pos.holding = true;
                        shared.pos.code = bestCode;
                        shared.pos.name = bestName;
                        shared.pos.qty = qty;
                        shared.pos.avgBuyPrice = bestCurrent;
                        shared.pos.lastKnownPrice = bestCurrent;
                        shared.pos.baseCloses = std::vector<double>(bestCloses.begin(), bestCloses.end() - 1);
                    }
                    log(">>> 매수 체결: " + bestName + "(" + bestCode + ") " + std::to_string(qty) +
                        "주 @ " + krw(bestCurrent) + " (주문번호 " + odno + ", 기댓값 " +
                        std::to_string(bestEv) + ")");
                    logTrade("BUY", bestCode, bestName, qty, bestCurrent, feeRate, 0.0, 0.0);
                } else {
                    log("매수 신호를 보이는 종목 없음, 다음 스캔까지 대기");
                }
            } else {
                std::string code, name;
                int posQty;
                double avgBuyPrice;
                std::vector<double> baseCloses;
                { std::lock_guard<std::mutex> lock(shared.mtx);
                  code = shared.pos.code; name = shared.pos.name; posQty = shared.pos.qty;
                  avgBuyPrice = shared.pos.avgBuyPrice; baseCloses = shared.pos.baseCloses; }

                std::string label = name + "(" + code + ")";
                double current = client->getCurrentPrice(code);
                auto closes = baseCloses;
                closes.push_back(current);

                Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
                double pnl = (current - avgBuyPrice) * posQty;
                double netPct = netProfitPct(avgBuyPrice, current, feeRate, taxRate);

                // Held stock might fall out of the scan's watchlist entirely (it's ranked
                // by volume, not by "do we own it") -- so don't rely on the scan's news pool
                // for it. Search by name directly instead, every poll, regardless of rank.
                double newsSentiment = 0.0;
                if (mode != "mock") {
                    auto heldNews = NewsCrawler::searchHeadlines(name);
                    newsSentiment = NewsCrawler::scoreSentiment(heldNews, name);
                }
                bool badNews = mode != "mock" && newsSentiment <= badNewsThreshold;

                log(label + " 현재가=" + krw(current) + " 시그널=" + signalStr(sig) + " 보유=" +
                    std::to_string(posQty) + "주, 평단가 " + krw(avgBuyPrice) + ", 평가손익 " + krw(pnl) +
                    ", 순손익률 " + std::to_string(netPct * 100) + "%" +
                    (mode != "mock" ? ", 뉴스감성 " + std::to_string(newsSentiment) : ""));

                { std::lock_guard<std::mutex> lock(shared.mtx); shared.pos.lastKnownPrice = current; }

                bool takeProfitHit = netPct >= takeProfitPct;
                if (sig == Signal::Sell || takeProfitHit || badNews) {
                    auto odno = client->placeMarketOrder(code, IBroker::Side::Sell, posQty);
                    double netProfit = netPct * avgBuyPrice * posQty;
                    std::string reason = badNews ? "악재" : takeProfitHit ? "익절" : "데드크로스";
                    log("<<< 매도 체결: " + label + " " + std::to_string(posQty) + "주 @ " + krw(current) +
                        " (주문번호 " + odno + ", 사유 " + reason +
                        "), 수수료/세금 차감 순손익 " + krw(netProfit));
                    logTrade("SELL", code, name, posQty, current, feeRate, taxRate, netProfit);
                    { std::lock_guard<std::mutex> lock(shared.mtx); shared.pos = Position{}; }
                }
            }
        } catch (const std::exception& e) {
            log(std::string("ERROR: ") + e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
    }
}
