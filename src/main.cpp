#include "broker.hpp"
#include "kis_client.hpp"
#include "mock_broker.hpp"
#include "sim_broker.hpp"
#include "strategy.hpp"
#include "../third_party/json.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
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

static void log(const std::string& msg) {
    std::string line = "[" + timestamp() + "] " + msg;
    std::cout << line << std::endl;
    std::ofstream f("trading.log", std::ios::app);
    f << line << "\n";
}

static std::string krw(double v) {
    return std::to_string((long long)std::llround(v)) + "원";
}

static std::string signalStr(Signal s) {
    return s == Signal::Buy ? "BUY" : s == Signal::Sell ? "SELL" : "HOLD";
}

struct Position {
    bool holding = false;
    std::string code, name;
    int qty = 0;
    double avgBuyPrice = 0.0;
    std::vector<double> baseCloses; // historical closes for `code`, not including today
};

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
    int qty = cfg.value("qty", 1);
    int shortPeriod = cfg.value("sma_short", 5);
    int longPeriod = cfg.value("sma_long", 20);
    int pollSeconds = cfg.value("poll_seconds", 60);

    log("starting trading bot: watchlist=" + std::to_string(watchlistSize) + " sma(" +
        std::to_string(shortPeriod) + "," + std::to_string(longPeriod) + ") mode=" + mode);

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

    Position pos;

    while (true) {
        try {
            if (!pos.holding) {
                log("종목 스캔 중 (거래량 상위 " + std::to_string(watchlistSize) + "개)...");
                auto candidates = client->getTopVolumeStocks(watchlistSize);
                std::this_thread::sleep_for(apiPause);

                std::string bestCode, bestName;
                double bestMomentum = -1e18;
                double bestCurrent = 0.0;
                std::vector<double> bestCloses;

                for (auto& c : candidates) {
                    std::string label = c.name + "(" + c.code + ")";
                    try {
                        auto closes = client->getDailyCloses(c.code, longPeriod + 5);
                        std::this_thread::sleep_for(apiPause);
                        double current = client->getCurrentPrice(c.code);
                        std::this_thread::sleep_for(apiPause);
                        closes.push_back(current);

                        Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
                        log("  " + label + " 현재가=" + krw(current) + " 시그널=" + signalStr(sig));

                        if (sig == Signal::Buy) {
                            double momentum = smaMomentum(closes, shortPeriod, longPeriod);
                            if (momentum > bestMomentum) {
                                bestMomentum = momentum;
                                bestCode = c.code;
                                bestName = c.name;
                                bestCurrent = current;
                                bestCloses = closes;
                            }
                        }
                    } catch (const std::exception& e) {
                        log("  " + label + " 조회 실패: " + e.what());
                    }
                }

                if (!bestCode.empty()) {
                    auto odno = client->placeMarketOrder(bestCode, IBroker::Side::Buy, qty);
                    pos.holding = true;
                    pos.code = bestCode;
                    pos.name = bestName;
                    pos.qty = qty;
                    pos.avgBuyPrice = bestCurrent;
                    pos.baseCloses = std::vector<double>(bestCloses.begin(), bestCloses.end() - 1);
                    log(">>> 매수 체결: " + bestName + "(" + bestCode + ") " + std::to_string(qty) +
                        "주 @ " + krw(bestCurrent) + " (주문번호 " + odno + ", 모멘텀 " +
                        std::to_string(bestMomentum) + ")");
                } else {
                    log("매수 신호를 보이는 종목 없음, 다음 스캔까지 대기");
                }
            } else {
                std::string label = pos.name + "(" + pos.code + ")";
                double current = client->getCurrentPrice(pos.code);
                auto closes = pos.baseCloses;
                closes.push_back(current);

                Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
                double pnl = (current - pos.avgBuyPrice) * pos.qty;
                log(label + " 현재가=" + krw(current) + " 시그널=" + signalStr(sig) + " 보유=" +
                    std::to_string(pos.qty) + "주, 평단가 " + krw(pos.avgBuyPrice) + ", 평가손익 " + krw(pnl));

                if (sig == Signal::Sell) {
                    auto odno = client->placeMarketOrder(pos.code, IBroker::Side::Sell, pos.qty);
                    log("<<< 매도 체결: " + label + " " + std::to_string(pos.qty) + "주 @ " + krw(current) +
                        " (주문번호 " + odno + "), 손익 " + krw(pnl));
                    pos = Position{};
                }
            }
        } catch (const std::exception& e) {
            log(std::string("ERROR: ") + e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
    }
}
