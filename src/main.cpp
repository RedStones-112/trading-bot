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

    std::string code = cfg.at("code");
    int qty = cfg.value("qty", 1);
    int shortPeriod = cfg.value("sma_short", 5);
    int longPeriod = cfg.value("sma_long", 20);
    int pollSeconds = cfg.value("poll_seconds", 60);

    log("starting trading bot: code=" + code + " sma(" + std::to_string(shortPeriod) +
        "," + std::to_string(longPeriod) + ") mode=" + mode);

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
    const auto apiPause = std::chrono::milliseconds(1100);

    // Prefer a name given in config.json -- the KIS quote response doesn't reliably include
    // one (field varies by account/endpoint), so don't burn an extra API call guessing.
    std::string stockName = cfg.value("name", "");
    if (stockName.empty()) {
        std::this_thread::sleep_for(apiPause);
        try {
            stockName = client->getStockName(code);
        } catch (const std::exception& e) {
            log(std::string("could not fetch stock name, falling back to code: ") + e.what());
            stockName = code;
        }
    }
    std::string label = stockName + "(" + code + ")";

    // Daily closes only change once a trading day -- fetch them once at startup instead of
    // every poll, so each poll only needs a single getCurrentPrice() call.
    std::this_thread::sleep_for(apiPause);
    std::vector<double> baseCloses = client->getDailyCloses(code, longPeriod + 5);

    bool holding = cfg.value("start_holding", false);
    double avgBuyPrice = 0.0;

    while (true) {
        try {
            double current = client->getCurrentPrice(code);
            auto closes = baseCloses;
            closes.push_back(current); // treat live price as "today's" close for signal purposes

            Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
            std::string sigStr = sig == Signal::Buy ? "BUY" : sig == Signal::Sell ? "SELL" : "HOLD";

            std::string posStr = holding
                ? std::to_string(qty) + "주, 평단가 " + krw(avgBuyPrice) +
                  ", 평가손익 " + krw((current - avgBuyPrice) * qty)
                : "0주";
            log(label + " 현재가=" + krw(current) + " 시그널=" + sigStr + " 보유=" + posStr);

            if (sig == Signal::Buy && !holding) {
                auto odno = client->placeMarketOrder(code, IBroker::Side::Buy, qty);
                holding = true;
                avgBuyPrice = current;
                log(">>> 매수 체결: " + label + " " + std::to_string(qty) + "주 @ " + krw(current) +
                    " (주문번호 " + odno + ")");
            } else if (sig == Signal::Sell && holding) {
                auto odno = client->placeMarketOrder(code, IBroker::Side::Sell, qty);
                double pnl = (current - avgBuyPrice) * qty;
                log("<<< 매도 체결: " + label + " " + std::to_string(qty) + "주 @ " + krw(current) +
                    " (주문번호 " + odno + "), 손익 " + krw(pnl));
                holding = false;
                avgBuyPrice = 0.0;
            }
        } catch (const std::exception& e) {
            log(std::string("ERROR: ") + e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
    }
}
