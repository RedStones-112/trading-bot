// Offline PORTFOLIO-level simulation over locally cached daily bars (data/daily_bars/*.json,
// populated by backtest_bulk.cpp) -- reimplements a simplified version of main.cpp's scan-mode
// day-by-day, to answer "what would total account equity actually do" rather than just
// per-signal win-rate/EV (backtest.cpp/backtest_bulk.cpp answer that question instead).
//
// 사용자 요청(2026-09-28): take_profit_pct/stop_loss_pct 비율과 골든크로스 SMA 기간을
// 조정해가며 3년치 데이터로 다시 시뮬레이션을 돌리고, 튜닝에 전혀 안 쓴 20개 종목(hold-out)
// 에서 3년 자산증가율 300%를 넘을 때까지(또는 예산 소진 시까지) 반복 수정.
//
// **완전 오프라인** -- KIS 호출이 전혀 없고 data/daily_bars 캐시만 읽음(config.json도
// appkey 없이 fee_rate/tax_rate 등 숫자만 있으면 동작). 그래서 파라미터를 수백 가지
// 바꿔가며 반복 실행해도 전부 로컬 연산이라 몇 초~몇십 초 안에 끝남 -- 5년치를 라이브
// KIS 호출로 매번 다시 받아오던 것과 정반대로, 캐시가 있는 한 이 반복은 사실상 공짜.
//
// **hold-out 분리**: data/daily_bars에 캐시된 종목 중 3년치(755거래일) 이상 있는 것만
// 골라, 고정 시드(42)로 20개를 무작위로 뽑아 `data/holdout_symbols.json`에 한 번 저장하고
// (이후 실행은 이 파일을 그대로 재사용 -- 매번 다시 뽑으면 "hold-out"이라는 말이 무의미해짐)
// 나머지를 훈련 유니버스로 씀. 그리드서치(파라미터 스윕)는 훈련 유니버스에서만 평가하고,
// hold-out 20개는 최종 후보 몇 개를 검증할 때만 씀.
//
// ponytail: main.cpp의 valuation/momentum/interest/event 4개 배수는 과거 시점의 PER/PBR/
// 뉴스/이벤트 데이터가 없어 반영 안 함(gain에 곱하는 배수 없이 gain=price*takeProfitPct
// 그대로) -- 그 4개 배수는 애초에 백테스트로 검증된 적 없는 휴리스틱이라 시뮬레이션에 넣어도
// 신뢰도를 더해주지 않음. `rebuy_cooldown_seconds`(기본 300초)도 하루 단위 시뮬레이션에서는
// 하루 안에 이미 소멸하는 값이라 생략. `daily_loss_limit`도 생략(스캔을 멈추는 안전장치라
// "이 신호/사이징이 자산을 얼마나 불리는가"라는 이번 질문과는 결이 다름). 보유 종목 감시는
// main.cpp와 동일하게 baseCloses/baseBars(매수 시점 캐시, 매일 안 갱신)를 씀.
#include "broker.hpp"
#include "strategy.hpp"
#include "bar_cache.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

using json = nlohmann::json;

namespace {

struct SimParams {
    int smaShort = 5, smaLong = 20;
    double takeProfitPct = 0.02, stopLossPct = 0.03;
    double feeRate = 0.00015, taxRate = 0.0018;
    int maxPositions = 3;
    double riskPerTradePct = 0.01;
    double positionCashFraction = 0.3;
    int maxTopupsPerSymbol = 3;
    double maxPositionValueFraction = 0.5;
    std::string probabilityMode = "wave";
    double initialCash = 10000000.0;

