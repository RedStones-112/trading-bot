// Wide-universe, multi-year offline backtest -- same signal-replay idea as backtest.cpp,
// but built to answer the same questions (does probability_mode/a filter have real edge)
// with a much bigger sample, fast. 사용자 요청(2026-09-28): 라이브 봇이 하루 한 스텝씩 새
// 일봉을 쌓아가며 실험(예: wave 확률 임계값 필터)을 검증하는 게 너무 느려서(수십 세션째
// 결론 미확정, PROGRESS.md 09-08~09-28 항목들 참고) -- 과거 5년치 실제 KIS 일봉을 한 번에
// 받아와 훨씬 큰 표본으로 같은 질문에 답하기 위한 별도 도구.
//
// **일일 자동 세션(scripts/run_daily_claude_review.ps1)이 쓰는 backtest.cpp/backtest.exe는
// 건드리지 않음** -- 사용자가 이 5년치 백테스트는 일단 수동 리서치 전용으로 쓰기로 함
// (2026-09-28 대화에서 확인). 그 자동화 스크립트/프롬프트는 CLAUDE.md 하드룰상 사람 확인
// 없이는 수정 대상이 아니기도 함.
//
// 종목 유니버스: 오늘 시점 거래량순위 상위(getTopVolumeStocks, 세그먼트 병합+ETF 필터링
// 이미 구현된 것 재사용) + trades-*.log에 등장한 종목(라이브 봇이 실제로 골랐던 종목,
// backtest.cpp와 같은 소스)의 합집합 -- trades.log만 쓰면 라이브 봇이 이미 고른 종목으로만
// 편향되고, 거래량순위만 쓰면 최근에 유동성 있던 종목으로만 좁혀지므로 합쳐서 더 넓고 덜
// 편향된 표본을 만듦.
//
// 일봉 히스토리는 data/daily_bars/<code>.json에 로컬 캐시(bar_cache.hpp, 다른 파일 기반
// 데이터와 같은 관례) -- 처음 실행만 종목당 최대 ~13번 페이지 호출(1.1초 간격)로 5년치를
// 전부 받고, 그 다음부터는 종목당 한 번(최근 100봉)만 호출해서 캐시 뒤에 이어 붙임. 이게
// 핵심 -- 재실행할 때마다 몇 분 안에 전체 유니버스가 최신 데이터로 갱신되고 그 위에서
// 훨씬 큰 표본으로 같은 백테스트 리포트를 다시 낼 수 있음.
//
// ponytail: backtest.cpp와 리포트 형식/신호 재현 로직이 상당 부분 겹치지만(SignalRecord/
// Bucket/골든크로스 재현 루프), 일부러 별도 파일로 둠(사용자가 위에서 결정한 대로 daily
// 자동화가 의존하는 backtest.cpp의 동작을 조금도 바꾸지 않기 위함) -- 공유 헤더로 뽑는 건
// 나중에 이 도구가 자리잡은 뒤에 고려.
#include "broker.hpp"
#include "kis_client.hpp"
#include "strategy.hpp"
#include "trade_log.hpp"
#include "bar_cache.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <windows.h>

using json = nlohmann::json;

