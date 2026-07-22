#pragma once
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
