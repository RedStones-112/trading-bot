#include "kis_client.hpp"
#include "http_client.hpp"
#include "../third_party/json.hpp"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <set>
#include <thread>
#include <chrono>

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
    // The API caps each query at ~30 rows regardless of FID_VOL_CNT. FID_INPUT_ISCD "0000" /
    // "1001" / "2001" each return a different ~30-row list (confirmed live: near-zero overlap,
    // "0000" skews toward the most liquid leveraged/inverse ETFs) -- querying all three and
    // merging gets a meaningfully bigger, less ETF-heavy candidate pool than any one alone.
    static const std::vector<std::string> segments = {"0000", "1001", "2001"};

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
    std::set<std::string> seen;
    for (size_t i = 0; i < segments.size() && (int)result.size() < count; i++) {
        if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        std::string query =
            "FID_COND_MRKT_DIV_CODE=J&FID_COND_SCR_DIV_CODE=20171&FID_INPUT_ISCD=" + segments[i] +
            "&FID_DIV_CLS_CODE=0&FID_BLNG_CLS_CODE=0&FID_TRGT_CLS_CODE=111111111"
            "&FID_TRGT_EXLS_CLS_CODE=000000000&FID_INPUT_PRICE_1=0&FID_INPUT_PRICE_2=0"
            "&FID_VOL_CNT=0&FID_INPUT_DATE_1=0";
        auto body = request("/uapi/domestic-stock/v1/quotations/volume-rank", "GET", "FHPST01710000", query, true);
        auto j = json::parse(body);

        for (auto& row : j.at("output")) {
            if ((int)result.size() >= count) break;
            std::string c = row.value("mksc_shrn_iscd", "");
            if (c.empty() || seen.count(c)) continue;
            std::string name = row.value("hts_kor_isnm", c);
            if (looksLikeEtf(name)) continue;
            seen.insert(c);
            double price = std::stod(row.value("stck_prpr", "0"));
            double dayChangePct = std::stod(row.value("prdy_ctrt", "0"));
            // vol_inrt: 전일 거래량 대비 오늘 거래량 비율(%) -- confirmed live, matches
            // inquire-price's prdy_vrss_vol_rate for the same stock. Free from this same
            // ranking call, no separate request needed for the volume-surge signal.
            double volumeSurgePct = std::stod(row.value("vol_inrt", "0"));
            result.push_back({c, name, price, dayChangePct, volumeSurgePct});
        }
    }
    return result;
}

Fundamentals KisClient::getFundamentals(const std::string& code) {
    // Confirmed live (2026-07-23): per/pbr/eps/bps and bstp_kor_isnm (KRX 업종 한글명) all
    // present in inquire-price's output -- 005930/000660 both came back "전기·전자".
    std::string query = "FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" + code;
    auto body = request("/uapi/domestic-stock/v1/quotations/inquire-price", "GET", "FHKST01010100", query, true);
    auto j = json::parse(body);
    auto& out = j.at("output");
    Fundamentals f;
    f.per = std::stod(out.value("per", "0"));
    f.pbr = std::stod(out.value("pbr", "0"));
    f.sector = out.value("bstp_kor_isnm", "");
    return f;
}

std::vector<DailyBar> KisClient::getDailyBars(const std::string& code, int count) {
    std::string query = "FID_COND_MRKT_DIV_CODE=J&FID_INPUT_ISCD=" + code +
                         "&FID_INPUT_DATE_1=" + dateOffset(count * 2 + 10) +
                         "&FID_INPUT_DATE_2=" + dateOffset(0) +
                         "&FID_PERIOD_DIV_CODE=D&FID_ORG_ADJ_PRC=1";
    auto body = request("/uapi/domestic-stock/v1/quotations/inquire-daily-itemchartprice", "GET",
                         "FHKST03010100", query, true);
    auto j = json::parse(body);
    std::vector<DailyBar> bars;
    // stck_hgpr/stck_lwpr/acml_vol confirmed live (2026-07-24) in the same output2 rows as
    // stck_clpr -- no separate call needed for the volume-profile signal (strategy.hpp).
    for (auto& row : j.at("output2")) {
        if (!row.contains("stck_clpr")) continue;
        std::string s = row.at("stck_clpr").get<std::string>();
        if (s.empty()) continue;
        DailyBar b;
        b.close = std::stod(s);
        b.high = std::stod(row.value("stck_hgpr", "0"));
        b.low = std::stod(row.value("stck_lwpr", "0"));
        b.volume = std::stod(row.value("acml_vol", "0"));
        bars.push_back(b);
    }
    // KIS returns most-recent-first; SMA/volume-profile both want oldest-first.
    std::reverse(bars.begin(), bars.end());
    if ((int)bars.size() > count) bars.erase(bars.begin(), bars.end() - count);
    return bars;
}

