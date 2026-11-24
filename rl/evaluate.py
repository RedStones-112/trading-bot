"""검증 구간(기본 2024-01-01 이후) 정책 비교. 사용: python rl/evaluate.py [--start 2024-01-01] [--model rl/models/ppo.zip] [--cost-bps 5]
정책: 동일비중 B&H / 동일비중 매일 리밸런스 / 전액 현금 / PPO(모델 파일이 있을 때)."""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import numpy as np
from rl.features import load, PATH as PATH0
from rl.env import PortfolioEnv

def logit(w):
    return np.log(np.asarray(w, np.float64) + 1e-9)

def run(env, policy):
    obs, _ = env.reset()
    vals, tos, done, i = [1.0], [], False, 0
    while not done:
        obs, _, done, _, info = env.step(policy(obs, i))
        vals.append(info["value"]); tos.append(info["turnover"]); i += 1
    return np.array(vals), np.array(tos)

def metrics(vals, tos):
    r = vals[1:] / vals[:-1] - 1
    n = len(r)
    cum = vals[-1] - 1
    ann = vals[-1] ** (252 / n) - 1
    vol = r.std(ddof=1) * np.sqrt(252) if n > 1 else 0.0
    sharpe = r.mean() / r.std(ddof=1) * np.sqrt(252) if vol > 0 else 0.0
    mdd = (vals / np.maximum.accumulate(vals) - 1).min()
    return cum, ann, vol, sharpe, mdd, tos.mean()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", default="2024-01-01")
    ap.add_argument("--model", default="rl/models/ppo.zip")
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--turnover-penalty", type=float, default=0.0)
    ap.add_argument("--smooth", type=float, default=0.0)
    ap.add_argument("--data", default=None)
    ap.add_argument("--end", default=None)
    ap.add_argument("--mask-feats", action="store_true")
    ap.add_argument("--dd-limit", type=float, default=0.0)
    ap.add_argument("--dd-mult", type=float, default=4.0)
    ap.add_argument("--ext", action="store_true")
    ap.add_argument("--anchor", type=float, default=0.0)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    data = (load(a.data or PATH0, ext=a.ext)).split(a.start, a.end)
    env = PortfolioEnv(data, cost_bps=a.cost_bps, episode_len=None, random_start=False,
                       dd_limit=a.dd_limit, dd_mult=a.dd_mult)
    N = env.N
    def eqw(i):  # 시점 i의 거래가능 종목 동일비중
        m = data.elig[10 + i] if data.pit else np.ones(N, bool)
        return np.append(m / m.sum(), 0.0)
    cash = np.zeros(N + 1); cash[-1] = 1.0
    def hold(o):  # 현재 주식 비중 비율 유지, 현금은 목표 0 (오버레이가 켜지면 상한만큼만 투자)
        ws = o[10 * N * env.F: 10 * N * env.F + N].astype(np.float64)
        return np.append(ws / ws.sum(), 0.0) if ws.sum() > 0 else eqw(0)
    policies = {
        "동일비중 B&H": lambda o, i: logit(eqw(0)) if i == 0 else logit(hold(o)),  # 이후 현재(drift) 주식 비중 유지
        "동일비중 일일리밸": lambda o, i: logit(eqw(i)),
        "전액 현금": lambda o, i: logit(cash),
    }
    penv = PortfolioEnv(data, cost_bps=a.cost_bps, episode_len=None, random_start=False,
                        turnover_penalty=a.turnover_penalty, smooth=a.smooth, anchor_scale=a.anchor, mask_feats=a.mask_feats,
                             dd_limit=a.dd_limit, dd_mult=a.dd_mult)
    if os.path.exists(a.model):
        from stable_baselines3 import PPO
        m = PPO.load(a.model)
        policies["PPO"] = lambda o, i: m.predict(o, deterministic=True)[0]
        envs = {"PPO": penv}
    else:
        print(f"[모델 없음: {a.model}] 베이스라인만 평가")

    envs = locals().get("envs", {})
    print(f"구간 {str(data.dates[0])[:10]} ~ {str(data.dates[-1])[:10]}, T={len(data.dates)}, N={N}, cost={a.cost_bps}bps")
    print(f"{'정책':<16}{'누적수익':>10}{'연환산':>10}{'연변동성':>10}{'샤프':>8}{'MDD':>10}{'일턴오버':>10}")
    for name, pol in policies.items():
        c, an, v, s, d, t = metrics(*run(envs.get(name, env), pol))
        print(f"{name:<14}{c:>10.2%}{an:>10.2%}{v:>10.2%}{s:>8.2f}{d:>10.2%}{t:>10.4f}")
    spy_row(data)

def spy_row(data):
    import pandas as pd
    mp = "rl/data/market_long.parquet" if data.dates[0] < np.datetime64("2015-01-01") else "rl/data/market.parquet"
    spy = pd.read_parquet(mp).spy.reindex(pd.DatetimeIndex(data.dates)).ffill().values[10:len(data.dates) - 1]
    v = spy / spy[0]
    c, an, vol, sh, dd, _ = metrics(v, np.zeros(1))
    print(f"{'SPY(참고)':<14}{c:>10.2%}{an:>10.2%}{vol:>10.2%}{sh:>8.2f}{dd:>10.2%}{'-':>10}")

if __name__ == "__main__":
    main()
