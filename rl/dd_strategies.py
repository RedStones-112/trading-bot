"""낙폭 15% 제한 하에서 수익 극대화: 종목선택(core) x 노출관리(timing) x 하드 오버레이 조합 비교.
python -m rl.dd_strategies

core   : 순위권 동일비중 / GBM 상위20(5일마다 교체, 학습구간 적합, rl/ic_test.py와 같은 방식)
timing : 변동성 타기팅(core의 직전 20일 실현변동성 기준, 목표 vt) x 추세필터(SPY < 200일선이면 노출 trend_off)
overlay: rl/ddctl.py (고점 대비 limit, 배수 mult) -- 한도를 '보장'하는 안전장치
파라미터는 학습구간(2016-11~)에서 '동일비중 core'로만 고름(GBM은 학습구간에서 in-sample이라 선택에 쓰면 편향).
선택 규칙: 학습구간 MDD가 한도 이내인 조합 중 연환산 수익 최대. 그 다음 시험구간(2006-11~2016-11)으로 채점."""
import itertools, sys
import numpy as np, pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor
from rl.features import load
from rl.ic_test import xs_rank, N_STOCK
from rl import ddctl

LIMIT, COST = 0.15, 5e-4
TRAIN, TEST = ("2016-11-29", None), ("2006-11-29", "2016-11-29")


def gbm_scores(d, h=5):
    T, N, F = d.feats.shape
    lg = np.log1p(d.rets.astype(np.float64)); cs = np.vstack([np.zeros((1, N)), np.cumsum(lg, 0)])
    fwd = np.full((T, N), np.nan); fwd[:T - h] = np.expm1(cs[h + 1:T + 1] - cs[1:T - h + 1])
    lab = d.elig & ~np.isnan(fwd)
    y = xs_rank(fwd, lab)
    X = np.stack([xs_rank(d.feats[:, :, f], d.elig) for f in range(N_STOCK)], -1)  # 예측 시엔 라벨 불필요
    X = np.concatenate([X, np.where(d.elig[:, :, None], d.feats[:, :, N_STOCK:], np.nan)], -1)
    tr = (d.dates >= np.datetime64(TRAIN[0])) & ((d.dates < np.datetime64(TRAIN[1])) if TRAIN[1] else True)
    it = np.where(tr)[0]
    it = it[it + h <= it.max()][::5]  # 라벨이 학습구간 밖을 보지 않게 퍼지
    Xt, yt = X[it].reshape(-1, F), y[it].reshape(-1)
    ok = ~np.isnan(yt) & ~np.isnan(Xt).any(1)
    m = HistGradientBoostingRegressor(max_iter=200, learning_rate=0.05, max_depth=4,
                                      min_samples_leaf=200, random_state=0).fit(Xt[ok], yt[ok])
    P = np.full((T, N), np.nan); fl = X.reshape(-1, F); g = ~np.isnan(fl).any(1)
    P.reshape(-1)[g] = m.predict(fl[g]); P[~d.elig] = np.nan
    return P


