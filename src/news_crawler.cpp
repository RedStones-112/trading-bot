#include "news_crawler.hpp"
#include "http_client.hpp"

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

            size_t pos = 0;
            while (true) {
                size_t start = resp.body.find("<item", pos);
                if (start == std::string::npos) break;
                size_t end = resp.body.find("</item>", start);
                if (end == std::string::npos) break;
                std::string block = resp.body.substr(start, end - start);

                NewsItem item;
                item.title = extractTag(block, "title");
                item.description = extractTag(block, "description");
                if (!item.title.empty()) result.push_back(std::move(item));

                pos = end + 7;
            }
        } catch (...) {
            // best-effort: one bad feed shouldn't sink the whole crawl
        }
    }
    return result;
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
