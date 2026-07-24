#pragma once
#include <map>
#include <string>
#include <vector>

// A stock's descriptive tags (sector/theme keywords) -- used to score "관련종목 모멘텀
// 전이" (a stock rallying because a tag-related stock surged today) by tag overlap
// against today's other movers. `parentCompany` is usually blank in this v1: no reliable
// free source was found for corporate-affiliate mapping, so it's left for manual/Claude-
// assisted filling in later. `lastUpdated` exists so a future pass can tell which entries
// are stale (tags are set once, lazily, and never auto-refreshed).
struct StockTags {
    std::string name;
    std::vector<std::string> tags;
    std::string parentCompany;
    std::string lastUpdated; // "YYYY-MM-DD"
};

// Local, file-backed store (`stock_tags.json`) so tags survive a restart. Populated
// lazily by the caller (see main.cpp) the first time an untagged code shows up in a
// scan -- this class itself is just persistence, it doesn't decide what a stock's tags
// should be.
class StockTagStore {
public:
    explicit StockTagStore(std::string filePath);

    // nullptr if `code` has never been tagged.
    const StockTags* find(const std::string& code) const;

    // Adds/overwrites one entry and immediately persists (writes are rare -- once per
    // newly-seen stock -- so there's no need to batch them).
    void set(const std::string& code, StockTags tags);

private:
    void load();
    void save() const;

    std::string filePath_;
    std::map<std::string, StockTags> tags_;
};