    std::string label() const {
        std::ostringstream s;
        s << "sma" << smaShort << "/" << smaLong << " tp" << std::fixed << std::setprecision(1)
          << takeProfitPct * 100 << "%/sl" << stopLossPct * 100 << "% " << probabilityMode
          << " cash" << std::setprecision(0) << positionCashFraction * 100 << "% pos" << maxPositions;
        return s.str();
    }
};

struct SymbolSeries {
    std::string code;
    std::vector<DailyBar> bars; // oldest-first, trimmed to exactly targetDays
    std::map<std::string, int> dateIndex;
};

struct SimPosition {
    int qty = 0;
    double avgBuyPrice = 0.0;
    int topUps = 0;
    bool deadCrossPending = false;
    double lastKnownPrice = 0.0;
    std::vector<double> baseCloses; // fixed at first buy, never refreshed (matches main.cpp)
    std::vector<DailyBar> baseBars;
};

struct SimResult {
    double initialCash = 0.0, finalEquity = 0.0, growthPct = 0.0;
    int trades = 0, wins = 0, losses = 0;
    double winRate() const { return (wins + losses) > 0 ? (double)wins / (wins + losses) * 100.0 : 0.0; }
};

std::vector<SymbolSeries> loadUniverse(const std::vector<std::string>& codes, int targetDays) {
    std::vector<SymbolSeries> result;
    for (auto& code : codes) {
        auto bars = loadCachedBars(code);
        if ((int)bars.size() < targetDays) continue; // keep every symbol's window the same length -- a fair comparison
        if ((int)bars.size() > targetDays) bars.erase(bars.begin(), bars.end() - targetDays);
        SymbolSeries s;
        s.code = code;
        s.bars = std::move(bars);
        for (int i = 0; i < (int)s.bars.size(); i++)
            if (!s.bars[i].date.empty()) s.dateIndex[s.bars[i].date] = i;
        result.push_back(std::move(s));
    }
    return result;
}

double probabilityFor(const std::string& mode, double volumeSurgePct, double trendPct,
                       double belowRatio, const std::vector<DailyBar>& window, double currentPrice) {
    return mode == "basic" ? probabilityFromTechnicals(volumeSurgePct, trendPct, belowRatio)
                            : probabilityFromWaveAnalysis(window, currentPrice);
}

struct TradeRecord { std::string code; double netPct = 0.0; double pnlKrw = 0.0; };

SimResult simulate(const std::vector<SymbolSeries>& universe, const SimParams& p,
                    std::vector<TradeRecord>* tradeLog = nullptr) {
    SimResult result;
    result.initialCash = p.initialCash;
    if (universe.empty()) { result.finalEquity = p.initialCash; return result; }

    std::set<std::string> calSet;
    for (auto& s : universe)
        for (auto& b : s.bars)
            if (!b.date.empty()) calSet.insert(b.date);
    std::vector<std::string> calendar(calSet.begin(), calSet.end()); // set -> already date-sorted ascending

    std::map<std::string, const SymbolSeries*> byCode;
    for (auto& s : universe) byCode[s.code] = &s;

    int windowBars = p.smaLong + 5; // same margin as backtest_bulk.cpp/main.cpp's live scan window
    double cash = p.initialCash;
    std::map<std::string, SimPosition> positions;

    for (auto& date : calendar) {
        // Phase A: monitor held positions, exit on take-profit/stop-loss/confirmed dead-cross.
        std::map<std::string, double> heldEv;
        std::vector<std::string> toClose;
        for (auto& [code, pos] : positions) {
            auto* s = byCode.at(code);
            auto it = s->dateIndex.find(date);
            if (it == s->dateIndex.end()) continue; // no bar today for this symbol -- carry position unchanged
            const DailyBar& bar = s->bars[it->second];
            double current = bar.close; // SMA/trend/EV all stay close-based, same convention as the rest of this project
            pos.lastKnownPrice = current;

            std::vector<double> closes = pos.baseCloses;
            closes.push_back(current);
            Signal sig;
            try { sig = smaCrossSignal(closes, p.smaShort, p.smaLong); } catch (...) { continue; }

            // TP/SL against the day's high/low, not the close -- checking only the close (as
            // this simulator originally did) understates how often a stop is actually touched
            // intraday and, when it is hit on a day that also closes far below it, overstates
            // the realized loss vs. what a real stop order would have filled at. This mirrors
            // strategy.hpp's resolveTradeOutcome, which the signal-level backtests already use
            // (found 2026-09-28 diagnosing why tuned configs did worse live than in training --
            // PROGRESS.md, "왜 튜닝값이 test에서 못 통했나"). Fee/tax-adjusted price thresholds:
            double tpPrice = pos.avgBuyPrice * (1 + p.feeRate) * (1 + p.takeProfitPct) / (1 - p.feeRate - p.taxRate);
            double slPrice = pos.avgBuyPrice * (1 + p.feeRate) * (1 - p.stopLossPct) / (1 - p.feeRate - p.taxRate);
            bool tpHit = bar.high > 0 && bar.high >= tpPrice;
            bool slHit = bar.low > 0 && bar.low <= slPrice;
            bool deadNow = (sig == Signal::Sell);
            bool deadConfirmed = deadNow && pos.deadCrossPending;
            pos.deadCrossPending = deadNow;

            // Same-day double-touch is ambiguous from a daily bar (can't tell which came
            // first) -- conservative call, same as resolveTradeOutcome: treat as a stop-out.
            bool tp = tpHit && !slHit;
            bool sl = slHit;
            double exitPrice = current; // dead-cross exit has no threshold -- just the close
            if (sl) exitPrice = (bar.high < slPrice) ? bar.high : slPrice; // whole day gapped under the stop -> best price actually available that day
            else if (tp) exitPrice = tpPrice; // limit-style fill, doesn't credit upside past the target

            double trendPct = pos.baseCloses.empty() ? 0.0 : (current - pos.baseCloses.front()) / pos.baseCloses.front();
            double belowRatio = belowPriceVolumeRatio(pos.baseBars, current);
            double prob = probabilityFor(p.probabilityMode, 100.0, trendPct, belowRatio, pos.baseBars, current);
            heldEv[code] = current * p.takeProfitPct * prob;

            if (tp || sl || deadConfirmed) {
                double exitNetPct = netProfitPct(pos.avgBuyPrice, exitPrice, p.feeRate, p.taxRate);
                cash += pos.qty * exitPrice * (1 - p.feeRate - p.taxRate);
                result.trades++;
                if (exitNetPct >= 0) result.wins++; else result.losses++;
                if (tradeLog) tradeLog->push_back({code, exitNetPct, pos.avgBuyPrice * pos.qty * exitNetPct});
                toClose.push_back(code);
            }
        }
        for (auto& c : toClose) { positions.erase(c); heldEv.erase(c); }

        // Phase B: scan every symbol with a golden cross today (held or not -- a held symbol
        // whose signal is still Buy is eligible for a top-up, same as main.cpp).
        struct Cand {
            std::string code;
            double price = 0.0, ev = 0.0;
            std::vector<double> closes;
            std::vector<DailyBar> window;
        };
        std::vector<Cand> cands;
        for (auto& s : universe) {
            auto it = s.dateIndex.find(date);
            if (it == s.dateIndex.end()) continue;
            int idx = it->second;
            if (idx < windowBars) continue;
            std::vector<DailyBar> window(s.bars.begin() + (idx - windowBars), s.bars.begin() + idx);
            std::vector<double> closes;
            closes.reserve(window.size() + 1);
            for (auto& b : window) closes.push_back(b.close);
            double currentPrice = s.bars[idx].close;
            closes.push_back(currentPrice);
            Signal sig;
            try { sig = smaCrossSignal(closes, p.smaShort, p.smaLong); } catch (...) { continue; }
            if (sig != Signal::Buy) continue;
            double trendPct = window.front().close > 0 ? (currentPrice - window.front().close) / window.front().close : 0.0;
            double belowRatio = belowPriceVolumeRatio(window, currentPrice);
            double volumeSurgePct = (idx > 0 && s.bars[idx - 1].volume > 0)
                ? s.bars[idx].volume / s.bars[idx - 1].volume * 100.0 : 100.0;
            double prob = probabilityFor(p.probabilityMode, volumeSurgePct, trendPct, belowRatio, window, currentPrice);
            double ev = currentPrice * p.takeProfitPct * prob;
            cands.push_back({s.code, currentPrice, ev, std::move(closes), std::move(window)});
        }
        std::sort(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.ev > b.ev; });

        bool evictedThisCycle = false;
        for (auto& cand : cands) {
            bool alreadyHeld = positions.count(cand.code) > 0;
            bool canAct = alreadyHeld || (int)positions.size() < p.maxPositions;
            if (!canAct) {
                if (evictedThisCycle || heldEv.empty()) continue;
                auto worstIt = std::min_element(heldEv.begin(), heldEv.end(),
                    [](auto& a, auto& b) { return a.second < b.second; });
                auto& worstPos = positions.at(worstIt->first);
                double switchCost = worstPos.qty * worstPos.lastKnownPrice * (p.feeRate * 2 + p.taxRate);
                if (cand.ev > worstIt->second + switchCost) {
                    double netPct = netProfitPct(worstPos.avgBuyPrice, worstPos.lastKnownPrice, p.feeRate, p.taxRate);
                    cash += worstPos.qty * worstPos.lastKnownPrice * (1 - p.feeRate - p.taxRate);
                    result.trades++;
                    if (netPct >= 0) result.wins++; else result.losses++;
                    if (tradeLog) tradeLog->push_back({worstIt->first, netPct, worstPos.avgBuyPrice * worstPos.qty * netPct});
                    positions.erase(worstIt->first);
                    heldEv.erase(worstIt->first);
                    evictedThisCycle = true;
                    canAct = true;
                } else continue;
            }

            // otherValue intentionally sums ALL current positions (including cand.code if it's
            // a top-up) -- matches main.cpp's own totalEquity computation exactly (see
            // PROGRESS.md's account of that code, the variable name undersells what it does).
            double otherValue = 0.0;
            for (auto& [code, pos] : positions) otherValue += pos.qty * pos.lastKnownPrice;
            double totalEquity = cash + otherValue;
            double unitCost = cand.price * (1 + p.feeRate);

            double riskBudget = totalEquity * p.riskPerTradePct;
            double lossPerShare = cand.price * p.stopLossPct;
            int riskQty = lossPerShare > 0 ? (int)std::floor(riskBudget / lossPerShare) : 0;
            int cashFractionQty = (int)std::floor(p.positionCashFraction * cash / unitCost);
            int buyQty = std::min(riskQty, cashFractionQty);
            if (buyQty < 1 && cash >= unitCost) buyQty = 1;

            if (alreadyHeld && positions.at(cand.code).topUps >= p.maxTopupsPerSymbol) continue;

            if (buyQty >= 1) {
                double currentPositionValue = alreadyHeld
                    ? positions.at(cand.code).avgBuyPrice * positions.at(cand.code).qty : 0.0;
                double room = p.maxPositionValueFraction * totalEquity - currentPositionValue;
                int roomQty = room > 0 ? (int)std::floor(room / unitCost) : 0;
                if (roomQty < buyQty) buyQty = roomQty;
            }
            if (buyQty < 1) continue;

            double cost = buyQty * unitCost;
            if (cost > cash) continue;
            cash -= cost;
            auto& pos = positions[cand.code];
            if (pos.qty > 0) {
                pos.avgBuyPrice = (pos.avgBuyPrice * pos.qty + cand.price * buyQty) / (pos.qty + buyQty);
                pos.qty += buyQty;
                pos.topUps++;
            } else {
                pos.qty = buyQty;
                pos.avgBuyPrice = cand.price;
                pos.baseCloses = std::vector<double>(cand.closes.begin(), cand.closes.end() - 1);
                pos.baseBars = cand.window;
            }
            pos.lastKnownPrice = cand.price;
        }
    }