namespace {

struct SignalRecord {
    std::string code;
    bool trendOk = false;
    double basicProb = 0.5;
    double waveProb = 0.5;
    TradeOutcome outcome = TradeOutcome::None;
};

struct Bucket {
    int wins = 0, losses = 0, none = 0;
    double winRate() const { return (wins + losses) > 0 ? (double)wins / (wins + losses) * 100.0 : 0.0; }
};

void addTo(Bucket& b, TradeOutcome o) {
    if (o == TradeOutcome::Win) b.wins++;
    else if (o == TradeOutcome::Loss) b.losses++;
    else b.none++;
}

std::set<std::pair<std::string, std::string>> loadTradesLogSymbols() {
    std::set<std::pair<std::string, std::string>> symbols;
    for (auto& path : allTradesLogPaths()) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            std::vector<std::string> cols;
            std::string cell;
            while (std::getline(ss, cell, ',')) cols.push_back(cell);
            if (cols.size() < 4) continue;
            symbols.insert({cols[2], cols[3]});
        }
    }
    return symbols;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    // CLI 인자(전부 선택, 수동 리서치 도구라 config.json에 안 넣고 실행 시점에 조절):
    //   argv[1] = 목표 히스토리 연수 (기본 5)
    //   argv[2] = getTopVolumeStocks에 요청할 개수 (기본 300 -- API 자체가 세그먼트당 30개
    //             상한이라 실제로는 최대 ~90개까지만 돌아옴, 작게 주면 빠른 소규모 테스트용)
    //   argv[3] = "notrades"면 trades-*.log 종목 합집합을 건너뜀(순수 거래량순위만)
    int years = argc > 1 ? std::stoi(argv[1]) : 5;
    int universeRequest = argc > 2 ? std::stoi(argv[2]) : 300;
    bool includeTradesLog = !(argc > 3 && std::string(argv[3]) == "notrades");
    int targetTradingDays = years * 250; // 연 거래일수 근사, main.cpp kMlTrainingDays와 같은 관례

    std::ifstream cfgFile("config.json");
    if (!cfgFile) {
        std::cerr << "config.json not found -- run this from the project root (same directory trading_bot.exe reads config.json from).\n";
        return 1;
    }
    json cfg;
    try {
        cfg = json::parse(cfgFile, nullptr, true, true);
    } catch (const std::exception& e) {
        std::cerr << "config.json parse failed: " << e.what() << "\n";
        return 1;
    }
    std::string mode = cfg.value("mode", "paper");
    if (mode == "mock") {
        std::cerr << "mode=mock has no real price history to backtest against -- set mode to sim/paper/live in config.json (only getDailyBars/getTopVolumeStocks are called, read-only, no orders placed).\n";
        return 1;
    }

    int smaShort = cfg.value("sma_short", 5);
    int smaLong = cfg.value("sma_long", 20);
    double feeRate = cfg.value("fee_rate", 0.00015);
    double taxRate = cfg.value("tax_rate", 0.0018);
    double takeProfitPct = cfg.value("take_profit_pct", 0.02);
    double stopLossPct = cfg.value("stop_loss_pct", 0.03);
    const int kLookaheadDays = 5;          // main.cpp의 kMlLabelLookaheadDays와 같은 관례/값
    const int kTrendFilterLookbackDays = 5; // main.cpp의 kTrendFilterLookbackDays와 반드시 맞출 것
    int windowBars = smaLong + 5;           // 라이브 스캔과 같은 창 크기

    KisClient client(cfg.at("appkey"), cfg.at("appsecret"), cfg.value("cano", ""),
                      cfg.value("acnt_prdt_cd", ""), mode != "live");
    // 단발성 WinHTTP 타임아웃(GetLastError=12002) 한 번에 20~30분짜리 실행 전체가 날아가는
    // 걸 막음 -- main.cpp가 2026-09-15에 같은 증상(인증 재시도 누락)을 무한 재시도로 고쳤던
    // 것과 같은 이유지만, 이건 사람이 지켜보는 수동 도구라 무한정 매달리지 않고 유한 횟수만
    // 재시도함(2026-09-28, 이 도구를 실제로 돌리다 라이브로 재현해서 발견).
    bool authenticated = false;
    for (int attempt = 1; attempt <= 5 && !authenticated; attempt++) {
        try {
            client.authenticate();
            authenticated = true;
        } catch (const std::exception& e) {
            std::cerr << "인증 실패(시도 " << attempt << "/5): " << e.what() << "\n";
            if (attempt < 5) std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    if (!authenticated) {
        std::cerr << "인증 5회 재시도 모두 실패 -- 종료.\n";
        return 1;
    }

    std::cout << "종목 유니버스 수집 중 (거래량순위 top " << universeRequest
              << (includeTradesLog ? " + trades-*.log" : "") << ")...\n";
    std::set<std::pair<std::string, std::string>> symbols;
    for (auto& s : client.getTopVolumeStocks(universeRequest)) symbols.insert({s.code, s.name});
    if (includeTradesLog)
        for (auto& s : loadTradesLogSymbols()) symbols.insert(s);
    std::cout << "종목 유니버스: " << symbols.size() << "개, 목표 히스토리 " << years << "년(약 "
              << targetTradingDays << "거래일)\n";

    std::vector<SignalRecord> records;
    int symbolsOk = 0, symbolsFailed = 0, fullFetch = 0, topUpFetch = 0;
    for (auto& [code, name] : symbols) {
        std::vector<DailyBar> bars;
        try {
            auto cached = loadCachedBars(code);
            if ((int)cached.size() < targetTradingDays) {
                // 캐시가 없거나 목표치보다 훨씬 부족 -- 전체 페이지네이션으로 새로 받음
                // (getDailyBars가 이미 100봉/페이지, 페이지 사이 1.1초 간격으로 구현돼 있음).
                bars = client.getDailyBars(code, targetTradingDays);
                fullFetch++;
            } else {
                // 캐시가 이미 충분 -- 최근 100봉(API 호출 1번)만 받아서 캐시 뒤에 이어붙임.
                auto recent = client.getDailyBars(code, 100);
                bars = mergeBars(cached, recent);
                if ((int)bars.size() > targetTradingDays)
                    bars.erase(bars.begin(), bars.end() - targetTradingDays);
                topUpFetch++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // KIS 레이트리밋 -- 종목 사이 간격
            saveCachedBars(code, bars);
        } catch (const std::exception& e) {
            std::cout << "  " << name << "(" << code << ") 일봉 조회 실패: " << e.what() << " -- 캐시된 값으로 대체\n";
            bars = loadCachedBars(code);
            symbolsFailed++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        }

        int n = (int)bars.size();
        if (n < windowBars + kLookaheadDays + 1) {
            std::cout << "  " << name << "(" << code << ") 일봉 부족(" << n << "개) -- 스킵\n";
            continue;
        }
        int perSymbol = 0;
        for (int i = windowBars; i + kLookaheadDays < n; i++) {
            std::vector<DailyBar> window(bars.begin() + (i - windowBars), bars.begin() + i);
            std::vector<double> closes;
            closes.reserve(window.size() + 1);
            for (auto& b : window) closes.push_back(b.close);
            closes.push_back(bars[i].close);

            Signal sig = smaCrossSignal(closes, smaShort, smaLong);
            if (sig != Signal::Buy) continue;

            double currentPrice = bars[i].close;
            double trendPct = window.front().close > 0
                ? (currentPrice - window.front().close) / window.front().close : 0.0;
            double belowRatio = belowPriceVolumeRatio(window, currentPrice);
            double volumeSurgePct = (i > 0 && bars[i - 1].volume > 0)
                ? bars[i].volume / bars[i - 1].volume * 100.0 : 100.0;

            SignalRecord rec;
            rec.code = code;
            rec.trendOk = smaTrendNotFalling(closes, smaLong, kTrendFilterLookbackDays);
            rec.basicProb = probabilityFromTechnicals(volumeSurgePct, trendPct, belowRatio);
            rec.waveProb = probabilityFromWaveAnalysis(window, currentPrice);
            rec.outcome = resolveTradeOutcome(bars, i, takeProfitPct, stopLossPct, kLookaheadDays);
            records.push_back(rec);
            perSymbol++;
        }
        std::cout << "  " << name << "(" << code << ") 일봉 " << n << "개, 골든크로스 " << perSymbol << "건\n";
        symbolsOk++;
    }

    std::ostringstream report;
    report << "종목 유니버스 " << symbols.size() << "개(거래량순위 top " << universeRequest
           << (includeTradesLog ? " + trades-*.log 합집합" : "") << "), 목표 히스토리 " << years << "년\n";
    report << "처리 " << symbolsOk << "개(전체수집 " << fullFetch << " / 캐시 top-up " << topUpFetch
           << "), 실패 " << symbolsFailed << "개, 총 신호(골든크로스) " << records.size() << "건\n\n";

    double roundTripDrag = feeRate * 2 + taxRate;
    auto bucketLine = [&](const Bucket& b) {
        double ev = (b.wins + b.losses) > 0
            ? (b.winRate() / 100.0) * takeProfitPct - (1 - b.winRate() / 100.0) * stopLossPct - roundTripDrag
            : 0.0;
        std::ostringstream line;
        line << (b.wins + b.losses) << "건 (승 " << b.wins << " / 패 " << b.losses << ", 미결 " << b.none
             << ") 승률 " << std::fixed << std::setprecision(1) << b.winRate()
             << "% -- 단순 EV(수수료/세금 반영) " << std::setprecision(2) << ev * 100.0 << "%/trade";
        return line.str();
    };

    report << "[신호 자체의 엣지: 추세 필터 적용 전/후]\n";
    Bucket allNoFilter, allWithFilter;
    for (auto& r : records) {
        addTo(allNoFilter, r.outcome);
        if (r.trendOk) addTo(allWithFilter, r.outcome);
    }
    report << "  추세 필터 없음(현재 라이브 로직 기준): " << bucketLine(allNoFilter) << "\n";
    report << "  추세 필터 적용(smaTrendNotFalling): " << bucketLine(allWithFilter) << "\n";

    report << "\n[확률 추정 모드별 캘리브레이션: probability 3분위 구간별 실제 승률]\n";
    report << "(구간별 승률이 확률 크기 순서대로 올라가야 그 확률값이 실제로 의미 있다는 뜻,\n"
              " 순서가 뒤섞이거나 평평하면 그 모드는 노이즈에 가깝다는 뜻)\n";
    auto reportCalibration = [&](const std::string& modeLabel, bool useWave) {
        std::vector<std::pair<double, TradeOutcome>> probOutcome;
        for (auto& r : records) probOutcome.push_back({useWave ? r.waveProb : r.basicProb, r.outcome});
        std::sort(probOutcome.begin(), probOutcome.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
        size_t n = probOutcome.size();
        if (n < 3) { report << "  " << modeLabel << ": 표본 부족(" << n << "건)\n"; return; }
        size_t third = n / 3;
        report << "  " << modeLabel << ":\n";
        const char* tierNames[3] = {"하위(확률 낮음)", "중위", "상위(확률 높음)"};
        for (int t = 0; t < 3; t++) {
            size_t start = t * third;
            size_t end = (t == 2) ? n : (t + 1) * third;
            Bucket b;
            for (size_t k = start; k < end; k++) addTo(b, probOutcome[k].second);
            report << "    " << tierNames[t] << " (확률 " << std::setprecision(2) << probOutcome[start].first
                   << "~" << probOutcome[end - 1].first << "): " << bucketLine(b) << "\n";
        }
    };
    reportCalibration("basic 모드", false);

    // wave 모드는 basic처럼 연속값이 아니라 probabilityFromWaveAnalysis(strategy.hpp)가
    // 정확히 8개 이산값만 반환하는 룩업 테이블이라(2026-09-15 발견, PROGRESS.md "알려진
    // 한계" 참고), 위 percentile 3분위 분할은 그 이산값 클러스터(특히 0.65)를 표본 순서에
    // 따라 임의로 상/중위 경계에서 쪼개는 타이브레이크 아티팩트를 만듦. 대신 실제 반환값별로
    // 직접 묶어서(경계를 임의로 정하지 않으므로 이 아티팩트가 원천적으로 없음) 각 값의 진짜
    // 승률/EV를 보여줌 -- backtest.cpp와 동일한 수정(2026-09-28, 같은 세션).
    report << "\n  wave 모드 (이산값별 -- percentile 3분위 대신 실제 반환값으로 직접 묶음,\n"
              "   09-15에 발견된 타이브레이크 아티팩트 회피):\n";
    std::map<double, Bucket> waveByValue;
    for (auto& r : records) addTo(waveByValue[r.waveProb], r.outcome);
    if (waveByValue.empty()) {
        report << "    표본 부족(0건)\n";
    } else {
        for (auto& [value, b] : waveByValue)
            report << "    확률=" << std::fixed << std::setprecision(2) << value << ": " << bucketLine(b) << "\n";
    }

    std::cout << "\n" << report.str();
    std::ofstream out("backtest_bulk_report.txt");
    out << report.str();
    std::cout << "\nbacktest_bulk_report.txt 저장 완료. (일일 자동 세션이 쓰는 backtest_report.txt와는 별개 파일)\n";
    return 0;
}
