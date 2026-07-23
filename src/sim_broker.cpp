#include "sim_broker.hpp"
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

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

SimBroker::SimBroker(std::string appkey, std::string appsecret, double initialCash)
    : kis_(std::move(appkey), std::move(appsecret), "", "", /*paperTrading=*/false), cash_(initialCash) {}

std::string SimBroker::placeMarketOrder(const std::string& code, Side side, int qty,
                                         double feeRate, double taxRate) {
    double fillPrice = kis_.getCurrentPrice(code);
    if (side == Side::Buy) {
        double cost = qty * fillPrice * (1 + feeRate);
        if (cost > cash_) throw std::runtime_error("insufficient virtual cash: need " + std::to_string(cost) +
                                                     ", have " + std::to_string(cash_));
        cash_ -= cost;
    } else {
        cash_ += qty * fillPrice * (1 - feeRate - taxRate);
    }
    std::string id = "SIM" + std::to_string(++orderSeq_);

    std::ofstream ledger("sim_fills.log", std::ios::app);
    ledger << timestamp() << "," << id << "," << code << ","
           << (side == Side::Buy ? "BUY" : "SELL") << "," << qty << "," << fillPrice << "\n";

    return id;
}