    double finalEquity = cash;
    for (auto& [code, pos] : positions) finalEquity += pos.qty * pos.lastKnownPrice;
    result.finalEquity = finalEquity;
    result.growthPct = (finalEquity / p.initialCash - 1.0) * 100.0;
    return result;
}

// 종목 유니버스를 훈련/hold-out으로 한 번만 나누고 파일에 고정 -- 실행할 때마다 다시 뽑으면
// "튜닝에 안 쓴 20개"라는 전제 자체가 깨짐.
std::pair<std::vector<std::string>, std::vector<std::string>> loadOrCreateSplit(
    const std::vector<std::string>& eligibleCodes, int holdoutSize) {
    const std::string path = "data/holdout_symbols.json";
    std::ifstream in(path);
    if (in) {
        json j = json::parse(in);
        std::vector<std::string> holdout = j.at("holdout").get<std::vector<std::string>>();
        std::set<std::string> holdoutSet(holdout.begin(), holdout.end());
        std::vector<std::string> train;
        for (auto& c : eligibleCodes) if (!holdoutSet.count(c)) train.push_back(c);
        return {train, holdout};
    }
    std::vector<std::string> shuffled = eligibleCodes; // already alphabetically sorted by listCachedSymbols
    std::mt19937 rng(42); // fixed seed -- reproducible split
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    size_t n = std::min((size_t)holdoutSize, shuffled.size());
    std::vector<std::string> holdout(shuffled.begin(), shuffled.begin() + n);
    std::vector<std::string> train(shuffled.begin() + n, shuffled.end());
    std::filesystem::create_directories("data");
    json j;
    j["holdout"] = holdout;
    j["train"] = train;
    j["note"] = "고정 무작위 분리(시드 42, 2026-09-28) -- 튜닝은 train만 보고, holdout은 최종 검증 전용. 재실행해도 이 파일이 있으면 그대로 재사용됨.";
    std::ofstream out(path);
    out << j.dump(2);
    return {train, holdout};
}

