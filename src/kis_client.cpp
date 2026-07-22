#include "kis_client.hpp"
#include "http_client.hpp"
#include "../third_party/json.hpp"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

using json = nlohmann::json;

namespace {
std::wstring toWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string dateOffset(int daysAgo) {
    std::time_t t = std::time(nullptr) - (std::time_t)daysAgo * 86400;
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y%m%d");
    return oss.str();
}
}

KisClient::KisClient(std::string appkey, std::string appsecret,
                      std::string cano, std::string acntPrdtCd, bool paperTrading)
    : appkey_(std::move(appkey)), appsecret_(std::move(appsecret)),
      cano_(std::move(cano)), acntPrdtCd_(std::move(acntPrdtCd)), paper_(paperTrading) {
    host_ = paper_ ? L"openapivts.koreainvestment.com" : L"openapi.koreainvestment.com";
    port_ = paper_ ? 29443 : 9443;
}

void KisClient::authenticate() {
    json body = {{"grant_type", "client_credentials"}, {"appkey", appkey_}, {"appsecret", appsecret_}};
    std::string headers = "Content-Type: application/json";
    auto resp = http::request(host_, port_, L"/oauth2/tokenP", "POST", headers, body.dump());
    if (resp.status != 200) throw std::runtime_error("auth failed: HTTP " + std::to_string(resp.status) + " " + resp.body);
    auto j = json::parse(resp.body);
    accessToken_ = j.at("access_token").get<std::string>();
}

std::string KisClient::hashKey(const std::string& jsonBody) {
    std::string headers = "Content-Type: application/json\r\n"
                           "appkey: " + appkey_ + "\r\n"
                           "appsecret: " + appsecret_;
    auto resp = http::request(host_, port_, L"/uapi/hashkey", "POST", headers, jsonBody);
    if (resp.status != 200) throw std::runtime_error("hashkey failed: HTTP " + std::to_string(resp.status) + " " + resp.body);
    return json::parse(resp.body).at("HASH").get<std::string>();
}

std::string KisClient::request(const std::string& path, const std::string& method,
                                const std::string& trId, const std::string& queryOrBody, bool isQuery) {
    if (accessToken_.empty()) throw std::runtime_error("call authenticate() first");

    std::string headers = "Content-Type: application/json\r\n"
                           "authorization: Bearer " + accessToken_ + "\r\n"
                           "appkey: " + appkey_ + "\r\n"
                           "appsecret: " + appsecret_ + "\r\n"
                           "tr_id: " + trId + "\r\n"
                           "custtype: P";

    std::wstring wpath = toWide(path);
    std::string body;
    if (isQuery) {
        wpath += L"?" + toWide(queryOrBody);
    } else {
        body = queryOrBody;
        headers += "\r\nhashkey: " + hashKey(body);
    }

    auto resp = http::request(host_, port_, wpath, method, headers, body);
    if (resp.status != 200) throw std::runtime_error("KIS API error: HTTP " + std::to_string(resp.status) + " " + resp.body);
    return resp.body;
}

double KisClient::getCurrentPrice(const std::string& code) {
    std::string query = "FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" + code;
    auto body = request("/uapi/domestic-stock/v1/quotations/inquire-price", "GET", "FHKST01010100", query, true);
    auto j = json::parse(body);
    return std::stod(j.at("output").at("stck_prpr").get<std::string>());
}

std::string KisClient::getStockName(const std::string& code) {
    std::string query = "FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" + code;
    auto body = request("/uapi/domestic-stock/v1/quotations/inquire-price", "GET", "FHKST01010100", query, true);
    auto j = json::parse(body);
    return j.at("output").value("hts_kor_isnm", code);
}

std::vector<StockInfo> KisClient::getTopVolumeStocks(int count) {
    // 거래량순위 (volume rank) -- candidate universe for "which stock looks best right now".
    std::string query =
        "FID_COND_MRKT_DIV_CODE=J&FID_COND_SCR_DIV_CODE=20171&FID_INPUT_ISCD=0000"
        "&FID_DIV_CLS_CODE=0&FID_BLNG_CLS_CODE=0&FID_TRGT_CLS_CODE=111111111"
        "&FID_TRGT_EXLS_CLS_CODE=000000000&FID_INPUT_PRICE_1=0&FID_INPUT_PRICE_2=0"
        "&FID_VOL_CNT=0&FID_INPUT_DATE_1=0";
    auto body = request("/uapi/domestic-stock/v1/quotations/volume-rank", "GET", "FHPST01710000", query, true);
    auto j = json::parse(body);

    // Leveraged/inverse ETFs & ETNs dominate raw volume rankings but decay over time and are
    // far riskier than ordinary shares for a plain SMA-crossover strategy -- exclude them.
    static const std::vector<std::string> etfMarkers = {
        "레버리지", "인버스", "KODEX", "TIGER", "KBSTAR", "SOL ", "ARIRANG", "KINDEX", "HANARO", "KOSEF"
    };
    auto looksLikeEtf = [&](const std::string& name) {
        for (auto& marker : etfMarkers)
            if (name.find(marker) != std::string::npos) return true;
        return false;
    };

    std::vector<StockInfo> result;
    for (auto& row : j.at("output")) {
        if ((int)result.size() >= count) break;
        std::string c = row.value("mksc_shrn_iscd", "");
        if (c.empty()) continue;
        std::string name = row.value("hts_kor_isnm", c);
        if (looksLikeEtf(name)) continue;
        double price = std::stod(row.value("stck_prpr", "0"));
        result.push_back({c, name, price});
    }
    return result;
}

std::vector<double> KisClient::getDailyCloses(const std::string& code, int count) {
    std::string query = "FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" + code +
                         "&FID_INPUT_DATE_1=" + dateOffset(count * 2 + 10) +
                         "&FID_INPUT_DATE_2=" + dateOffset(0) +
                         "&FID_PERIOD_DIV_CODE=D&FID_ORG_ADJ_PRC=1";
    auto body = request("/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice", "GET",
                         "FHKST03010100", query, true);
    auto j = json::parse(body);
    std::vector<double> closes;
    for (auto& row : j.at("output2")) {
        if (!row.contains("stck_clpr")) continue;
        std::string s = row.at("stck_clpr").get<std::string>();
        if (s.empty()) continue;
        closes.push_back(std::stod(s));
    }
    // KIS returns most-recent-first; SMA wants oldest-first.
    std::reverse(closes.begin(), closes.end());
    if ((int)closes.size() > count) closes.erase(closes.begin(), closes.end() - count);
    return closes;
}

std::string KisClient::placeMarketOrder(const std::string& code, Side side, int qty) {
    std::string trId = paper_ ? (side == Side::Buy ? "VTTC0802U" : "VTTC0801U")
                               : (side == Side::Buy ? "TTTC0802U" : "TTTC0801U");
    json body = {
        {"CANO", cano_}, {"ACNT_PRDT_CD", acntPrdtCd_}, {"PDNO", code},
        {"ORD_DVSN", "01"}, // 01 = market order
        {"ORD_QTY", std::to_string(qty)}, {"ORD_UNPR", "0"}
    };
    auto respBody = request("/uapi/domestic-stock/v1/trading/order-cash", "POST", trId, body.dump(), false);
    auto j = json::parse(respBody);
    if (j.at("rt_cd").get<std::string>() != "0")
        throw std::runtime_error("order rejected: " + j.value("msg1", respBody));
    return j.at("output").at("ODNO").get<std::string>();
}
