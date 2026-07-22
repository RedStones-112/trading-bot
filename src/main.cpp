#include "kis_client.hpp"
#include "strategy.hpp"
#include "../third_party/json.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <ctime>
#include <iomanip>
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

    KisClient client(cfg.at("appkey"), cfg.at("appsecret"),
                      cfg.at("cano"), cfg.at("acnt_prdt_cd"),
                      cfg.value("paper_trading", true));

    std::string code = cfg.at("code");
    int qty = cfg.value("qty", 1);
    int shortPeriod = cfg.value("sma_short", 5);
    int longPeriod = cfg.value("sma_long", 20);
    int pollSeconds = cfg.value("poll_seconds", 60);

    log("starting trading bot: code=" + code + " sma(" + std::to_string(shortPeriod) +
        "," + std::to_string(longPeriod) + ") paper=" + (cfg.value("paper_trading", true) ? "true" : "false"));

    try {
        client.authenticate();
    } catch (const std::exception& e) {
        log(std::string("authentication failed, check appkey/appsecret in config.json: ") + e.what());
        return 1;
    }
    log("authenticated with KIS API");

    bool holding = cfg.value("start_holding", false);

    while (true) {
        try {
            auto closes = client.getDailyCloses(code, longPeriod + 5);
            double current = client.getCurrentPrice(code);
            closes.push_back(current); // treat live price as "today's" close for signal purposes

            Signal sig = smaCrossSignal(closes, shortPeriod, longPeriod);
            log("price=" + std::to_string(current) + " signal=" +
                (sig == Signal::Buy ? "BUY" : sig == Signal::Sell ? "SELL" : "HOLD") +
                " holding=" + (holding ? "yes" : "no"));

            if (sig == Signal::Buy && !holding) {
                auto odno = client.placeMarketOrder(code, KisClient::Side::Buy, qty);
                holding = true;
                log("BUY order placed, order_no=" + odno);
            } else if (sig == Signal::Sell && holding) {
                auto odno = client.placeMarketOrder(code, KisClient::Side::Sell, qty);
                holding = false;
                log("SELL order placed, order_no=" + odno);
            }
        } catch (const std::exception& e) {
            log(std::string("ERROR: ") + e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(pollSeconds));
    }
}
