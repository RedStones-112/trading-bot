"""PPO 학습: python -m rl.train --timesteps 300000 --seed 0 --out rl/models/ppo.zip"""
import argparse, os
from stable_baselines3 import PPO
from stable_baselines3.common.vec_env import DummyVecEnv
from stable_baselines3.common.monitor import Monitor
from rl.features import load, PATH as PATH0
from rl.env import PortfolioEnv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timesteps", type=int, default=300000)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", default="rl/models/ppo.zip")
    ap.add_argument("--n-envs", type=int, default=4)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--turnover-penalty", type=float, default=0.0)
    ap.add_argument("--smooth", type=float, default=0.0)
    ap.add_argument("--data", default=None)
    ap.add_argument("--market", default=None, help="시장특성 parquet(spy/vix 열), 국내는 rl/data/kr_market.parquet")
    ap.add_argument("--train-start", default=None)
    ap.add_argument("--train-end", default="2024-01-01")
    ap.add_argument("--mask-feats", action="store_true")
    ap.add_argument("--dd-limit", type=float, default=0.0)
    ap.add_argument("--dd-mult", type=float, default=4.0)
    ap.add_argument("--ext", action="store_true")
    ap.add_argument("--anchor", type=float, default=0.0)
    ap.add_argument("--rebalance-every", type=int, default=1, help="리밸런스 주기(거래일), 월 1회 = 21")
    ap.add_argument("--policy", choices=["mlp", "shared"], default="mlp")
    a = ap.parse_args()

    train = (load(a.data or PATH0, ext=a.ext, market=a.market)).split(a.train_start, a.train_end or None)

    def make(i):
        def f():
            e = PortfolioEnv(train, random_start=True, episode_len=252, cost_bps=a.cost_bps,
                             turnover_penalty=a.turnover_penalty, smooth=a.smooth, anchor_scale=a.anchor, mask_feats=a.mask_feats,
                             dd_limit=a.dd_limit, dd_mult=a.dd_mult, rebalance_every=a.rebalance_every)
            e.reset(seed=a.seed + i)
            return Monitor(e)
        return f

    venv = DummyVecEnv([make(i) for i in range(a.n_envs)])  # 환경이 가벼워 Dummy가 윈도우에서 가장 안정적/빠름
    if a.policy == "shared":  # 종목 공유 가중치 정책(rl/policy.py), pit 데이터 필요
        from rl.policy import SharedStockPolicy
        T, N, F = train.feats.shape
        pol, pk = SharedStockPolicy, dict(n_stocks=N, n_feats=F, window=10,
                                         extra=2 if a.dd_limit > 0 else 0)
    else:
        pol, pk = "MlpPolicy", dict(net_arch=dict(pi=[64, 64], vf=[64, 64]))
    model = PPO(pol, venv, seed=a.seed, device="cpu", verbose=1,
                n_steps=256, batch_size=256, n_epochs=5, learning_rate=3e-4, policy_kwargs=pk)
    model.learn(total_timesteps=a.timesteps)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    model.save(a.out)
    print("saved", a.out)


if __name__ == "__main__":
    main()
