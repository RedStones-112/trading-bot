#include "../src/strategy.hpp"
#include "../src/mock_broker.hpp"
#include "../src/news_crawler.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    // Flat history, then a sharp rise: short SMA should cross above long SMA -> Buy.
    std::vector<double> closes(20, 100.0);
    closes.push_back(150.0); // 21st point, pushes short SMA(5) above long SMA(20)
    assert(smaCrossSignal(closes, 5, 20) == Signal::Buy);

    // Flat history, then a sharp drop -> short SMA crosses below long SMA -> Sell.
    std::vector<double> closes2(20, 100.0);
    closes2.push_back(50.0);
    assert(smaCrossSignal(closes2, 5, 20) == Signal::Sell);

    // No cross: still flat -> Hold.
    std::vector<double> closes3(21, 100.0);
    assert(smaCrossSignal(closes3, 5, 20) == Signal::Hold);

    // Not enough history -> throws.
    bool threw = false;
    try {
        smaCrossSignal(std::vector<double>{1.0, 2.0}, 5, 20);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // MockBroker: never touches the network, returns sane values.
    MockBroker broker(70000.0, /*seed=*/42);
    double price = broker.getCurrentPrice("005930");
    assert(price > 0);
    auto history = broker.getDailyCloses("005930", 10);
    assert(history.size() == 10);
    auto orderId = broker.placeMarketOrder("005930", IBroker::Side::Buy, 1);
    assert(!orderId.empty());

    // netProfitPct: buy 10000, sell 10500 with 0.015% fee + 0.18% tax should still be a net gain,
    // and matches a plain hand-computed value.
    {
        double pct = netProfitPct(10000.0, 10500.0, 0.00015, 0.0018);
        double buyCost = 10000.0 * 1.00015;
        double sellProceeds = 10500.0 * (1 - 0.00015 - 0.0018);
        double expected = (sellProceeds - buyCost) / buyCost;
        assert(std::fabs(pct - expected) < 1e-9);
        assert(pct > 0); // a 5% gross move should clear fees/tax easily
    }
    // A flat price nets a small loss once fees/tax are deducted.
    assert(netProfitPct(10000.0, 10000.0, 0.00015, 0.0018) < 0);

    // NewsCrawler::scoreSentiment: keyword hits only count for headlines mentioning the stock.
    {
        std::vector<NewsItem> news = {
            {"삼성전자 실적 급등, 목표가 상향", ""},
            {"삼성전자 공장 화재로 소송 우려", ""},
            {"이 뉴스는 다른 종목 얘기", "SK하이닉스 급등"},
        };
        // headline 1: +1 (급등) +1 (목표가 상향); headline 2: -1 (소송); headline 3 doesn't mention 삼성전자.
        assert(NewsCrawler::scoreSentiment(news, "삼성전자") == 1.0);
        assert(NewsCrawler::scoreSentiment(news, "존재하지않는종목") == 0.0);
    }

    std::cout << "all tests passed\n";
    return 0;
}
