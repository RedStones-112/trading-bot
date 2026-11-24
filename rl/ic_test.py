"""신호 존재 여부 검증(지도학습). python -m rl.ic_test [--horizon 20]

1) 특성별 순위 IC: 매일 순위권(eligible) 종목들 사이에서 특성 순위와 h일 후 수익률 순위의 상관.
   학습구간(2016-11~)과 시험구간(2006-11~2016-11) 각각 계산해 부호가 일관되는지 본다.
2) 모델 IC: 학습구간에서 Ridge / HistGradientBoosting을 맞추고 시험구간 IC로 채점.
3) 포트폴리오: 시험구간에서 h일마다 예측 상위 20종목 동일비중 vs 순위권 전체 동일비중(비용 5bp).
시장 특성(종목 간 동일값)은 횡단면 IC가 정의되지 않으므로 1)에서는 제외, 모델 입력에는 포함."""
import argparse, sys
import numpy as np, pandas as pd
from sklearn.linear_model import Ridge
from sklearn.ensemble import HistGradientBoostingRegressor
from rl.features import load

NAMES = ["ret1", "ret5", "ret20", "vol20", "sma50_gap", "volume_ratio", "ret60", "ret120", "ret250",
         "dd252", "rsi14", "rel20", "xsrank20", "mkt_ret1", "mkt_ret20", "mkt_vol20", "spy_gap200",
         "vix", "vix_chg5"]
N_STOCK = 13
TRAIN, TEST = ("2016-11-29", None), ("2006-11-29", "2016-11-29")


def xs_rank(x, mask):
    """[T,N] -> 행마다 mask 안에서 0~1 순위 - 0.5 (mask 밖 NaN)."""
    df = pd.DataFrame(np.where(mask, x, np.nan))
    return (df.rank(axis=1, pct=True) - 0.5).values


def daily_ic(a, b):
    """행별 피어슨(순위끼리면 스피어만). 유효 종목 20개 미만인 날은 NaN."""
    m = ~np.isnan(a) & ~np.isnan(b)
    out = np.full(len(a), np.nan)
    for t in range(len(a)):
        if m[t].sum() >= 20:
            out[t] = np.corrcoef(a[t, m[t]], b[t, m[t]])[0, 1]
    return out