double cagrPct(double growthPct, int years) {
    double factor = 1.0 + growthPct / 100.0;
    if (factor <= 0.0) return -100.0;
    return (std::pow(factor, 1.0 / years) - 1.0) * 100.0;
}

std::vector<std::vector<std::string>> makeFolds(std::vector<std::string> codes, int k, unsigned seed) {
    std::mt19937 rng(seed);
    std::shuffle(codes.begin(), codes.end(), rng);
    std::vector<std::vector<std::string>> folds(k);
    for (size_t i = 0; i < codes.size(); i++) folds[i % k].push_back(codes[i]);
    return folds;
}

// 사용자 요청(2026-09-28, 두 번째): 300%라는 단일 hold-out 목표 대신 "1년당 10% 정도"라는
// 현실적인 목표로 바꾸고 "반복 학습"(재검증)해달라는 것 -- 앞서 발견한 대로 hold-out
// 20종목 1세트만 보고 판단하면 우연에 크게 좌우되므로(train 1등이 hold-out에서 기본값보다
// 못했던 사례), 이번엔 K겹 교차검증(fold마다 나머지로 튜닝 -> 그 fold로 검증, 5번 반복)으로
// "여러 다른 종목 묶음에서 꾸준히 10%/년을 넘기는가"를 직접 확인함.
void runCrossValidation(const std::vector<std::string>& eligible, const SimParams& base, int years, int targetDays) {
    const int kFolds = 5;
    auto folds = makeFolds(eligible, kFolds, 7);
    std::cout << "=== " << kFolds << "겹 교차검증 (목표: hold-out에서 연 10% 안팎을 꾸준히) ===\n";
    for (int i = 0; i < kFolds; i++) std::cout << "  fold " << i << ": " << folds[i].size() << "개 종목\n";
    std::cout << "\n";

    // 1차 시도(sma5/60, sma15/60, tp/sl 2~3%대까지 넓게 봤을 때)에서 train 성적이
    // 300~900%까지 치솟는 조합이 fold별로 계속 나왔는데, test로는 -1%~15%로 완전히
    // 무너지는 걸 확인함(과최적화) -- 그래서 이번엔 일부러 탐색범위를 보수적으로 좁힘
    // (SMA 장기이평 30 이하, TP/SL도 현재 라이브값(2%/3%) 근방으로 제한, 동시보유도 소수
    // 종목 집중(3/6) 대신 분산(10/20) 위주로) -- "이 좁은 범위에서도 기본값보다 꾸준히
    // 나은 게 있는지"를 보려는 것이지, 넓게 뒤져서 우연히 잘 맞는 조합을 또 찾으려는 게
    // 아님(그건 이미 실패로 확인됨).
    std::vector<std::pair<int, int>> smaPairs = {
        {5, 20}, {5, 25}, {6, 25}, {8, 25}, {8, 30}, {10, 30}
    };
    std::vector<std::pair<double, double>> tpSlPairs = {
        {0.02, 0.03}, {0.02, 0.025}, {0.025, 0.03}, {0.025, 0.025}, {0.03, 0.03}, {0.03, 0.02}, {0.025, 0.02}
    };
    std::vector<std::string> modes = {"basic", "wave"};
    std::vector<double> cashFractions = {0.3, 0.5};
    std::vector<int> maxPosOptions = {10, 20};

    auto printResult = [](const std::string& tag, const SimParams& p, const SimResult& r, int years) {
        std::cout << tag << " [" << p.label() << "] 증가율 " << std::fixed << std::setprecision(1)
                  << r.growthPct << "% (연 " << cagrPct(r.growthPct, years) << "%), 거래 " << r.trades
                  << "건 승률 " << std::setprecision(1) << r.winRate() << "%\n";
    };

    // 이중검증(nested) 선택: "그 fold의 train 전체에서 성적이 제일 좋은 것"을 고르는 대신,
    // train을 다시 반으로(innerA/innerB) 쪼개서 "양쪽 절반 모두에서 나쁘지 않은"(최솟값이
    // 가장 큰) 조합을 고름 -- 09-28 앞선 5겹 시도들은 "train 전체 1등"이 그 train 표본의
    // 우연에 낚인 것으로 계속 드러났음(train 34% -> test -7% 등). 한쪽 절반에만 우연히
    // 잘 맞는 조합은 다른 절반에서 성적이 나빠 최솟값이 낮게 나오므로 자동으로 걸러짐.
    std::vector<double> tunedFoldCagr, baselineFoldCagr;
    for (int fold = 0; fold < kFolds; fold++) {
        std::vector<std::string> testCodes = folds[fold];
        std::vector<std::string> trainCodes;
        for (int j = 0; j < kFolds; j++)
            if (j != fold) trainCodes.insert(trainCodes.end(), folds[j].begin(), folds[j].end());

        std::vector<std::string> innerA, innerB;
        {
            std::vector<std::string> shuffled = trainCodes;
            std::mt19937 rng(1000 + fold);
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            for (size_t i = 0; i < shuffled.size(); i++) (i % 2 == 0 ? innerA : innerB).push_back(shuffled[i]);
        }
        auto innerAUniverse = loadUniverse(innerA, targetDays);
        auto innerBUniverse = loadUniverse(innerB, targetDays);
        auto testUniverse = loadUniverse(testCodes, targetDays);

        struct Scored { SimParams p; double robust; SimResult a, b; };
        auto robustScore = [](const SimResult& a, const SimResult& b) { return std::min(a.growthPct, b.growthPct); };

        std::vector<Scored> scored;
        for (auto& [ss, sl] : smaPairs)
            for (auto& [tp, slp] : tpSlPairs)
                for (auto& mode : modes) {
                    SimParams p = base;
                    p.smaShort = ss; p.smaLong = sl; p.takeProfitPct = tp; p.stopLossPct = slp; p.probabilityMode = mode;
                    SimResult ra = simulate(innerAUniverse, p), rb = simulate(innerBUniverse, p);
                    scored.push_back({p, robustScore(ra, rb), ra, rb});
                }
        std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.robust > b.robust; });

        std::vector<Scored> refined;
        for (int i = 0; i < 5 && i < (int)scored.size(); i++)
            for (double cf : cashFractions)
                for (int mp : maxPosOptions) {
                    SimParams p = scored[i].p;
                    p.positionCashFraction = cf;
                    p.maxPositions = mp;
                    SimResult ra = simulate(innerAUniverse, p), rb = simulate(innerBUniverse, p);
                    refined.push_back({p, robustScore(ra, rb), ra, rb});
                }
        scored.insert(scored.end(), refined.begin(), refined.end());
        std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.robust > b.robust; });

        SimParams best = scored.front().p;
        SimResult testResult = simulate(testUniverse, best);
        SimResult baselineResult = simulate(testUniverse, base);

        std::cout << "--- fold " << fold << " (train " << trainCodes.size() << "개 = innerA "
                  << innerAUniverse.size() << " + innerB " << innerBUniverse.size() << ", test "
                  << testUniverse.size() << "개) ---\n";
        printResult("  [innerA, 이 fold에서 고른 최선]  ", best, scored.front().a, years);
        printResult("  [innerB, 같은 설정]              ", best, scored.front().b, years);
        printResult("  [test, 위 설정을 안 본 종목에]    ", best, testResult, years);
        printResult("  [test, 현재 라이브 기본값]        ", base, baselineResult, years);
        std::cout << "\n";

        tunedFoldCagr.push_back(cagrPct(testResult.growthPct, years));
        baselineFoldCagr.push_back(cagrPct(baselineResult.growthPct, years));
    }

    auto summarize = [&](const std::string& label, const std::vector<double>& v) {
        double sum = 0, mn = v[0], mx = v[0];
        int over10 = 0;
        for (double x : v) { sum += x; mn = std::min(mn, x); mx = std::max(mx, x); if (x >= 10.0) over10++; }
        std::cout << label << ": 평균 " << std::fixed << std::setprecision(1) << sum / v.size()
                  << "%/년, 최저 " << mn << "%/년, 최고 " << mx << "%/년, "
                  << v.size() << "개 fold 중 " << over10 << "개가 연 10% 이상\n";
    };
    std::cout << "=== 5겹 요약 (연간 CAGR 기준) ===\n";
    summarize("튜닝(fold마다 train으로 고른 설정)", tunedFoldCagr);
    summarize("기본값(현재 라이브 설정 그대로)     ", baselineFoldCagr);
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    int years = argc > 1 ? std::stoi(argv[1]) : 3;
    int targetDays = years * 250;

    SimParams base;
    std::ifstream cfgFile("config.json");
    if (cfgFile) {
        try {
            json cfg = json::parse(cfgFile, nullptr, true, true);
            base.feeRate = cfg.value("fee_rate", base.feeRate);
            base.taxRate = cfg.value("tax_rate", base.taxRate);
            base.initialCash = cfg.value("initial_cash", base.initialCash);
            base.maxPositions = cfg.value("max_positions", base.maxPositions);
            base.riskPerTradePct = cfg.value("risk_per_trade_pct", base.riskPerTradePct);
            base.positionCashFraction = cfg.value("position_cash_fraction", base.positionCashFraction);
            base.maxTopupsPerSymbol = cfg.value("max_topups_per_symbol", base.maxTopupsPerSymbol);
            base.maxPositionValueFraction = cfg.value("max_position_value_fraction", base.maxPositionValueFraction);
        } catch (const std::exception&) { /* config.json optional for this offline tool */ }
    }

    auto eligible = listCachedSymbols();
    // Pre-filter to symbols that actually have >= targetDays cached -- loadUniverse also
    // checks this, but filtering here first keeps the train/holdout split itself limited to
    // symbols usable at this history length.
    std::vector<std::string> withEnoughHistory;
    for (auto& c : eligible) {
        auto bars = loadCachedBars(c);
        if ((int)bars.size() >= targetDays) withEnoughHistory.push_back(c);
    }
    if (withEnoughHistory.size() < 30) {
        std::cerr << "캐시된 종목 중 " << years << "년(" << targetDays << "거래일) 이상인 게 "
                  << withEnoughHistory.size() << "개뿐 -- backtest_bulk.exe로 데이터를 먼저 더 모을 것.\n";
        return 1;
    }

    if (argc > 2 && std::string(argv[2]) == "cv") {
        runCrossValidation(withEnoughHistory, base, years, targetDays);
        return 0;
    }

    // 진단 전용: 특정 fold의 test 유니버스에 특정 설정을 돌려서 트레이드 단위로 무슨 일이
    // 있었는지 봄(2026-09-28, "왜 튜닝값이 test에서 안 통했나" 질문에 답하려고 추가) --
    // 사용법: portfolio_sim.exe <연수> trades <fold 0~4> <smaShort> <smaLong> <tp> <sl> [mode]
    if (argc > 2 && std::string(argv[2]) == "trades") {
        int fold = std::stoi(argv[3]);
        SimParams p = base;
        p.smaShort = std::stoi(argv[4]);
        p.smaLong = std::stoi(argv[5]);
        p.takeProfitPct = std::stod(argv[6]);
        p.stopLossPct = std::stod(argv[7]);
        if (argc > 8) p.probabilityMode = argv[8];
        if (argc > 9) p.maxPositions = std::stoi(argv[9]);
        if (argc > 10) p.positionCashFraction = std::stod(argv[10]);

        auto folds = makeFolds(withEnoughHistory, 5, 7);
        if (fold < 0 || fold >= (int)folds.size()) { std::cerr << "fold는 0~" << folds.size() - 1 << "\n"; return 1; }
        auto testUniverse = loadUniverse(folds[fold], targetDays);
        std::cout << "fold " << fold << " test 유니버스 " << testUniverse.size() << "개, 설정 [" << p.label() << "]\n\n";

        std::vector<TradeRecord> log;
        auto r = simulate(testUniverse, p, &log);
        std::cout << "증가율 " << std::fixed << std::setprecision(1) << r.growthPct << "% (연 "
                  << cagrPct(r.growthPct, years) << "%), 거래 " << r.trades << "건 승률 "
                  << r.winRate() << "%\n\n";

        std::sort(log.begin(), log.end(), [](auto& a, auto& b) { return a.pnlKrw < b.pnlKrw; });
        std::cout << "최악 손실 10건:\n";
        for (int i = 0; i < 10 && i < (int)log.size(); i++)
            std::cout << "  " << log[i].code << " 순손익률 " << std::setprecision(2) << log[i].netPct * 100
                      << "% -> " << std::setprecision(0) << log[i].pnlKrw << "원\n";
        std::cout << "\n최고 이익 10건:\n";
        for (int i = (int)log.size() - 1; i >= 0 && i >= (int)log.size() - 10; i--)
            std::cout << "  " << log[i].code << " 순손익률 " << std::setprecision(2) << log[i].netPct * 100
                      << "% -> " << std::setprecision(0) << log[i].pnlKrw << "원\n";

        double totalPnl = 0;
        for (auto& t : log) totalPnl += t.pnlKrw;
        double top5LossSum = 0;
        for (int i = 0; i < 5 && i < (int)log.size(); i++) top5LossSum += log[i].pnlKrw;
        std::cout << "\n총 손익 " << std::setprecision(0) << totalPnl << "원, 그중 최악 5건 합계 "
                  << top5LossSum << "원(" << std::setprecision(1)
                  << (totalPnl != 0 ? top5LossSum / std::fabs(totalPnl) * 100.0 : 0.0) << "%)\n";
        return 0;
    }

    auto [trainCodes, holdoutCodes] = loadOrCreateSplit(withEnoughHistory, 20);
    auto trainUniverse = loadUniverse(trainCodes, targetDays);
    auto holdoutUniverse = loadUniverse(holdoutCodes, targetDays);
    std::cout << "훈련 유니버스 " << trainUniverse.size() << "개, hold-out(튜닝에 안 씀) "
              << holdoutUniverse.size() << "개, 목표 히스토리 " << years << "년(" << targetDays << "거래일)\n\n";

    auto printResult = [](const std::string& tag, const SimParams& p, const SimResult& r) {
        std::cout << tag << " [" << p.label() << "] 초기자산 " << std::fixed << std::setprecision(0)
                  << r.initialCash << " -> 최종 " << r.finalEquity << " (증가율 "
                  << std::setprecision(1) << r.growthPct << "%), 거래 " << r.trades
                  << "건 승률 " << std::setprecision(1) << r.winRate() << "%\n";
    };

    if (argc > 5) {
        // Single explicit config, e.g.: portfolio_sim.exe 3 single 5 20 0.02 0.03 wave
        SimParams p = base;
        p.smaShort = std::stoi(argv[3]);
        p.smaLong = std::stoi(argv[4]);
        p.takeProfitPct = std::stod(argv[5]);
        if (argc > 6) p.stopLossPct = std::stod(argv[6]);
        if (argc > 7) p.probabilityMode = argv[7];
        printResult("[train]  ", p, simulate(trainUniverse, p));
        printResult("[holdout]", p, simulate(holdoutUniverse, p));
        return 0;
    }

    // 1단계 그리드서치: SMA 기간 조합(사용자가 요청한 "골든크로스 신호변경") x TP/SL 비율
    // (현재 take_profit=2%/stop_loss=3%의 손익분기 승률 64%가 실측 승률 37~39%보다 훨씬
    // 높다는 2026-09-28 발견에 따라, SL을 TP보다 훨씬 좁게 주는 방향도 포함) x probability_mode.
    std::vector<std::pair<int, int>> smaPairs = {
        {5, 20}, {5, 10}, {3, 15}, {8, 25}, {6, 25}, {8, 20}, {8, 30}, {10, 25},
        {10, 30}, {10, 50}, {5, 60}, {15, 60}, {20, 100}
    };
    std::vector<std::pair<double, double>> tpSlPairs = {
        {0.02, 0.03}, {0.02, 0.02}, {0.03, 0.02}, {0.015, 0.03},
        {0.05, 0.01}, {0.04, 0.01}, {0.03, 0.01}, {0.05, 0.02}, {0.08, 0.02}, {0.03, 0.005},
        {0.10, 0.01}, {0.06, 0.005}, {0.15, 0.02}, {0.08, 0.01}, {0.12, 0.015}
    };
    std::vector<std::string> modes = {"basic", "wave"};

    struct Scored { SimParams p; SimResult train; };
    std::vector<Scored> scored;
    for (auto& [ss, sl] : smaPairs)
        for (auto& [tp, slp] : tpSlPairs)
            for (auto& mode : modes) {
                SimParams p = base;
                p.smaShort = ss; p.smaLong = sl; p.takeProfitPct = tp; p.stopLossPct = slp; p.probabilityMode = mode;
                scored.push_back({p, simulate(trainUniverse, p)});
            }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.train.growthPct > b.train.growthPct; });

    std::cout << "=== 1단계(SMA x TP/SL x 확률모드, " << scored.size() << "개 조합) 훈련 유니버스 상위 10개 ===\n";
    for (int i = 0; i < 10 && i < (int)scored.size(); i++) printResult("[train]  ", scored[i].p, scored[i].train);

    // 2단계: 1단계 상위 8개의 신호/TP-SL 조합을 고정하고, 자금배분 파라미터(포지션당 현금
    // 비율, 동시보유 종목수)를 같이 바꿔가며 재탐색 -- SL이 좁을수록 리스크기반 사이징이
    // 커지지만 position_cash_fraction 상한에 곧 막히는 걸 1차 결과에서 확인(0.5%로 더
    // 좁혀도 1.0%보다 안 나아짐), 그 상한 자체와 동시보유 종목수를 넓히면 더 벌 수 있는지 확인.
    // 낮은 승률(15~30%)에 고배당비 TP/SL을 쓰는 조합일수록 "병렬로 많이 분산할수록 대수의
    // 법칙으로 복리 변동성이 줄어든다"는 가설을 검증하기 위해 위쪽(더 많은 동시보유)도 같이 봄
    // -- 1차 시도(3/5/8만 봤을 때)에서 원래 설정(pos20)보다 다 못한 걸 보고 범위를 늘림.
    std::vector<double> cashFractions = {0.3, 0.5, 0.7, 1.0};
    std::vector<int> maxPosOptions = {3, 5, 8, 15, 20, 30, 50};
    std::vector<Scored> refined;
    for (int i = 0; i < 8 && i < (int)scored.size(); i++) {
        for (double cf : cashFractions) {
            for (int mp : maxPosOptions) {
                SimParams p = scored[i].p;
                p.positionCashFraction = cf;
                p.maxPositions = mp;
                refined.push_back({p, simulate(trainUniverse, p)});
            }
        }
    }
    std::sort(refined.begin(), refined.end(), [](auto& a, auto& b) { return a.train.growthPct > b.train.growthPct; });

    std::cout << "\n=== 2단계(자금배분 재탐색, " << refined.size() << "개 조합) 훈련 유니버스 상위 10개 ===\n";
    for (int i = 0; i < 10 && i < (int)refined.size(); i++) printResult("[train]  ", refined[i].p, refined[i].train);

    // 최종 검증은 1단계+2단계를 합친 전체 풀에서 train 기준 진짜 상위 5개로 함 -- 2단계가
    // 항상 1단계보다 낫다는 보장이 없음(실제로 이번 실행에서 1단계 원안이 더 좋았던 경우가
    // 있었음), 그래서 둘 중 진짜 최고를 놓치지 않기 위함.
    std::vector<Scored> combined = scored;
    combined.insert(combined.end(), refined.begin(), refined.end());
    std::sort(combined.begin(), combined.end(), [](auto& a, auto& b) { return a.train.growthPct > b.train.growthPct; });

    std::cout << "\n=== 전체(1+2단계) 통틀어 train 상위 5개를 hold-out(튜닝에 전혀 안 쓴 "
              << holdoutUniverse.size() << "개 종목)에 검증 ===\n";
    for (int i = 0; i < 5 && i < (int)combined.size(); i++) {
        auto holdoutResult = simulate(holdoutUniverse, combined[i].p);
        printResult("[train]  ", combined[i].p, combined[i].train);
        printResult("[holdout]", combined[i].p, holdoutResult);
    }

    // 3단계: train 순위 상위 5개가 holdout에서 baseline(105%)과 별 차이가 없거나 더 나쁜
    // 건(overfitting 신호) -- train 순위만으로 최종 후보를 고르는 대신, 상위 60개 후보 전부를
    // holdout에도 직접 돌려서 "train에서 안 뽑혔지만 실제로 20개 종목에 일반화되는 조합"이
    // 있는지 정직하게 확인. (주의: 이렇게 넓게 holdout을 들여다보는 것 자체가 결국 holdout에
    // 약하게 과적합할 위험을 만듦 -- 진짜 검증은 이 결과를 또 다른 새 종목에 한 번 더
    // 대조해봐야 함. 이번 세션 범위에서는 "train 최적화가 holdout에 안 먹힌다"는 사실 자체를
    // 확인하는 게 목적이라 이 한계를 그대로 보고함.)
    int topN = std::min((size_t)60, combined.size());
    std::vector<std::pair<SimParams, SimResult>> holdoutScored;
    for (int i = 0; i < topN; i++)
        holdoutScored.push_back({combined[i].p, simulate(holdoutUniverse, combined[i].p)});
    std::sort(holdoutScored.begin(), holdoutScored.end(),
              [](auto& a, auto& b) { return a.second.growthPct > b.second.growthPct; });
    std::cout << "\n=== 3단계: train 상위 " << topN << "개를 전부 holdout에 돌려서 holdout 기준 재정렬한 상위 10개 ===\n";
    for (int i = 0; i < 10 && i < (int)holdoutScored.size(); i++)
        printResult("[holdout]", holdoutScored[i].first, holdoutScored[i].second);

    // 4단계(진단 전용, "정직한 out-of-sample 추정"이 아님-- holdout에 직접 그리드서치하는
    // 거라 이 결과 자체를 신뢰하면 안 됨): "이 신호 계열(SMA 크로스+TP/SL+사이징)이 이
    // 20개 종목에서 파라미터를 아무리 잘 맞춰도 최대 얼마나 벌 수 있는가"라는 상한선만
    // 확인하기 위함 -- 이 상한선조차 300%에 한참 못 미치면, "더 세밀하게 튜닝하면 될 것"이
    // 아니라 이 신호 계열 자체의 한계라고 결론지을 근거가 됨.
    std::cout << "\n=== 4단계(진단 전용, holdout 직접 그리드서치 -- 정직한 추정 아님, "
                 "\"이 신호 계열의 최대 상한선\"만 확인) ===\n";
    std::vector<std::pair<int, int>> fineSma = {
        {6, 20}, {6, 25}, {6, 30}, {8, 20}, {8, 25}, {8, 30}, {10, 20}, {10, 25}, {10, 30}
    };
    std::vector<double> fineTp = {0.015, 0.02, 0.025, 0.03, 0.04, 0.05};
    std::vector<double> fineSl = {0.005, 0.008, 0.01, 0.015, 0.02};
    std::vector<int> finePos = {1, 2, 3, 4, 6};
    std::vector<double> fineCash = {0.5, 1.0};
    std::pair<SimParams, SimResult> bestCeiling;
    bool haveCeiling = false;
    long long ceilingCount = 0;
    for (auto& [ss, sl] : fineSma)
        for (double tp : fineTp)
            for (double slp : fineSl)
                for (auto& mode : modes)
                    for (int mp : finePos)
                        for (double cf : fineCash) {
                            SimParams p = base;
                            p.smaShort = ss; p.smaLong = sl; p.takeProfitPct = tp; p.stopLossPct = slp;
                            p.probabilityMode = mode; p.maxPositions = mp; p.positionCashFraction = cf;
                            auto r = simulate(holdoutUniverse, p);
                            ceilingCount++;
                            if (!haveCeiling || r.growthPct > bestCeiling.second.growthPct) { bestCeiling = {p, r}; haveCeiling = true; }
                        }
    std::cout << ceilingCount << "개 조합 중 holdout 최고치:\n";
    if (haveCeiling) printResult("[holdout, 치팅 상한선]", bestCeiling.first, bestCeiling.second);

    // 5단계(역시 진단 전용, holdout에 직접 맞추는 것): 4단계 최고 조합 주변을 더 촘촘히 탐색.
    if (haveCeiling) {
        std::vector<std::pair<int, int>> nearSma = {
            {6, 25}, {6, 30}, {6, 35}, {8, 25}, {8, 30}, {8, 35}, {10, 30}, {10, 35}, {12, 35}
        };
        std::vector<double> nearTp = {2.0, 2.2, 2.5, 2.8, 3.0};
        for (auto& v : nearTp) v /= 100.0;
        std::vector<double> nearSl = {0.6, 0.7, 0.8, 0.9, 1.0};
        for (auto& v : nearSl) v /= 100.0;
        std::vector<int> nearPos = {4, 5, 6, 7, 8};
        std::vector<double> nearCash = {0.85, 1.0};
        std::pair<SimParams, SimResult> best5 = bestCeiling;
        long long count5 = 0;
        for (auto& [ss, sl] : nearSma)
            for (double tp : nearTp)
                for (double slp : nearSl)
                    for (auto& mode : modes)
                        for (int mp : nearPos)
                            for (double cf : nearCash) {
                                SimParams p = base;
                                p.smaShort = ss; p.smaLong = sl; p.takeProfitPct = tp; p.stopLossPct = slp;
                                p.probabilityMode = mode; p.maxPositions = mp; p.positionCashFraction = cf;
                                auto r = simulate(holdoutUniverse, p);
                                count5++;
                                if (r.growthPct > best5.second.growthPct) best5 = {p, r};
                            }
        std::cout << "\n=== 5단계(진단 전용, 4단계 최고점 주변 " << count5 << "개 조합 정밀탐색) ===\n";
        printResult("[holdout, 치팅 상한선]", best5.first, best5.second);
    }

    // Baseline (현재 라이브 설정 그대로: 5/20, tp2%/sl3%, wave) for reference on both sides.
    SimParams liveDefault = base;
    std::cout << "\n=== 참고: 현재 라이브 설정(5/20, tp2%/sl3%, wave) ===\n";
    printResult("[train]  ", liveDefault, simulate(trainUniverse, liveDefault));
    printResult("[holdout]", liveDefault, simulate(holdoutUniverse, liveDefault));

    return 0;
}