double KisClient::getBuyableCash() {
    // 매수가능조회 -- ord_psbl_cash is account-wide buyable cash, not gated by PDNO's
    // price (PDNO is required by the endpoint but doesn't restrict the returned amount).
    // Field name is a best guess from docs; verify against a live paper-account call.
    std::string trId = paper_ ? "VTTC8908R" : "TTTC8908R";
    std::string query = "CANO=" + cano_ + "&ACNT_PRDT_CD=" + acntPrdtCd_ +
                         "&PDNO=005930&ORD_UNPR=0&ORD_DVSN=01"
                         "&CMA_EVLU_AMT_ICLD_YN=N&OVRS_ICLD_YN=N";
    auto body = request("/uapi/domestic-stock/v1/trading/inquire-psbl-order", "GET", trId, query, true);
    auto j = json::parse(body);
    return std::stod(j.at("output").value("ord_psbl_cash", "0"));
}

std::vector<HeldStock> KisClient::getHoldings() {
    // 잔고조회 -- lists everything currently held in the account, independent of
    // whatever this process remembers locally. Used at startup to recover positions
    // bought in a previous run. Field names are a best guess from docs; verify against
    // a live paper-account call. Not paginated (CTX_AREA_*100 left blank) -- fine for
    // this bot's small position counts, but a very large account could be truncated.
    std::string trId = paper_ ? "VTTC8434R" : "TTTC8434R";
    std::string query = "CANO=" + cano_ + "&ACNT_PRDT_CD=" + acntPrdtCd_ +
                         "&AFHR_FLPR_YN=N&OFL_YN=&INQR_DVSN=02&UNPR_DVSN=01"
                         "&FUND_STTL_ICLD_YN=N&FNCG_AMT_AUTO_RDPT_YN=N&PRCS_DVSN=00"
                         "&CTX_AREA_FK100=&CTX_AREA_NK100=";
    auto body = request("/uapi/domestic-stock/v1/trading/inquire-balance", "GET", trId, query, true);
    auto j = json::parse(body);

    std::vector<HeldStock> result;
    for (auto& row : j.at("output1")) {
        int qty = std::stoi(row.value("hldg_qty", "0"));
        if (qty <= 0) continue; // KIS keeps zero-qty rows for stocks fully sold earlier
        result.push_back({row.value("pdno", ""), row.value("prdt_name", ""), qty,
                           std::stod(row.value("pchs_avg_pric", "0"))});
    }
    return result;
}

std::vector<PendingOrder> KisClient::getPendingOrders() {
    if (paper_) {
        // Confirmed via a live call: KIS 모의투자 rejects this *listing* outright
        // ("모의투자에서는 해당업무가 제공되지 않습니다", rt_cd=1) -- but cancelOrder()
        // below (a different endpoint) works fine on paper accounts. Paper just has no
        // API-level way to *discover* what's pending; cancelling a known order id is fine.
        return {};
    }
    std::string query = "CANO=" + cano_ + "&ACNT_PRDT_CD=" + acntPrdtCd_ +
                         "&CTX_AREA_FK100=&CTX_AREA_NK100=&INQR_DVSN_1=0&INQR_DVSN_2=0";
    auto body = request("/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl", "GET", "TTTC8036R", query, true);
    auto j = json::parse(body);
    if (j.value("rt_cd", "1") != "0")
        throw std::runtime_error("pending-order query rejected: " + j.value("msg1", body));

    std::vector<PendingOrder> result;
    for (auto& row : j.at("output")) {
        int qty = std::stoi(row.value("psbl_qty", "0"));
        if (qty <= 0) continue; // nothing left to cancel on this order
        result.push_back({row.value("odno", ""), row.value("pdno", ""),
                           row.value("prdt_name", row.value("pdno", "")), qty});
    }
    return result;
}

void KisClient::cancelOrder(const std::string& odno) {
    // KRX_FWDG_ORD_ORGNO left blank and ORD_QTY=0 + QTY_ALL_ORD_YN=Y (cancel whatever
    // quantity is still unfilled, regardless of amount) confirmed working live on a paper
    // account -- no need for the blocked listing endpoint's per-order detail at all.
    std::string trId = paper_ ? "VTTC0803U" : "TTTC0803U";
    json body = {
        {"CANO", cano_}, {"ACNT_PRDT_CD", acntPrdtCd_},
        {"KRX_FWDG_ORD_ORGNO", ""},
        {"ORGN_ODNO", odno},
        {"ORD_DVSN", "01"}, // this bot only ever places market (01) orders
        {"RVSE_CNCL_DVSN_CD", "02"}, // 02 = 취소
        {"ORD_QTY", "0"},
        {"ORD_UNPR", "0"},
        {"QTY_ALL_ORD_YN", "Y"}
    };
    auto respBody = request("/uapi/domestic-stock/v1/trading/order-rvsecncl", "POST", trId, body.dump(), false);
    auto j = json::parse(respBody);
    if (j.at("rt_cd").get<std::string>() != "0")
        throw std::runtime_error("cancel rejected: " + j.value("msg1", respBody));
}

std::string KisClient::placeMarketOrder(const std::string& code, Side side, int qty,
                                         double /*feeRate*/, double /*taxRate*/) {
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
