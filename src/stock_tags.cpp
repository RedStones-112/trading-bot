#include "stock_tags.hpp"
#include "../third_party/json.hpp"
#include <fstream>

using json = nlohmann::json;

StockTagStore::StockTagStore(std::string filePath) : filePath_(std::move(filePath)) { load(); }

void StockTagStore::load() {
    std::ifstream in(filePath_);
    if (!in) return; // no file yet -- fine, starts empty
    json j;
    try {
        j = json::parse(in, nullptr, true, true);
    } catch (const std::exception&) {
        return; // corrupt/empty file -- start empty rather than crash the bot over this
    }
    for (auto& [code, entry] : j.items()) {
        StockTags st;
        st.name = entry.value("name", "");
        st.tags = entry.value("tags", std::vector<std::string>{});
        st.parentCompany = entry.value("parentCompany", "");
        st.lastUpdated = entry.value("lastUpdated", "");
        tags_[code] = std::move(st);
    }
}

void StockTagStore::save() const {
    json j;
    for (auto& [code, st] : tags_) {
        j[code] = {{"name", st.name}, {"tags", st.tags},
                    {"parentCompany", st.parentCompany}, {"lastUpdated", st.lastUpdated}};
    }
    std::ofstream out(filePath_, std::ios::trunc);
    out << j.dump(2);
}

const StockTags* StockTagStore::find(const std::string& code) const {
    auto it = tags_.find(code);
    return it == tags_.end() ? nullptr : &it->second;
}

void StockTagStore::set(const std::string& code, StockTags tags) {
    tags_[code] = std::move(tags);
    save();
}
