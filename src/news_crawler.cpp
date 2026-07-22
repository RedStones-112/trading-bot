#include "news_crawler.hpp"
#include "http_client.hpp"
#include <iomanip>
#include <sstream>

namespace {

void splitUrl(const std::string& url, std::wstring& host, std::wstring& path) {
    std::string s = url;
    const std::string prefix = "https://";
    if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
    auto slash = s.find('/');
    std::string h = slash == std::string::npos ? s : s.substr(0, slash);
    std::string p = slash == std::string::npos ? "/" : s.substr(slash);
    host = std::wstring(h.begin(), h.end());
    path = std::wstring(p.begin(), p.end());
}

// Extracts the text content of the first <tag>...</tag> in `block`, unwrapping a
// <![CDATA[...]]> payload if present. Returns "" if the tag isn't found.
std::string extractTag(const std::string& block, const std::string& tag) {
    std::string openTag = "<" + tag;
    size_t start = block.find(openTag);
    if (start == std::string::npos) return "";
    size_t gt = block.find('>', start);
    if (gt == std::string::npos) return "";
    std::string closeTag = "</" + tag + ">";
    size_t closeStart = block.find(closeTag, gt);
    if (closeStart == std::string::npos) return "";

    std::string content = block.substr(gt + 1, closeStart - gt - 1);
    size_t cdataStart = content.find("<![CDATA[");
    if (cdataStart != std::string::npos) {
        size_t cdataEnd = content.find("]]>", cdataStart);
        if (cdataEnd == std::string::npos) return "";
        content = content.substr(cdataStart + 9, cdataEnd - cdataStart - 9);
    }
    return content;
}

// Pulls every <item>...</item> block out of an RSS body into title/description pairs.
std::vector<NewsItem> extractItems(const std::string& body) {
    std::vector<NewsItem> result;
    size_t pos = 0;
    while (true) {
        size_t start = body.find("<item", pos);
        if (start == std::string::npos) break;
        size_t end = body.find("</item>", start);
        if (end == std::string::npos) break;
        std::string block = body.substr(start, end - start);

        NewsItem item;
        item.title = extractTag(block, "title");
        item.description = extractTag(block, "description");
        if (!item.title.empty()) result.push_back(std::move(item));

        pos = end + 7;
    }
    return result;
}

std::string urlEncode(const std::string& s) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
        else oss << '%' << std::setw(2) << (int)c;
    }
    return oss.str();
}

} // namespace

NewsCrawler::NewsCrawler(std::vector<std::string> feedUrls) : feedUrls_(std::move(feedUrls)) {}

std::vector<NewsItem> NewsCrawler::fetchHeadlines() const {
    std::vector<NewsItem> result;
    for (auto& url : feedUrls_) {
        try {
            std::wstring host, path;
            splitUrl(url, host, path);
            auto resp = http::request(host, 443, path, "GET", "", "");
            if (resp.status != 200) continue;
            auto items = extractItems(resp.body);
            result.insert(result.end(), items.begin(), items.end());
        } catch (...) {
            // best-effort: one bad feed shouldn't sink the whole crawl
        }
    }
    return result;
}

std::vector<NewsItem> NewsCrawler::searchHeadlines(const std::string& keyword) {
    try {
        std::string path = "/rss/search?q=" + urlEncode(keyword) + "&hl=ko&gl=KR&ceid=KR:ko";
        auto resp = http::request(L"news.google.com", 443, std::wstring(path.begin(), path.end()), "GET", "", "");
        if (resp.status != 200) return {};
        return extractItems(resp.body);
    } catch (...) {
        return {};
    }
}

double NewsCrawler::scoreSentiment(const std::vector<NewsItem>& news, const std::string& stockName) {
    static const std::vector<std::string> positive = {
        "급등", "상승", "호실적", "흑자전환", "최대실적", "신고가", "수주",
        "특허", "호재", "목표가 상향", "깜짝실적", "돌파", "역대급"
    };
    static const std::vector<std::string> negative = {
        "급락", "하락", "적자", "리콜", "소송", "횡령", "상장폐지",
        "경고", "악재", "목표가 하향", "과징금", "조사", "부도"
    };

    double score = 0;
    for (auto& item : news) {
        std::string text = item.title + " " + item.description;
        if (text.find(stockName) == std::string::npos) continue;
        for (auto& kw : positive) if (text.find(kw) != std::string::npos) score += 1;
        for (auto& kw : negative) if (text.find(kw) != std::string::npos) score -= 1;
    }
    return score;
}
