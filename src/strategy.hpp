#pragma once
#include "broker.hpp"
#include <algorithm>
#include <vector>
#include <numeric>
#include <stdexcept>

enum class Signal { Hold, Buy, Sell };

// Golden cross (short SMA crosses above long) => Buy. Dead cross => Sell. Else Hold.
// closes must be oldest-first and include today's price appended as the last element.
inline Signal smaCrossSignal(const std::vector<double>& closes, int shortPeriod, int longPeriod) {
    if ((int)closes.size() < longPeriod + 1)
        throw std::runtime_error("not enough price history for SMA(" + std::to_string(longPeriod) + ")");

    auto sma = [&](int period, int endExclusive) {
        double sum = std::accumulate(closes.begin() + (endExclusive - period), closes.begin() + endExclusive, 0.0);
        return sum / period;
    };

    int n = (int)closes.size();
    double shortNow = sma(shortPeriod, n);
    double longNow = sma(longPeriod, n);
    double shortPrev = sma(shortPeriod, n - 1);
    double longPrev = sma(longPeriod, n - 1);

    bool wasBelow = shortPrev <= longPrev;
    bool isAbove = shortNow > longNow;
    if (wasBelow && isAbove) return Signal::Buy;

    bool wasAbove = shortPrev >= longPrev;
    bool isBelow = shortNow < longNow;
    if (wasAbove && isBelow) return Signal::Sell;

    return Signal::Hold;
}

// Relative gap between short and long SMA (positive = short above long, i.e. bullish).
// Used to rank multiple Buy-signal candidates against each other -- bigger gap = stronger cross.
inline double smaMomentum(const std::vector<double>& closes, int shortPeriod, int longPeriod) {
    if ((int)closes.size() < longPeriod)
        throw std::runtime_error("not enough price history for SMA(" + std::to_string(longPeriod) + ")");
    auto sma = [&](int period) {
        double sum = std::accumulate(closes.end() - period, closes.end(), 0.0);
        return sum / period;
    };
    double shortNow = sma(shortPeriod);
    double longNow = sma(longPeriod);
    return (shortNow - longNow) / longNow;
}

// Net profit fraction of a round-trip trade after commission (both legs) and the
// securities transaction tax (sell leg only, per KRX convention). Positive = profit.
inline double netProfitPct(double buyPrice, double sellPrice, double feeRate, double taxRate) {
    double buyCost = buyPrice * (1 + feeRate);
    double sellProceeds = sellPrice * (1 - feeRate - taxRate);
    return (sellProceeds - buyCost) / buyCost;
}

// Approximates a volume profile from daily bars: what fraction of recent traded volume
// happened at price levels *below* `currentPrice` (0..1, 0.5 = neutral). Each day's volume
// is assumed spread evenly across that day's [low, high] range (no intraday tick data to
// do better with). A high ratio means most of the recent volume traded below where the
// stock sits now -- overhead supply is light, so there's less "everyone who bought up
// here is underwater and may sell to break even" resistance. A low ratio means the bulk
// of recent volume sits above the current price -- that's overhead resistance.
// ponytail: daily-bar approximation, not true intraday volume-at-price -- swap for KIS's
// minute-candle endpoint if this needs to be more precise than "which side of price the
// recent volume mostly sits on".
inline double belowPriceVolumeRatio(const std::vector<DailyBar>& bars, double currentPrice) {
    double belowVol = 0.0, aboveVol = 0.0;
    for (auto& bar : bars) {
        if (bar.volume <= 0 || bar.high <= bar.low) continue;
        if (bar.high <= currentPrice) { belowVol += bar.volume; continue; }
        if (bar.low >= currentPrice) { aboveVol += bar.volume; continue; }
        double fracBelow = (currentPrice - bar.low) / (bar.high - bar.low);
        belowVol += bar.volume * fracBelow;
        aboveVol += bar.volume * (1.0 - fracBelow);
    }
    double total = belowVol + aboveVol;
    return total > 0 ? belowVol / total : 0.5; // no usable bars -- neutral, not a guess either way
}

// Replaces news-sentiment probability with day-trading-appropriate technical signals
// (사용자 확인: 뉴스는 장기 재료인 경우가 많아 단타 봇에 오히려 손실을 유발함).
// `belowRatio` (volume-profile position, see above) dominates -- it's the signal the user
// specifically asked for ("실제로 올라온 물량"의 위치). Trend and volume-surge are smaller
// supporting adjustments. Same [0.05, 0.95] clamp shape as the sentiment version it replaces.
inline double probabilityFromTechnicals(double volumeSurgePct, double trendPct, double belowRatio) {
    double profileTerm = belowRatio - 0.5;                                          // -0.5..+0.5, dominant
    double trendTerm = std::clamp(trendPct, -0.2, 0.2) * 0.5;                        // recent-window return
    double volTerm = std::clamp((volumeSurgePct - 100.0) / 100.0, -1.0, 1.0) * 0.1;   // today's volume surge
    return std::clamp(0.5 + profileTerm + trendTerm + volTerm, 0.05, 0.95);
}
