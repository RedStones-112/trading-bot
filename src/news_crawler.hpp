#pragma once
#include <string>
#include <vector>

struct NewsItem {
    std::string title;
    std::string description;
};

// Crawls a handful of Korean financial-news RSS feeds and does simple keyword-based
// sentiment scoring. No YouTube/SNS -- those need official API auth (YouTube Data API,
// X/Twitter API) or fragile unofficial scraping, out of scope for this draft.
class NewsCrawler {
public:
    explicit NewsCrawler(std::vector<std::string> feedUrls);

    // Fetches all configured feeds fresh. Best-effort: a feed that fails to load or
    // parse is skipped rather than aborting the whole crawl.
    std::vector<NewsItem> fetchHeadlines() const;

    // Net sentiment for `stockName`: +1 per positive-keyword hit, -1 per negative-keyword
    // hit, summed over every headline/description that mentions the stock name. 0 if the
    // stock isn't mentioned at all. This is a crude keyword heuristic, not real NLP.
    static double scoreSentiment(const std::vector<NewsItem>& news, const std::string& stockName);

private:
    std::vector<std::string> feedUrls_;
};
