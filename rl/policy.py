"""종목 공유 가중치 정책(SharedStockPolicy) -- 종목 수 N이 커도 파라미터가 N에 비례해 늘지 않음.

관측(env.PortfolioEnv, pit 데이터) = [feats(window,N,F) 평탄화 | 비중(N+1) | eligible(N) | 낙폭정보(extra)].
- 종목별: 최근 window일 특성 + 현재 비중 + 거래가능 여부 -> 공유 MLP -> 임베딩 h_i -> 점수 s_i
- 전체: 거래가능 종목 임베딩 평균 + 현금 비중 + extra -> 현금 점수, 가치함수
행동 평균 = [s_1..s_N, s_cash] (env가 앵커/softmax로 비중 변환). 기존 MLP 정책과 달리 종목 순서/개수에 무관한 구조."""
import torch
from torch import nn
from stable_baselines3.common.policies import ActorCriticPolicy


class StockScorer(nn.Module):
    def __init__(self, n, f, window, extra, hid=64):
        super().__init__()
        self.n, self.f, self.w, self.extra = n, f, window, extra
        self.latent_dim_pi, self.latent_dim_vf = n + 1, hid
        self.per = nn.Sequential(nn.Linear(window * f + 2, hid), nn.Tanh(), nn.Linear(hid, hid), nn.Tanh())
        self.score = nn.Linear(hid, 1)
        self.glob = nn.Sequential(nn.Linear(hid + 1 + extra, hid), nn.Tanh())
        self.cash = nn.Linear(hid, 1)
        self.vf = nn.Sequential(nn.Linear(hid + 1 + extra, hid), nn.Tanh(), nn.Linear(hid, hid), nn.Tanh())
        nn.init.zeros_(self.score.weight); nn.init.zeros_(self.score.bias)  # 시작은 동일비중(앵커)
        nn.init.zeros_(self.cash.weight); nn.init.zeros_(self.cash.bias)

    def _embed(self, obs):
        B, N, F, W = obs.shape[0], self.n, self.f, self.w
        k = W * N * F
        x = obs[:, :k].view(B, W, N, F).permute(0, 2, 1, 3).reshape(B, N, W * F)
        wt = obs[:, k:k + N + 1]
        elig = obs[:, k + N + 1:k + 2 * N + 1]
        ext = obs[:, k + 2 * N + 1:]
        h = self.per(torch.cat([x, wt[:, :N, None], elig[..., None]], -1))
        m = elig[..., None]
        pooled = (h * m).sum(1) / m.sum(1).clamp(min=1.0)
        g = torch.cat([pooled, wt[:, N:], ext], -1)
        return h, g

    def forward_actor(self, obs):
        h, g = self._embed(obs)
        return torch.cat([self.score(h).squeeze(-1), self.cash(self.glob(g))], -1)

    def forward_critic(self, obs):
        return self.vf(self._embed(obs)[1])

    def forward(self, obs):
        h, g = self._embed(obs)
        return torch.cat([self.score(h).squeeze(-1), self.cash(self.glob(g))], -1), self.vf(g)


class SharedStockPolicy(ActorCriticPolicy):
    def __init__(self, observation_space, action_space, lr_schedule, n_stocks=0, n_feats=0, window=10,
                 extra=0, hid=64, **kw):
        self._cfg = (n_stocks, n_feats, window, extra, hid)
        super().__init__(observation_space, action_space, lr_schedule, **kw)
        # 점수가 곧 행동 평균: 기본 Linear(N+1 -> N+1) 대신 항등 사상, 옵티마이저 재생성
        self.action_net = nn.Identity()
        self.optimizer = self.optimizer_class(self.parameters(), lr=lr_schedule(1), **self.optimizer_kwargs)

    def _build_mlp_extractor(self):
        self.mlp_extractor = StockScorer(*self._cfg)
