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

int main() {
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

    bool holding = cfg.value("start_holding", false);

    while (true) {
        try {
            auto closes = client->getDailyCloses(code, longPeriod + 5);
            double current = client->getCurrentPrice(code);
            closes.push_back(current); // treat live price as "today's" close for signal purposes

            Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
            log("price=" + std::to_string(current) + " signal=" +
                (sig == Signal::Buy ? "BUY" : sig == Signal::Sell ? "SELL" : "HOLD") +
                " holding=" + (holding ? "yes" : "no"));

            if (sig == Signal::Buy && !holding) {
                auto odno = client->placeMarketOrder(code, IBroker::Side::Buy, qty);
                holding = true;
                log("BUY order placed, order_no=" + odno);
            } else if (sig == Signal::Sell && holding) {
                auto odno = client->placeMarketOrder(code, IBroker::Side::Sell, qty);
                holding = false;
                log("SELL order placed, order_no=" + odno);
            }
        } catch (const std::exception& e) {
            log(std::string("ERROR: ") + e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
    }
}