def backtest(d, idx, core, vt, trend_off, mult, spy_up, top=20, reb=5):
    """idx: 날짜 인덱스(연속). core: 'eq' 또는 점수행렬 P. 반환: (누적, 연환산, 샤프, MDD, 평균노출)."""
    N = d.rets.shape[1]
    v, peak, vals, expo = 1.0, 1.0, [1.0], []
    w = np.zeros(N)        # 실제 주식 비중(합<=1, 나머지 현금)
    c = np.zeros(N)        # core 비중(합=1)
    hist = []              # core 일간수익(변동성 추정용)
    for k, t in enumerate(idx[:-1]):
        if k % reb == 0 or c.sum() == 0:
            if isinstance(core, str):
                m = d.elig[t]; c = m / m.sum()
            else:
                p = np.where(d.elig[t] & ~np.isnan(core[t]), core[t], -np.inf)
                c = np.zeros(N); c[np.argsort(-p)[:top]] = 1.0 / top
        x = 1.0
        if vt > 0 and len(hist) >= 20:
            vol = np.std(hist[-20:], ddof=1) * np.sqrt(252)
            x = min(1.0, vt / max(vol, 1e-6))
        if trend_off < 1 and not spy_up[t]:
            x *= trend_off
        tw = np.append(c * x, 1 - x)
        tw = ddctl.apply(tw, v, peak, LIMIT if mult > 0 else 0.0, mult)
        nw = tw[:-1]
        v *= 1 - COST * (np.abs(nw - w).sum() + abs((1 - nw.sum()) - (1 - w.sum())))
        r = d.rets[t + 1].astype(np.float64)
        g = float(nw @ r)
        v *= 1 + g; peak = max(peak, v); vals.append(v); expo.append(nw.sum())
        hist.append(float(c @ r))
        w = nw * (1 + r) / (1 + g)
        c = c * (1 + r); c = c / c.sum() if c.sum() > 0 else c
    vals = np.array(vals); rr = vals[1:] / vals[:-1] - 1
    sh = rr.mean() / rr.std() * np.sqrt(252) if rr.std() > 0 else 0
    return vals[-1] - 1, vals[-1] ** (252 / len(rr)) - 1, sh, (vals / np.maximum.accumulate(vals) - 1).min(), np.mean(expo)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    global TRAIN, TEST
    if "--reverse" in sys.argv:  # 2006~2016 학습 / 2016~ 시험
        TRAIN, TEST = TEST, TRAIN
    print(f"학습 {TRAIN}, 시험 {TEST}")
    d = load("rl/data/us_pit_20y.parquet", ext=True)
    mk = pd.read_parquet("rl/data/market_long.parquet").reindex(pd.DatetimeIndex(d.dates)).ffill()
    spy_up = (mk.spy > mk.spy.rolling(200).mean()).values
    rng = lambda p: np.where((d.dates >= np.datetime64(p[0])) & ((d.dates < np.datetime64(p[1])) if p[1] else True))[0]
    tr, te = rng(TRAIN), rng(TEST)
    grid = list(itertools.product([0, 0.06, 0.08, 0.10, 0.12, 0.15], [1.0, 0.5, 0.0], [0, 2, 3, 4]))

    print(f"[1] 파라미터 선택: 학습구간, 동일비중 core, 낙폭 한도 {LIMIT:.0%}")
    rows = []
    for vt, to, mu in grid:
        c, an, sh, dd, ex = backtest(d, tr, "eq", vt, to, mu, spy_up)
        rows.append((vt, to, mu, c, an, sh, dd, ex))
    df = pd.DataFrame(rows, columns=["vt", "trend_off", "mult", "cum", "ann", "sharpe", "mdd", "expo"])
    ok = df[df.mdd >= -LIMIT].sort_values("ann", ascending=False)
    print(ok.head(8).to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    best = ok.iloc[0]
    print(f"\n선택: vt={best.vt}, trend_off={best.trend_off}, mult={best.mult}")

    P = gbm_scores(d)
    print(f"\n[2] 시험구간 {TEST} 채점, 비용 5bp")
    print(f"{'전략':<34}{'누적':>9}{'연환산':>8}{'샤프':>7}{'MDD':>9}{'평균노출':>9}")
    cases = [("동일비중, 제한 없음", "eq", 0, 1.0, 0),
             ("동일비중, 오버레이만(mult 2)", "eq", 0, 1.0, 2),
             ("동일비중, 선택 조합", "eq", best.vt, best.trend_off, best.mult),
             ("GBM상위20, 제한 없음", P, 0, 1.0, 0),
             ("GBM상위20, 선택 조합", P, best.vt, best.trend_off, best.mult)]
    for name, core, vt, to, mu in cases:
        c, an, sh, dd, ex = backtest(d, te, core, vt, to, mu, spy_up)
        flag = "" if dd >= -LIMIT - 1e-9 else "  <- 한도 초과"
        print(f"{name:<30}{c:>9.1%}{an:>8.1%}{sh:>7.2f}{dd:>9.1%}{ex:>9.2f}{flag}")
    print(f"\n[3] 같은 조합을 학습구간에도 적용(참고, GBM은 in-sample이라 과대평가)")
    for name, core, vt, to, mu in cases:
        c, an, sh, dd, ex = backtest(d, tr, core, vt, to, mu, spy_up)
        print(f"{name:<30}{c:>9.1%}{an:>8.1%}{sh:>7.2f}{dd:>9.1%}{ex:>9.2f}")


if __name__ == "__main__":
    main()
