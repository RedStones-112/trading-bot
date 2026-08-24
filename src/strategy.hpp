#pragma once
#include "broker.hpp"
#include <algorithm>
#include <cmath>
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

// True when the longer-period SMA itself isn't declining (today's SMA(period) vs the same
// SMA `lookback` bars ago) -- a higher-timeframe trend gate layered on top of the raw
// golden cross. smaCrossSignal alone fires on any 5-day/20-day flip, including a brief
// bounce inside an ongoing decline; observed in practice as repeated buy-stopLoss-rebuy
// cycles on the same falling stock even with the rebuy cooldown in place (삼기/다스코,
// 2026-08-10~14, PROGRESS.md). closes must be oldest-first. Not enough history to check
// => true (fail-open, same "정보 없음=중립" convention as the other signals here -- doesn't
// block buys just because a candidate has thin history).
inline bool smaTrendNotFalling(const std::vector<double>& closes, int period, int lookback) {
    if ((int)closes.size() < period + lookback) return true;
    auto sma = [&](int endExclusive) {
        double sum = std::accumulate(closes.begin() + (endExclusive - period), closes.begin() + endExclusive, 0.0);
        return sum / period;
    };
    int n = (int)closes.size();
    return sma(n) >= sma(n - lookback);
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

enum class TradeOutcome { Win, Loss, None };

// Walks forward from `bars[anchorIdx]` (the hypothetical entry price) up to `lookaheadDays`
// bars, and reports which target the price hits first: Win if the high reaches
// +takeProfitPct before the low reaches -stopLossPct, Loss if the reverse, None if neither
// happens within the horizon. A day whose high *and* low both cross their targets is
// ambiguous (can't tell from a daily bar which one the price touched first intraday) --
// treated as Loss, same conservative "no clean win" call `ml_model.cpp::buildTrainingSet`
// made when this logic lived there before it was extracted for `tools/backtest.cpp` to
// reuse too (2026-08-14, so both places agree on what counts as a win instead of drifting).
inline TradeOutcome resolveTradeOutcome(const std::vector<DailyBar>& bars, int anchorIdx,
                                         double takeProfitPct, double stopLossPct, int lookaheadDays) {
    double anchor = bars[anchorIdx].close;
    for (int d = 1; d <= lookaheadDays && anchorIdx + d < (int)bars.size(); d++) {
        const DailyBar& fut = bars[anchorIdx + d];
        bool tpHit = fut.high > 0 && fut.high >= anchor * (1 + takeProfitPct);
        bool slHit = fut.low > 0 && fut.low <= anchor * (1 - stopLossPct);
        if (tpHit && slHit) return TradeOutcome::Loss;
        if (tpHit) return TradeOutcome::Win;
        if (slHit) return TradeOutcome::Loss;
    }
    return TradeOutcome::None;
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

// One confirmed zigzag reversal point (a swing high or swing low), in the order found.
struct Swing { double price; };

// Classic zigzag: walks `closes` oldest-first and only records a swing once price reverses
// by `reversalPct` from the running extreme -- filters daily noise into the handful of moves
// actually large enough to call a "leg". ponytail: the very first leg's direction is
// undetermined until the first reversalPct move away from closes.front(), so a run that
// wobbles under that threshold the whole time yields zero swings (caller falls back to
// neutral) rather than mis-guessing which way the first leg went.
inline std::vector<Swing> detectSwings(const std::vector<double>& closes, double reversalPct) {
    std::vector<Swing> swings;
    if (closes.size() < 2) return swings;
    bool haveTrend = false, trendUp = false;
    double extreme = closes.front();
    for (size_t i = 1; i < closes.size(); i++) {
        double price = closes[i];
        if (!haveTrend) {
            if (price >= extreme * (1.0 + reversalPct)) { haveTrend = true; trendUp = true; swings.push_back({extreme}); extreme = price; }
            else if (price <= extreme * (1.0 - reversalPct)) { haveTrend = true; trendUp = false; swings.push_back({extreme}); extreme = price; }
        } else if (trendUp) {
            if (price > extreme) extreme = price;
            else if (price <= extreme * (1.0 - reversalPct)) { swings.push_back({extreme}); trendUp = false; extreme = price; }
        } else {
            if (price < extreme) extreme = price;
            else if (price >= extreme * (1.0 + reversalPct)) { swings.push_back({extreme}); trendUp = true; extreme = price; }
        }
    }
    return swings;
}

// 파동분석(Elliott Wave 계열) 근사치 -- 정식 엘리엇 파동이론은 5-3 파동 문법/파동차수/
// 파동간 피보나치 길이 비율까지 검증하는 주관적인 기법이라 알고리즘으로 통일된 "정답"이
// 없음(ponytail: 정식 파동 카운터가 필요해지면 교체, 지금은 그중 계산 가능한 부분만 씀).
// 여기서는 지그재그로 유의미한 스윙만 추려서 마지막으로 확정된 파동(swing A -> swing B)의
// 방향을 추세로 보고, 지금 가격이 그 파동을 얼마나 되돌렸는지(피보나치 되돌림 구간)로
// 판단: A->B가 상승파동이었는데 B를 다시 돌파하면 상승파동(3파 유사) 지속으로 보고 크게
// 반영, 38.2~61.8% 되돌림이면 건강한 조정(2파/4파 유사)으로 보고 소폭 반영, A를 뚫고
// 내려가면 파동 구조 자체가 무효화된 것으로 보고 비관적으로 반영. 하락파동은 대칭으로
// 처리. bars는 오래된 순, currentPrice는 지금 이 순간의 실시간가(아직 확정 스윙에
// 포함 안 됨).
inline double probabilityFromWaveAnalysis(const std::vector<DailyBar>& bars, double currentPrice) {
    std::vector<double> closes;
    closes.reserve(bars.size());
    for (auto& bar : bars) closes.push_back(bar.close);

    const double kReversalPct = 0.03; // 3% -- 일봉 노이즈를 걸러내는 임계값, 튜닝 대상
    auto swings = detectSwings(closes, kReversalPct);
    if (swings.size() < 2) return 0.5; // 유의미한 파동 구조를 못 찾음 -- 중립

    double a = swings[swings.size() - 2].price; // 마지막으로 확정된 파동의 시작점
    double b = swings.back().price;              // 마지막으로 확정된 파동의 끝점
    double legSize = std::fabs(b - a);
    if (legSize <= 0) return 0.5;

    if (b > a) { // 마지막 확정 파동이 상승
        if (currentPrice > b) return 0.80; // 직전 고점을 다시 돌파 -- 상승파동 지속(3파 유사)
        double retracement = (b - currentPrice) / legSize;
        if (retracement <= 0.382) return 0.65; // 얕은 되돌림 -- 추세 재개 가능성 높음
        if (retracement <= 0.618) return 0.55; // 피보나치 되돌림 구간 -- 여전히 건강한 조정
        if (retracement < 1.0) return 0.40;    // 깊은 되돌림 -- 파동 구조가 약해 보임
        return 0.20;                            // 시작점(A)까지 되돌림 -- 구조 무효화
    } else { // 마지막 확정 파동이 하락
        if (currentPrice < b) return 0.20; // 직전 저점을 다시 하회 -- 하락파동 지속
        double retracement = (currentPrice - b) / legSize;
        if (retracement <= 0.382) return 0.35;
        if (retracement <= 0.618) return 0.45;
        if (retracement < 1.0) return 0.55;
        return 0.65; // 시작점(A)까지 반등 -- 하락구조 무효화
    }
}

// Resamples daily closes (oldest-first) into one close per `daysPerWeek` trading days (the
// last daily close in each block -- lines up with how a KRX weekly candle's close forms),
// then returns a `weeksPeriod`-week SMA over those weekly closes, oldest-first. KIS has no
// weekly-bar endpoint this bot already calls, and this tracks a real weekly chart's shape
// closely enough for a slope read without a separate API call (ponytail: swap for KIS's
// 주봉 endpoint if this ever needs to match a published weekly chart exactly). Returns an
// empty vector if there isn't at least `weeksPeriod` weeks of data yet.
inline std::vector<double> weeklySmaSeries(const std::vector<double>& dailyCloses, int weeksPeriod, int daysPerWeek = 5) {
    std::vector<double> weeklyCloses;
    for (size_t i = daysPerWeek - 1; i < dailyCloses.size(); i += daysPerWeek)
        weeklyCloses.push_back(dailyCloses[i]);

    std::vector<double> result;
    if ((int)weeklyCloses.size() < weeksPeriod) return result;
    double sum = 0.0;
    for (int i = 0; i < weeksPeriod; i++) sum += weeklyCloses[i];
    result.push_back(sum / weeksPeriod);
    for (size_t i = weeksPeriod; i < weeklyCloses.size(); i++) {
        sum += weeklyCloses[i] - weeklyCloses[i - weeksPeriod];
        result.push_back(sum / weeksPeriod);
    }
    return result;
}

// "focus" 모드(사용자가 직접 지정한 종목만 관찰하는 모드) 전용 시그널 -- 골든/데드크로스
// 대신 이동평균선(주)의 기울기 "모양"을 봄. curvature = 이번 구간 기울기 - 직전 구간
// 기울기(양수면 기울기가 위로 휘어짐, 즉 하락폭이 줄거나 상승폭이 줄어드는 방향). 아직
// 하락 중(slope<0)인데 curvature>0(하락폭이 줄어들며 완만해지는 중)이면 비중 확대,
// 아직 상승 중(slope>0)인데 curvature<0(상승폭이 줄어들며 완만해지는 중)이면 비중 축소.
// 같은 방향으로 더 가팔라지는 중이거나(구조상 아직 바닥/천장 신호가 아님) 데이터 부족
// (weeklySmaSeries가 3개 미만을 돌려줌)이면 Hold.
enum class FocusAction { ScaleIn, ScaleOut, Hold };

inline FocusAction focusWeeklySlopeSignal(const std::vector<double>& weeklyMa) {
    if (weeklyMa.size() < 3) return FocusAction::Hold;
    size_t n = weeklyMa.size();
    double slopeNow = weeklyMa[n - 1] - weeklyMa[n - 2];
    double slopePrev = weeklyMa[n - 2] - weeklyMa[n - 3];
    double curvature = slopeNow - slopePrev;
    if (slopeNow < 0 && curvature > 0) return FocusAction::ScaleIn;
    if (slopeNow > 0 && curvature < 0) return FocusAction::ScaleOut;
    return FocusAction::Hold;
}