def summarize(ic, h):
    ic = ic[~np.isnan(ic)]
    sub = ic[::h]  # 겹치지 않는 표본으로 t값(h일 수익률 중첩 보정)
    t = sub.mean() / (sub.std(ddof=1) / np.sqrt(len(sub))) if len(sub) > 2 else np.nan
    return ic.mean(), ic.mean() / ic.std(ddof=1), t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--horizon", type=int, default=20)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args(); h = a.horizon
    sys.stdout.reconfigure(encoding="utf-8")

    d = load("rl/data/us_pit_20y.parquet", ext=True)
    T, N, F = d.feats.shape
    # h일 선행 수익률: t일 종가 -> t+h일 종가
    lg = np.log1p(d.rets.astype(np.float64))
    cs = np.vstack([np.zeros((1, N)), np.cumsum(lg, 0)])
    fwd = np.full((T, N), np.nan)
    fwd[:T - h] = np.expm1(cs[h + 1:T + 1] - cs[1:T - h + 1])
    mask = d.elig & ~np.isnan(fwd)
    y = xs_rank(fwd, mask)
    X = np.stack([xs_rank(d.feats[:, :, f], mask) for f in range(N_STOCK)], -1)  # 종목특성: 횡단면 순위
    X = np.concatenate([X, np.where(mask[:, :, None], d.feats[:, :, N_STOCK:], np.nan)], -1)  # 시장특성: 원값

    def period(p):
        m = np.ones(T, bool)
        if p[0]: m &= d.dates >= np.datetime64(p[0])
        if p[1]: m &= d.dates < np.datetime64(p[1])
        return m
    tr, te = period(TRAIN), period(TEST)
    # 학습구간 끝 h일은 시험/미래 정보와 무관하지만 라벨이 없으므로 자동 제외(NaN)

    print(f"h={h}일, 종목 {N}, 학습 {d.dates[tr][0].astype('M8[D]')}~, 시험 "
          f"{d.dates[te][0].astype('M8[D]')}~{d.dates[te][-1].astype('M8[D]')}")
    print(f"\n[1] 특성별 순위 IC (t값은 {h}일 간격 비중첩 표본 기준, |t|>2면 유의)")
    print(f"{'특성':<14}{'학습IC':>9}{'학습t':>8}{'시험IC':>9}{'시험t':>8}  부호일치")
    for f in range(N_STOCK):
        r_tr = summarize(daily_ic(X[tr, :, f], y[tr]), h)
        r_te = summarize(daily_ic(X[te, :, f], y[te]), h)
        same = "O" if np.sign(r_tr[0]) == np.sign(r_te[0]) else "X"
        print(f"{NAMES[f]:<14}{r_tr[0]:>9.4f}{r_tr[2]:>8.2f}{r_te[0]:>9.4f}{r_te[2]:>8.2f}  {same}")

    # 2) 모델: 학습구간에서 5일 간격 표본으로 적합(중첩 완화)
    idx_tr = np.where(tr)[0][::5]
    Xtr = X[idx_tr].reshape(-1, F); ytr = y[idx_tr].reshape(-1)
    ok = ~np.isnan(ytr) & ~np.isnan(Xtr).any(1)
    Xtr, ytr = Xtr[ok], ytr[ok]
    models = {"Ridge": Ridge(alpha=10.0),
              "GBM": HistGradientBoostingRegressor(max_iter=200, learning_rate=0.05, max_depth=4,
                                                   min_samples_leaf=200, random_state=0)}
    print(f"\n[2] 모델 IC (학습 표본 {len(ytr):,}개)")
    print(f"{'모델':<8}{'학습IC':>9}{'시험IC':>9}{'시험IR':>9}{'시험t':>8}")
    preds = {}
    for name, mdl in models.items():
        mdl.fit(Xtr, ytr)
        P = np.full((T, N), np.nan)
        flat = X.reshape(-1, F); good = ~np.isnan(flat).any(1)
        P.reshape(-1)[good] = mdl.predict(flat[good])
        P[~mask] = np.nan
        preds[name] = P
        i_tr = summarize(daily_ic(P[tr], y[tr]), h)
        i_te = summarize(daily_ic(P[te], y[te]), h)
        print(f"{name:<8}{i_tr[0]:>9.4f}{i_te[0]:>9.4f}{i_te[1]:>9.3f}{i_te[2]:>8.2f}")

    # 3) 시험구간 포트폴리오: h일마다 리밸런스, 상위 top 동일비중 vs 순위권 전체 동일비중
    print(f"\n[3] 시험구간 포트폴리오 ({h}일마다 리밸런스, 상위 {a.top}종목, 비용 {a.cost_bps}bp)")
    print(f"{'전략':<16}{'누적수익':>10}{'연환산':>9}{'샤프':>7}{'MDD':>9}")
    te_idx = np.where(te)[0]
    def backtest(pick):
        v, vals, w = 1.0, [1.0], np.zeros(N)
        for k, t in enumerate(te_idx[:-1]):
            if k % h == 0:
                nw = pick(t)
                v *= 1 - a.cost_bps / 1e4 * np.abs(nw - w).sum(); w = nw
            r = d.rets[t + 1].astype(np.float64)
            g = w @ r; v *= 1 + g
            w = w * (1 + r) / (1 + g) if (1 + g) > 0 else w
            vals.append(v)
        vals = np.array(vals); rr = vals[1:] / vals[:-1] - 1
        return vals[-1] - 1, vals[-1] ** (252 / len(rr)) - 1, rr.mean() / rr.std() * np.sqrt(252), \
               (vals / np.maximum.accumulate(vals) - 1).min()
    def eqw(t):
        m = d.elig[t]; return m / m.sum()
    def top_of(P):
        def f(t):
            p = np.where(d.elig[t] & ~np.isnan(P[t]), P[t], -np.inf)
            sel = np.argsort(-p)[:a.top]; w = np.zeros(N); w[sel] = 1 / a.top; return w
        return f
    for name, pick in [("순위권 동일비중", eqw)] + [(f"{k} 상위{a.top}", top_of(P)) for k, P in preds.items()] \
            + [(f"{k} 하위{a.top}", top_of(-P)) for k, P in preds.items()]:
        c, an, sh, dd = backtest(pick)
        print(f"{name:<16}{c:>10.1%}{an:>9.1%}{sh:>7.2f}{dd:>9.1%}")


if __name__ == "__main__":
    main()
