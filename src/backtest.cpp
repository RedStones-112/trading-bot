// Offline backtest: replays the live scan's own signal functions (strategy.hpp) over real
// historical daily bars, so probability_mode(basic/wave)와 추세 필터가 실제로 승률/기대값에
// 도움이 되는지 라이브 시행착오 없이 통계로 확인할 수 있음 (2026-08-14, PROGRESS.md
// "3일 운영 성과 진단" 참고 -- 표본 20건짜리 라이브 관찰로는 노이즈와 엣지를 구분할 수 없어서
// 만듦). trading_bot과 같은 config.json/KisClient를 읽기 전용으로만 씀(주문 없음, 계좌에
// 영향 없음) -- getDailyBars만 호출.
//
// 종목 유니버스: trades.log에 등장한 종목(코드 unique) -- 실제로 라이브 스캔이 골라온
// 종목들이라 임의 유니버스보다 대표성이 있음. trades.log가 없으면 에러로 안내.
//
// ponytail: 포트폴리오 시뮬레이션(자금 배분/동시보유/로테이션)까지는 안 함 -- "이 신호가
// 뜬 시점에 샀으면 TP/SL 중 뭐가 먼저 왔나"만 보는 신호 단위 백테스트. 그거면 지금 필요한
// 질문(신호 자체에 엣지가 있는지)에는 충분하고, 포트폴리오 시뮬레이션은 훨씬 복잡한데
// 지금 당장 필요하지도 않음.
#include "broker.hpp"
#include "kis_client.hpp"
#include "strategy.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <windows.h>

using json = nlohmann::json;

namespace {

// Live main.cpp 스캔이 하나의 후보에 대해 계산하는 것과 동일한 값들 -- probability_mode별
// 확률/추세필터 판정에 필요한 전부.
struct SignalRecord {
    std::string code;
    bool trendOk = false;      // smaTrendNotFalling 통과 여부
    double basicProb = 0.5;
    double waveProb = 0.5;
    TradeOutcome outcome = TradeOutcome::None;
};

std::set<std::pair<std::string, std::string>> loadSymbolUniverse() {
    std::set<std::pair<std::string, std::string>> symbols; // (code, name)
    std::ifstream f("trades.log");
    std::string line;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::vector<std::string> cols;
        std::string cell;
        while (std::getline(ss, cell, ',')) cols.push_back(cell);
        if (cols.size() < 4) continue;
        symbols.insert({cols[2], cols[3]});
    }
    return symbols;
}

struct Bucket {
    int wins = 0, losses = 0, none = 0;
    double winRate() const { return (wins + losses) > 0 ? (double)wins / (wins + losses) * 100.0 : 0.0; }
};

void addTo(Bucket& b, TradeOutcome o) {
    if (o == TradeOutcome::Win) b.wins++;
    else if (o == TradeOutcome::Loss) b.losses++;
    else b.none++;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

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
        std::cerr << "mode=mock has no real price history to backtest against -- set mode to sim/paper/live in config.json (only getDailyBars is called, read-only, no orders placed).\n";
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
    const int kHistoryBars = 100;           // getDailyBars 안전 상한, main.cpp의 kMlTrainingBars와 동일
    int windowBars = smaLong + 5;           // 라이브 스캔과 같은 창 크기

    auto symbols = loadSymbolUniverse();
    if (symbols.empty()) {
        std::cerr << "trades.log에서 종목을 못 찾음 -- 프로젝트 루트(trades.log가 있는 곳)에서 실행할 것.\n";
        return 1;
    }
    std::cout << "종목 유니버스: " << symbols.size() << "개 (trades.log 기준)\n";

    KisClient client(cfg.at("appkey"), cfg.at("appsecret"), cfg.value("cano", ""),
                      cfg.value("acnt_prdt_cd", ""), mode != "live");
    try {
        client.authenticate();
    } catch (const std::exception& e) {
        std::cerr << "인증 실패: " << e.what() << "\n";
        return 1;
    }
    std::cout << "인증 완료, 일봉 조회 시작 (종목당 " << kHistoryBars << "봉, 1.1초 간격)...\n";

    std::vector<SignalRecord> records;
    int symbolsOk = 0, symbolsFailed = 0;
    for (auto& [code, name] : symbols) {
        try {
            auto bars = client.getDailyBars(code, kHistoryBars);
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            int n = (int)bars.size();
            if (n < windowBars + kLookaheadDays + 1) {
                std::cout << "  " << name << "(" << code << ") 일봉 부족(" << n << "개) -- 스킵\n";
                symbolsFailed++;
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
            std::cout << "  " << name << "(" << code << ") 골든크로스 " << perSymbol << "건\n";
            symbolsOk++;
        } catch (const std::exception& e) {
            std::cout << "  " << name << "(" << code << ") 일봉 조회 실패: " << e.what() << " -- 스킵\n";
            symbolsFailed++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        }
    }

    std::ostringstream report;
    report << "종목 " << symbolsOk << "개 처리, " << symbolsFailed << "개 스킵/실패, 총 신호(골든크로스) "
           << records.size() << "건\n\n";

    double roundTripDrag = feeRate * 2 + taxRate; // 매수/매도 수수료 + 매도세, 승패 무관하게 매 트레이드 발생
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
    report << "  추세 필터 적용(이번에 추가한 smaTrendNotFalling): " << bucketLine(allWithFilter) << "\n";

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
    reportCalibration("wave 모드", true);

    std::cout << "\n" << report.str();
    std::ofstream out("backtest_report.txt");
    out << report.str();
    std::cout << "\nbacktest_report.txt 저장 완료.\n";
    return 0;
}
