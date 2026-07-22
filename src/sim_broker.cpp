#include "sim_broker.hpp"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {
std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}
}

SimBroker::SimBroker(std::string appkey, std::string appsecret)
    : kis_(std::move(appkey), std::move(appsecret), "", "", /*paperTrading=*/false) {}

std::string SimBroker::placeMarketOrder(const std::string& code, Side side, int qty) {
    double fillPrice = kis_.getCurrentPrice(code);
    std::string id = "SIM" + std::to_string(++orderSeq_);

    std::ofstream ledger("sim_fills.log", std::ios::app);
    ledger << timestamp() << "," << id << "," << code << ","
           << (side == Side::Buy ? "BUY" : "SELL") << "," << qty << "," << fillPrice << "\n";

    return id;
}
