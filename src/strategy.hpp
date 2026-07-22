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
