"""U자 바닥 신호: 큰 하락 후 U자로 바닥을 다지고 '발목' 정도까지만 회복한 종목. python -m rl.u_shape [--data ...]

신호(t일 종가 기준, 미래 정보 없음):
  고점 H = 직전 252일 최고가, 저점 L = 직전 120일 중 H 이후 최저가
  - 하락폭 1 - L/H >= DROP(0.25)
  - 저점이 LOW_MIN~LOW_MAX(10~60)거래일 전 (방금 찍은 V자 급락 제외, 바닥 다지는 기간 확보)
  - 회복률 (P - L)/(H - L) 이 REC_LO~REC_HI(0.05~0.35) -- '발목' 정도
  - 20일 이평 5일 기울기 > 0 이고 P > 20일 이평 (상승 전환)
백테스트: 시총 상위 100 유니버스, 월(21거래일) 리밸런스, 비용 11bp, 양쪽 구간."""
import argparse, sys
import numpy as np
import pandas as pd
from rl.features import load
from rl.env import PortfolioEnv
from rl.evaluate import run, metrics, logit

DROP, LOW_MIN, LOW_MAX, REC_LO, REC_HI = 0.25, 10, 60, 0.05, 0.35


def signal(close: pd.DataFrame) -> pd.DataFrame:
    c = close.values.astype(float)
    T, N = c.shape
    sig = np.zeros((T, N), bool)
    ma = close.rolling(20).mean().values
    for t in range(252, T):
        win = c[t - 251:t + 1]                       # 직전 252일(오늘 포함)
        if np.isnan(win[-1]).all():
            continue
        hi_pos = np.nanargmax(np.where(np.isnan(win), -np.inf, win), axis=0)
        H = win[hi_pos, np.arange(N)]
        seg = np.where(np.arange(252)[:, None] >= np.maximum(hi_pos, 252 - 120)[None, :], win, np.inf)
        seg = np.where(np.isnan(seg), np.inf, seg)
        lo_pos = seg.argmin(0)
        L = seg[lo_pos, np.arange(N)]
        P = win[-1]
        ago = 251 - lo_pos
        with np.errstate(invalid="ignore", divide="ignore"):
            rec = (P - L) / (H - L)
            ok = ((1 - L / H) >= DROP) & (ago >= LOW_MIN) & (ago <= LOW_MAX) & (rec >= REC_LO) & (rec <= REC_HI) \
                 & (ma[t] > ma[t - 5]) & (P > ma[t])
        sig[t] = np.nan_to_num(ok, nan=False).astype(bool)
    return pd.DataFrame(sig, index=close.index, columns=close.columns)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="rl/data/kr_cap100.parquet")
    ap.add_argument("--market", default="rl/data/kr_market.parquet")
    ap.add_argument("--tilt", type=float, default=3.0, help="신호 종목 비중 배수")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    raw = pd.read_parquet(a.data)
    close = raw.pivot(index="date", columns="ticker", values="close").sort_index()
    cap = raw.pivot(index="date", columns="ticker", values="marcap").reindex_like(close).ffill().fillna(0)
    sig_all = signal(close)
    full = load(a.data, ext=True, market=a.market)
    for start, end, label in [("2015-11-30", "2021-01-01", "2015-12~2021"), ("2021-01-01", None, "2021~")]:
        d = full.split(start, end)
        dates = pd.DatetimeIndex(d.dates)
        S = sig_all.reindex(index=dates, columns=d.tickers).fillna(False).values & d.elig
        C = cap.reindex(index=dates, columns=d.tickers).values * d.elig
        E = d.elig.astype(float)
        env = PortfolioEnv(d, cost_bps=11, rebalance_every=21)

        def mk(fn):
            return lambda o, i: logit(np.append(fn(10 + i) / max(fn(10 + i).sum(), 1e-12), 0.0))
        pols = {
            "동일비중": lambda t: E[t],
            f"동일비중+U x{a.tilt:g}": lambda t: E[t] * (1 + (a.tilt - 1) * S[t]),
            "U 종목만(없으면 동일)": lambda t: S[t].astype(float) if S[t].sum() >= 3 else E[t],
            "시총가중": lambda t: C[t],
            f"시총가중+U x{a.tilt:g}": lambda t: C[t] * (1 + (a.tilt - 1) * S[t]),
        }
        reb = S[10::21].sum(1)
        print(f"\n[{label}] 리밸런스 시점 U 신호 종목 수: 평균 {reb.mean():.1f}, 0개인 달 {int((reb == 0).sum())}/{len(reb)}")
        print(f"{'전략':<22}{'누적':>9}{'연환산':>9}{'변동성':>9}{'샤프':>7}{'MDD':>9}")
        for n, f in pols.items():
            c, an, v, s, dd, _ = metrics(*run(env, mk(f)))
            print(f"{n:<20}{c:>9.1%}{an:>9.1%}{v:>9.1%}{s:>7.2f}{dd:>9.1%}")
        # 신호 자체의 예측력: 신호 종목 vs 나머지 순위권 종목의 이후 21일 수익률
        cs = np.vstack([np.zeros((1, d.rets.shape[1])), np.cumsum(np.log1p(d.rets), 0)])
        fwd = np.full(d.rets.shape, np.nan)
        fwd[:-21] = np.exp(cs[22:] - cs[1:-21]) - 1  # t+1 ~ t+21일 수익
        m = ~np.isnan(fwd)
        s_r = fwd[S & m].mean(); o_r = fwd[(d.elig & ~S) & m].mean()
        print(f"이후 21일 평균수익: U 신호 {s_r:.2%} (표본 {int((S & m).sum())}) vs 나머지 {o_r:.2%}")


if __name__ == "__main__":
    main()
