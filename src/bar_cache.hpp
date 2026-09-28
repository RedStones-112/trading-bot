#pragma once
#include "broker.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// Local disk cache of KIS daily bars, one JSON file per symbol under data/daily_bars/ --
// same "file-based, missing = empty" convention as stock_tags.json/events.json/ml_models/.
// Exists so backtest_bulk.cpp's wide-universe, multi-year backtest doesn't have to re-page
// through KIS's 100-bar-per-call history endpoint (1.1s/call) on every run (2026-09-28,
// PROGRESS.md -- the daily automated session's own backtest.cpp stays untouched, this cache
// is only for this separate manual research tool) -- only bars newer than what's already on
// disk need fetching after the first full pull.

inline std::string barCachePath(const std::string& code) {
    return "data/daily_bars/" + code + ".json";
}

inline std::vector<DailyBar> loadCachedBars(const std::string& code) {
    std::vector<DailyBar> bars;
    std::ifstream f(barCachePath(code));
    if (!f) return bars;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& row : j.at("bars")) {
            DailyBar b;
            b.date = row.value("date", "");
            b.close = row.value("close", 0.0);
            b.high = row.value("high", 0.0);
            b.low = row.value("low", 0.0);
            b.volume = row.value("volume", 0.0);
            bars.push_back(b);
        }
    } catch (const std::exception&) {
        return {}; // corrupt/partial cache file -- treat as empty, caller re-fetches fully
    }
    return bars;
}

inline void saveCachedBars(const std::string& code, const std::vector<DailyBar>& bars) {
    std::filesystem::create_directories("data/daily_bars");
    nlohmann::json j;
    j["bars"] = nlohmann::json::array();
    for (auto& b : bars)
        j["bars"].push_back({{"date", b.date}, {"close", b.close}, {"high", b.high},
                              {"low", b.low}, {"volume", b.volume}});
    std::ofstream out(barCachePath(code));
    out << j.dump();
}

// Merges freshly fetched bars into what's already cached, de-duplicated by date and sorted
// oldest-first (std::map keys ascending -- "YYYYMMDD" sorts correctly as a plain string).
// `fresh` wins on a date collision (KIS occasionally revises a recent bar's OHLCV before it
// finalizes). Bars with an empty date are dropped -- this cache only ever holds real KIS
// bars (backtest_bulk.cpp refuses mode=mock), and an empty date would collide with every
// other empty-dated bar under one map key.
// Every symbol code currently cached on disk (data/daily_bars/<code>.json), sorted for a
// deterministic iteration order -- portfolio_sim.cpp uses this to build its train/holdout
// symbol split without needing a live KIS call.
inline std::vector<std::string> listCachedSymbols() {
    std::vector<std::string> codes;
    if (!std::filesystem::exists("data/daily_bars")) return codes;
    for (auto& entry : std::filesystem::directory_iterator("data/daily_bars")) {
        if (entry.path().extension() == ".json") codes.push_back(entry.path().stem().string());
    }
    std::sort(codes.begin(), codes.end());
    return codes;
}

inline std::vector<DailyBar> mergeBars(const std::vector<DailyBar>& existing, const std::vector<DailyBar>& fresh) {
    std::map<std::string, DailyBar> byDate;
    for (auto& b : existing) if (!b.date.empty()) byDate[b.date] = b;
    for (auto& b : fresh) if (!b.date.empty()) byDate[b.date] = b;
    std::vector<DailyBar> result;
    result.reserve(byDate.size());
    for (auto& [date, bar] : byDate) result.push_back(bar);
    return result;
}
