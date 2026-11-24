"""포트폴리오 배분 환경. 행동=[N+1] 실수(마지막은 현금) -> softmax -> 목표비중.
관측: 최근 window일 feats 평탄화 + 현재 비중. 보상: log(1+ 순수익률), 순수익률 = w·ret - cost*turnover.
t일 종가에서 비중을 정하고 t+1일 수익률이 실현됨(미래누수 없음)."""
import gymnasium as gym
import numpy as np
from gymnasium import spaces
from rl import ddctl

class PortfolioEnv(gym.Env):
    def __init__(self, data, window=10, cost_bps=5.0, episode_len=None, random_start=False,
                 turnover_penalty=0.0, smooth=0.0, anchor_scale=0.0, mask_feats=False,
                 dd_limit=0.0, dd_mult=4.0, rebalance_every=1):
        self.k = max(1, int(rebalance_every))  # 한 step = k거래일(첫날만 리밸런스, 나머지는 보유하며 drift)
        self.mask_feats = mask_feats
        self.dd_limit, self.dd_mult = dd_limit, dd_mult  # 낙폭 제어 오버레이(rl/ddctl.py), 0이면 끔
        self.tp, self.smooth, self.anchor = turnover_penalty, smooth, anchor_scale
        self.d, self.window, self.cost = data, window, cost_bps / 1e4
        self.T, self.N, self.F = data.feats.shape
        self.episode_len = episode_len
        self.random_start = random_start
        self.action_space = spaces.Box(-5, 5, (self.N + 1,), np.float32)
        self.observation_space = spaces.Box(-np.inf, np.inf,
            (window * self.N * self.F + self.N + 1 + (self.N if data.pit else 0) + (2 if dd_limit > 0 else 0),),
            np.float32)

    def _obs(self):
        sl = slice(self.t - self.window + 1, self.t + 1)
        x = self.d.feats[sl]
        if self.mask_feats and self.d.pit:  # 거래 불가(순위권 밖) 종목 특성은 숨김
            x = x * self.d.elig[sl][:, :, None]
        x = x.ravel()
        parts = [x, self.w] + ([self.d.elig[self.t].astype(np.float32)] if self.d.pit else [])
        if self.dd_limit > 0:  # 현재 낙폭과 허용 노출 상한
            parts.append(np.array([self.value / self.peak - 1,
                ddctl.exposure_cap(self.value, self.peak, self.dd_limit, self.dd_mult)], np.float32))
        return np.concatenate(parts).astype(np.float32)

    def reset(self, seed=None, options=None):
        super().reset(seed=seed)
        lo = self.window
        hi = self.T - 2
        if self.episode_len:
            hi = max(lo, self.T - 1 - self.episode_len)
        self.t0 = int(self.np_random.integers(lo, hi + 1)) if self.random_start else lo
        self.t = self.t0
        self.end = min(self.T - 2, self.t0 + self.episode_len) if self.episode_len else self.T - 2
        self.w = np.zeros(self.N + 1, np.float32); self.w[-1] = 1.0
        self.value = 1.0
        self.peak = 1.0
        return self._obs(), {"value": self.value}

    def step(self, action):
        a = np.asarray(action, np.float64)
        if self.anchor > 0:  # 동일비중 앵커: 로짓 = 기준(주식 0, 현금 -2) + scale*action (편차만 학습)
            base = np.zeros(self.N + 1); base[-1] = -2.0
            a = base + self.anchor * a
        if self.d.pit:  # 해당 시점 상위100이 아닌 종목은 거래 불가(비중 0)
            a = a.copy(); a[:-1][~self.d.elig[self.t]] = -1e9
        e = np.exp(a - a.max()); nw = (e / e.sum()).astype(np.float32)
        if self.smooth > 0:
            nw = (self.smooth * self.w + (1 - self.smooth) * nw).astype(np.float32)
        nw = ddctl.apply(nw, self.value, self.peak, self.dd_limit, self.dd_mult).astype(np.float32)
        turnover = np.abs(nw - self.w).sum()
        v0, path, w = self.value, [], nw
        for j in range(self.k):
            r = self.d.rets[self.t + 1]
            gross = float(w[:-1] @ r)
            net = max(gross - (self.cost * turnover if j == 0 else 0.0), -0.99)
            self.value *= 1 + net
            self.peak = max(self.peak, self.value)
            path.append(self.value)
            # 가격 변동에 따른 비중 drift
            drift = np.append(w[:-1] * (1 + r), w[-1]); w = (drift / drift.sum()).astype(np.float32)
            self.t += 1
            if self.t >= self.end:
                break
        self.w = w
        done = self.t >= self.end
        return self._obs(), float(np.log(self.value / v0)) - self.tp * float(turnover), done, False,             {"value": self.value, "turnover": float(turnover), "path": path}
