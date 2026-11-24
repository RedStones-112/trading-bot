"""SharedStockPolicy(rl/policy.py)의 결정적 행동을 numpy만으로 계산 -- 라즈베리파이/안드로이드에서 PyTorch 불필요.

    python -m rl.live.np_policy export   # rl/models/exp8r_s*.zip -> rl/live/model/exp8r.npz (PC에서 1회, torch 필요)
    python -m rl.live.np_policy verify   # torch 결과와 numpy 결과 비교

결정적 행동 = 정책 평균(점수) -> SB3 predict와 같게 행동공간 [-5, 5]로 클리핑. 여러 시드는 평균."""
import os, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NPZ = os.path.join(ROOT, "rl", "live", "model", "exp8r.npz")
SRC = [os.path.join(ROOT, "rl", "models", f"exp8r_s{s}.zip") for s in range(3)]
LAYERS = ["per.0", "per.2", "score", "glob.0", "cash"]


class NpPolicy:
    def __init__(self, path=NPZ):
        z = np.load(path)
        self.n, self.f, self.w, self.extra = (int(x) for x in z["cfg"])
        self.k = int(z["k"])
        self.p = [{l: (z[f"{i}/{l}.weight"], z[f"{i}/{l}.bias"]) for l in LAYERS} for i in range(self.k)]

    def _one(self, p, obs):
        N, F, W = self.n, self.f, self.w
        k = W * N * F
        x = obs[:k].reshape(W, N, F).transpose(1, 0, 2).reshape(N, W * F)
        wt, elig, ext = obs[k:k + N + 1], obs[k + N + 1:k + 2 * N + 1], obs[k + 2 * N + 1:]
        lin = lambda name, v: v @ p[name][0].T + p[name][1]
        h = np.tanh(lin("per.2", np.tanh(lin("per.0", np.concatenate([x, wt[:N, None], elig[:, None]], 1)))))
        m = elig[:, None]
        pooled = (h * m).sum(0) / max(m.sum(), 1.0)
        g = np.concatenate([pooled, wt[N:], ext])
        return np.concatenate([lin("score", h)[:, 0], lin("cash", np.tanh(lin("glob.0", g)))])

    def act(self, obs):
        obs = np.asarray(obs, np.float64)
        return np.clip(np.mean([self._one(p, obs) for p in self.p], 0), -5, 5)


def export():
    from stable_baselines3 import PPO
    out, cfg = {}, None
    for i, path in enumerate(SRC):
        pol = PPO.load(path, device="cpu").policy
        sc = pol.mlp_extractor
        cfg = (sc.n, sc.f, sc.w, sc.extra)
        sd = sc.state_dict()
        for l in LAYERS:
            out[f"{i}/{l}.weight"] = sd[f"{l}.weight"].numpy().astype(np.float32)
            out[f"{i}/{l}.bias"] = sd[f"{l}.bias"].numpy().astype(np.float32)
    os.makedirs(os.path.dirname(NPZ), exist_ok=True)
    np.savez_compressed(NPZ, cfg=np.array(cfg), k=np.array(len(SRC)), **out)
    print("saved", NPZ, f"{os.path.getsize(NPZ) / 1024:.0f}KB", "cfg", cfg)


def verify(n=20):
    from stable_baselines3 import PPO
    npp = NpPolicy()
    models = [PPO.load(p, device="cpu") for p in SRC]
    rng = np.random.default_rng(0)
    dim = npp.w * npp.n * npp.f + 2 * npp.n + 1 + npp.extra
    worst = 0.0
    for _ in range(n):
        obs = rng.normal(0, 1, dim).astype(np.float32)
        obs[npp.w * npp.n * npp.f + npp.n + 1:npp.w * npp.n * npp.f + 2 * npp.n + 1] = rng.random(npp.n) < 0.65
        ref = np.mean([m.predict(obs, deterministic=True)[0] for m in models], 0)
        worst = max(worst, float(np.abs(ref - npp.act(obs)).max()))
    print(f"torch vs numpy 최대 차이 {worst:.2e} ({'OK' if worst < 1e-4 else '불일치'})")


if __name__ == "__main__":
    {"export": export, "verify": verify}[sys.argv[1]]()
