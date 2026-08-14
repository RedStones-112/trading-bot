#include "../src/strategy.hpp"
#include "../src/mock_broker.hpp"
#include "../src/news_crawler.hpp"
#include "../src/event_calendar.hpp"
#include "../src/ml_model.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <direct.h>
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

    // smaTrendNotFalling: steadily declining closes -> long SMA today is below long SMA
    // `lookback` bars ago -> false (this is the case that should block a golden-cross buy
    // that's really just a bounce inside a downtrend, e.g. 삼기/다스코 2026-08-10~14).
    std::vector<double> declining;
    for (int i = 0; i < 30; i++) declining.push_back(100.0 - i); // 100, 99, ..., 71
    assert(!smaTrendNotFalling(declining, 20, 5));

    // Flat/rising closes -> long SMA not falling -> true.
    std::vector<double> flat(30, 100.0);
    assert(smaTrendNotFalling(flat, 20, 5));
    std::vector<double> rising;
    for (int i = 0; i < 30; i++) rising.push_back(100.0 + i);
    assert(smaTrendNotFalling(rising, 20, 5));

    // Not enough history to compare -> fail-open (true), same "정보 없음=중립" convention
    // as the other signals -- doesn't block a buy just because history is thin.
    std::vector<double> thin(10, 100.0);
    assert(smaTrendNotFalling(thin, 20, 5));

    // MockBroker: never touches the network, returns sane values.
    MockBroker broker(70000.0, /*initialCash=*/10000000.0, /*seed=*/42);
    double price = broker.getCurrentPrice("005930");
    assert(price > 0);
    auto history = broker.getDailyBars("005930", 10);
    assert(history.size() == 10);
    assert(broker.getBuyableCash() == 10000000.0);
    auto orderId = broker.placeMarketOrder("005930", IBroker::Side::Buy, 1, 0.00015, 0.0018);
    assert(!orderId.empty());
    // Cash debited by the buy: price*(1+feeRate).
    assert(broker.getBuyableCash() < 10000000.0);
    // Buying more than affordable should throw rather than silently over-spend.
    bool insufficientThrew = false;
    try {
        broker.placeMarketOrder("005930", IBroker::Side::Buy, 1000000, 0.00015, 0.0018);
    } catch (const std::exception&) {
        insufficientThrew = true;
    }
    assert(insufficientThrew);

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

    // belowPriceVolumeRatio: fraction of recent volume that traded below currentPrice.
    {
        // All volume below current price (bar fully below) -> ratio 1.0 (max bullish: no overhead supply).
        std::vector<DailyBar> allBelow = {{95.0, 100.0, 90.0, 1000.0}};
        assert(std::fabs(belowPriceVolumeRatio(allBelow, 110.0) - 1.0) < 1e-9);

        // All volume above current price (bar fully above) -> ratio 0.0 (max bearish: overhead resistance).
        std::vector<DailyBar> allAbove = {{115.0, 120.0, 110.0, 1000.0}};
        assert(std::fabs(belowPriceVolumeRatio(allAbove, 100.0) - 0.0) < 1e-9);

        // currentPrice splits the bar's range exactly in half -> ratio 0.5.
        std::vector<DailyBar> split = {{100.0, 110.0, 90.0, 1000.0}};
        assert(std::fabs(belowPriceVolumeRatio(split, 100.0) - 0.5) < 1e-9);

        // No usable bars -> neutral 0.5, not a guess toward either side.
        assert(std::fabs(belowPriceVolumeRatio({}, 100.0) - 0.5) < 1e-9);
    }

    // probabilityFromTechnicals: volume-profile position dominates, trend/volume-surge are
    // smaller adjustments, everything clamped to [0.05, 0.95].
    {
        // Fully neutral inputs (belowRatio=0.5=neutral, no trend, no volume surge) -> 0.5.
        assert(std::fabs(probabilityFromTechnicals(100.0, 0.0, 0.5) - 0.5) < 1e-9);

        // Mixed non-saturating inputs match the hand-computed weighted sum.
        // profileTerm = 0.6-0.5 = 0.1; trendTerm = 0.05*0.5 = 0.025; volTerm = 0.5*0.1 = 0.05
        double expected = 0.5 + 0.1 + 0.025 + 0.05;
        assert(std::fabs(probabilityFromTechnicals(150.0, 0.05, 0.6) - expected) < 1e-9);

        // Extreme favorable (all volume below price, strong up-trend, big volume surge) clamps at 0.95.
        assert(probabilityFromTechnicals(500.0, 0.5, 1.0) == 0.95);
        // Extreme unfavorable (all volume above price, strong down-trend) clamps at 0.05.
        assert(probabilityFromTechnicals(100.0, -0.5, 0.0) == 0.05);
    }

    // detectSwings: zigzag reversal points at a fixed threshold.
    {
        // 100 -(+12%)-> 112 -(-13%)-> 97 -(+24%)-> 120: three reversals, each clear of 10%
        // (kept off the exact boundary so float rounding of the threshold can't flip a compare).
        std::vector<double> series = {100.0, 112.0, 97.0, 120.0};
        auto swings = detectSwings(series, 0.10);
        assert(swings.size() == 3);
        assert(std::fabs(swings[0].price - 100.0) < 1e-9);
        assert(std::fabs(swings[1].price - 112.0) < 1e-9);
        assert(std::fabs(swings[2].price - 97.0) < 1e-9);

        // Wobbles that never clear a 90% reversal from the running extreme -> no swings.
        std::vector<double> flat = {100.0, 105.0, 95.0, 102.0};
        assert(detectSwings(flat, 0.90).empty());
    }

    // probabilityFromWaveAnalysis: last confirmed swing leg sets the trend, currentPrice's
    // position against it (extension vs. Fibonacci retracement depth) sets the probability.
    {
        auto bar = [](double close) { return DailyBar{close, close + 1.0, close - 1.0, 1000.0}; };

        // Too little structure to confirm even one full leg -> neutral.
        assert(std::fabs(probabilityFromWaveAnalysis({}, 100.0) - 0.5) < 1e-9);
        std::vector<DailyBar> wobble = {bar(100.0), bar(101.0), bar(99.0), bar(100.0)}; // never clears 3%
        assert(std::fabs(probabilityFromWaveAnalysis(wobble, 100.0) - 0.5) < 1e-9);

        // Confirmed up-leg 100 -> 110 (bars dip back to 106 to confirm the 110 high).
        std::vector<DailyBar> upLeg = {bar(100.0), bar(104.0), bar(110.0), bar(106.0)};
        assert(std::fabs(probabilityFromWaveAnalysis(upLeg, 115.0) - 0.80) < 1e-9); // new high -- impulse continues
        assert(std::fabs(probabilityFromWaveAnalysis(upLeg, 107.0) - 0.65) < 1e-9); // 30% retrace -- shallow
        assert(std::fabs(probabilityFromWaveAnalysis(upLeg, 105.0) - 0.55) < 1e-9); // 50% retrace -- fib zone
        assert(std::fabs(probabilityFromWaveAnalysis(upLeg, 102.0) - 0.40) < 1e-9); // 80% retrace -- weak
        assert(std::fabs(probabilityFromWaveAnalysis(upLeg, 99.0) - 0.20) < 1e-9);  // past the leg's start -- invalidated

        // Confirmed down-leg 100 -> 90 (bars bounce back to 94 to confirm the 90 low), mirrored.
        std::vector<DailyBar> downLeg = {bar(100.0), bar(96.0), bar(90.0), bar(94.0)};
        assert(std::fabs(probabilityFromWaveAnalysis(downLeg, 85.0) - 0.20) < 1e-9);
        assert(std::fabs(probabilityFromWaveAnalysis(downLeg, 93.0) - 0.35) < 1e-9);
        assert(std::fabs(probabilityFromWaveAnalysis(downLeg, 95.0) - 0.45) < 1e-9);
        assert(std::fabs(probabilityFromWaveAnalysis(downLeg, 98.0) - 0.55) < 1e-9);
        assert(std::fabs(probabilityFromWaveAnalysis(downLeg, 101.0) - 0.65) < 1e-9);
    }

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

    // eventMultiplier: matching by code, by tag, and by "ALL"; distance-weighted decay;
    // events outside the lookahead window (past or too far out) excluded entirely.
    {
        std::vector<ScheduledEvent> events = {
            {"005930", "2026-01-08", "dividend", 0.2, "코드로 매칭, D-day(가중치 1.0)"},
            {"반도체", "2026-01-15", "legal", -0.2, "태그로 매칭, 7일 후(가중치 0.5)"},
            {"ALL", "2026-01-01", "political", 0.1, "지난 날짜 -- 제외돼야 함"},
            {"000660", "2026-01-30", "dividend", 0.5, "코드 불일치 + 태그 없음 -- 매칭 안 됨"},
        };
        double mult = eventMultiplier(events, "005930", {"반도체"}, "2026-01-08", 14);
        // 1.0 + 0.2*1.0(당일) + (-0.2)*(1 - 7/14)(7일 후, 가중치 0.5) = 1.0 + 0.2 - 0.1 = 1.1
        assert(std::fabs(mult - 1.1) < 1e-9);

        // 매칭되는 이벤트가 전혀 없으면 배수는 그대로 1.0.
        assert(std::fabs(eventMultiplier(events, "999999", {}, "2026-01-08", 14) - 1.0) < 1e-9);

        // 큰 impact도 [0.8, 1.3] 밖으로는 못 나감.
        std::vector<ScheduledEvent> bigEvent = {{"ALL", "2026-01-08", "legal", 5.0, "clamp 확인"}};
        assert(eventMultiplier(bigEvent, "005930", {}, "2026-01-08", 14) == 1.3);
    }
    // 없는 파일을 읽으면 조용히 빈 목록(스캔이 시작조차 실패하면 안 됨).
    assert(loadEventCalendar("no_such_file_events.json").empty());

    // buildTrainingSet: windowBars=3, lookaheadDays=2, takeProfitPct=2%, stopLossPct=3% --
    // anchor is always bars[2] (idx0..idx2 window), label decided by bars[3]/bars[4].
    {
        auto anchorBars = [](double h3, double l3, double h4, double l4) {
            return std::vector<DailyBar>{
                {100.0, 101.0, 99.0, 1000.0},
                {101.0, 102.0, 100.0, 1000.0},
                {100.0, 101.0, 99.0, 1000.0},
                {100.0, h3, l3, 1000.0},
                {100.0, h4, l4, 1000.0},
            };
        };
        // Take-profit hit on day 1 (high 103 >= 100*1.02) -> label 1.
        auto s1 = buildTrainingSet(anchorBars(103.0, 99.0, 101.0, 99.0), 0.02, 0.03, 2, 3, 2, 3);
        assert(s1.size() == 1);
        assert(s1[0].second == 1.0);
        assert(std::fabs(s1[0].first.trendPct) < 1e-9); // window front/anchor both close=100
        assert(std::fabs(s1[0].first.volumeSurgePct - 100.0) < 1e-9); // flat volume

        // Stop-loss hit on day 1 (low 96 <= 100*0.97) -> label 0.
        auto s2 = buildTrainingSet(anchorBars(101.0, 96.0, 101.0, 99.0), 0.02, 0.03, 2, 3, 2, 3);
        assert(s2.size() == 1 && s2[0].second == 0.0);

        // Neither threshold reached within the horizon -> label 0.
        auto s3 = buildTrainingSet(anchorBars(101.0, 99.0, 101.0, 99.0), 0.02, 0.03, 2, 3, 2, 3);
        assert(s3.size() == 1 && s3[0].second == 0.0);

        // Both thresholds hit the same day -> ambiguous, conservatively label 0.
        auto s4 = buildTrainingSet(anchorBars(103.0, 96.0, 101.0, 99.0), 0.02, 0.03, 2, 3, 2, 3);
        assert(s4.size() == 1 && s4[0].second == 0.0);

        // windowBars < smaLong -> refuses to produce samples rather than feeding smaMomentum
        // a window it can't compute over.
        assert(buildTrainingSet(anchorBars(101.0, 99.0, 101.0, 99.0), 0.02, 0.03, 2, 2, 2, 3).empty());
    }

    // MlModelStore: train/predict/persist round-trip against a throwaway directory.
    {
        const char* dir = "test_ml_models_tmp";
        std::vector<DailyBar> bars;
        for (int i = 0; i < 80; i++) {
            double close = 100.0 + 5.0 * std::sin(i * 0.3) + i * 0.05;
            bars.push_back({close, close * 1.01, close * 0.99, 1000.0 + i * 5});
        }

        {
            MlModelStore store(dir);
            assert(store.needsRetrain("000001", 1, "2026-01-01")); // never trained yet
            int n = store.train("000001", bars, 0.02, 0.03, 5, 5, 20, "2026-01-01");
            assert(n > 0); // 80 bars is comfortably above the minimum sample floor
            assert(!store.needsRetrain("000001", 1, "2026-01-01")); // trained today, 0 days elapsed
            assert(store.needsRetrain("000001", 1, "2026-01-02")); // 1 day elapsed >= retrainDays

            MlFeatures f;
            double p1 = store.predict("000001", f);
            assert(p1 >= 0.05 && p1 <= 0.95);
            assert(store.predict("999999", f) == -1.0); // untrained code -- no model to fall back on
        }
        {
            // Fresh store instance over the same directory: weights + index must survive.
            MlModelStore store2(dir);
            assert(!store2.needsRetrain("000001", 1, "2026-01-01")); // index.json round-tripped
            MlFeatures f;
            double p2 = store2.predict("000001", f);
            assert(p2 >= 0.05 && p2 <= 0.95); // .nn weights file round-tripped and is readable
        }

        std::remove((std::string(dir) + "/000001.nn").c_str());
        std::remove((std::string(dir) + "/index.json").c_str());
        _rmdir(dir);
    }

    std::cout << "all tests passed\n";
    return 0;
}
