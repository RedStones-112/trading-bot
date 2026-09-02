#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// trades.log split one file per month (trades-YYYY-MM.log) so a single file doesn't grow
// unbounded across a long-running account. Filenames sort chronologically as plain strings
// since they're YYYY-MM, so no separate index is needed. Shared by main.cpp (writes/reads
// the running account's own log) and backtest.cpp (reads it read-only for the symbol
// universe).
inline std::string tradesLogPath(const std::string& isoDateOrTimestamp) {
    return "trades-" + isoDateOrTimestamp.substr(0, 7) + ".log";
}

// All trades-*.log files present, oldest month first.
inline std::vector<std::string> allTradesLogPaths() {
    std::vector<std::string> paths;
    for (auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
        std::string name = entry.path().filename().string();
        if (name.rfind("trades-", 0) == 0 && name.size() == 18 && name.substr(14) == ".log")
            paths.push_back(name);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}
