"""parquet -> 학습용 배열. MarketData.feats[T,N,F], rets[T,N] (rets[t]=t일 종가수익률)."""
from dataclasses import dataclass
import numpy as np
import pandas as pd

PATH = "rl/data/us_top100_daily.parquet"
FEATURE_NAMES = ["ret1", "ret5", "ret20", "vol20", "sma50_gap", "volume_ratio"]

@dataclass
class MarketData:
    dates: np.ndarray    # [T] datetime64
    tickers: list        # [N]
    feats: np.ndarray    # [T,N,F] float32, 클리핑됨, 결측 0
    rets: np.ndarray     # [T,N] float32, 결측 0
    elig: np.ndarray = None  # [T,N] bool, 해당 시점 상위100(거래 가능) 여부. None이면 전부 가능
    pit: bool = False

    def split(self, start=None, end=None):
        """[start, end) 날짜 구간 슬라이스 (문자열 'YYYY-MM-DD')."""
        m = np.ones(len(self.dates), bool)
        if start: m &= self.dates >= np.datetime64(start)
        if end: m &= self.dates < np.datetime64(end)
        return MarketData(self.dates[m], self.tickers, self.feats[m], self.rets[m],
                          None if self.elig is None else self.elig[m], self.pit)

def load(path=PATH, ext=False, market=None, tickers=None) -> MarketData:
    """market: 시장특성 parquet 경로(기본은 기간에 따라 자동), tickers: 열 순서 고정(학습 때 종목 순서 재현용)."""
    df = pd.read_parquet(path)
    close = df.pivot(index="date", columns="ticker", values="close").sort_index()
    if tickers is not None:
        close = close.reindex(columns=tickers)
    vol = df.pivot(index="date", columns="ticker", values="volume").reindex(index=close.index, columns=close.columns)
    elig = None
    if "eligible" in df.columns:
        elig = df.pivot(index="date", columns="ticker", values="eligible").reindex(index=close.index, columns=close.columns)
    mk = None
    if ext:
        mpath = market or ("rl/data/market_long.parquet" if close.index[0] < pd.Timestamp("2015-01-01") else "rl/data/market.parquet")
        mk = pd.read_parquet(mpath)
    return from_frames(close, vol, elig, mk, ext)


def from_frames(close, vol, elig=None, market=None, ext=False) -> MarketData:
    """메모리 DataFrame으로 특성 계산(parquet/pyarrow 불필요 -- 라즈베리파이/안드로이드 실행기용).
    close/vol/elig: [날짜 x 티커] (열 순서가 곧 모델의 종목 순서), market: spy/vix 열을 가진 일봉 DataFrame."""
    close = close.sort_index()
    vol = vol.reindex(index=close.index, columns=close.columns)
    ret1 = close.pct_change(fill_method=None)
    f = [
        ret1,
        close.pct_change(5, fill_method=None),
        close.pct_change(20, fill_method=None),
        ret1.rolling(20).std(),
        close / close.rolling(50).mean() - 1,
        vol / vol.rolling(20).mean() - 1,
    ]
    scale = [10, 5, 3, 20, 3, 1]  # 대략적 정규화 후 [-5,5] 클리핑
    if ext:
        mk = market.reindex(close.index).ffill()
        mret = mk.spy.pct_change()
        mkt = [mret, mk.spy.pct_change(20), mret.rolling(20).std(),
               mk.spy / mk.spy.rolling(200).mean() - 1, mk.vix / 20 - 1, mk.vix.pct_change(5)]
        mscale = [10, 3, 20, 3, 1, 2]
        r20 = close.pct_change(20, fill_method=None)
        d = close.diff()
        rsi = 1 - 1 / (1 + d.clip(lower=0).rolling(14).mean() / ((-d.clip(upper=0)).rolling(14).mean() + 1e-9))
        f += [close.pct_change(60, fill_method=None), close.pct_change(120, fill_method=None),
              close.pct_change(250, fill_method=None), close / close.rolling(252).max() - 1,
              rsi - 0.5, r20.sub(r20.mean(axis=1), axis=0), r20.rank(axis=1, pct=True) - 0.5]
        scale += [2, 1.5, 1, 3, 4, 3, 4]
        cols = [np.clip(x.values * s, -5, 5) for x, s in zip(f, scale)]
        cols += [np.clip(np.repeat(m.values[:, None], close.shape[1], 1) * s, -5, 5)
                 for m, s in zip(mkt, mscale)]
        feats = np.stack(cols, axis=-1)
    else:
        feats = np.stack([np.clip(x.values * s, -5, 5) for x, s in zip(f, scale)], axis=-1)
    feats = np.nan_to_num(feats).astype(np.float32)
    rets = np.nan_to_num(ret1.values).astype(np.float32)
    pit = elig is not None
    if pit:
        elig = elig.reindex(index=close.index, columns=close.columns).fillna(False).values.astype(bool)
    return MarketData(close.index.values, list(close.columns), feats, rets, elig, pit)
