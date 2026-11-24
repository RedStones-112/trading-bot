"""실행기용 일봉 수집. 두 가지 백엔드, 반환 형식은 같음: (close[날짜x티커], volume[날짜x티커], market[spy,vix], 실패목록)

- "yfinance": PC/라즈베리파이 기본. 학습 데이터와 같은 소스(배당·분할 수정종가). 3년치를 매번 새로 받음(~2분).
- "kis"     : 안드로이드용(yfinance가 쓰는 curl_cffi를 Chaquopy에서 못 씀). KIS 해외 일봉(MODP=1 수정주가) +
              지수(VIX). state_dir/prices/에 종목별 CSV 캐시 -- 첫 실행 ~1시간(3년치), 이후 매일 종목당 1회 호출(~9분).
              최신 100일이 캐시와 0.5% 넘게 다르면(분할 등 소급 수정) 그 종목은 3년치를 다시 받음.
              주의: KIS 수정주가가 배당까지 반영하는지는 미확인 -- 학습(yfinance)과 미세한 차이 가능.
참고: 야후 차트 API를 requests로 직접 부르는 방식은 2026-11-24 429(Too Many Requests)로 막혀 폐기."""
import os
from datetime import date, timedelta
import numpy as np
import pandas as pd


def _cut(df, last_complete):
    return df[df.index <= pd.Timestamp(last_complete)]


def fetch_yfinance(symbols, last_complete, years=3):
    import yfinance as yf
    start = str(date.today() - timedelta(days=int(365 * years) + 10)); end = str(date.today() + timedelta(days=1))
    df = yf.download(symbols, start=start, end=end, auto_adjust=True, group_by="ticker", threads=True, progress=False)
    lv0 = set(df.columns.get_level_values(0))
    close = pd.DataFrame({t: df[t]["Close"] for t in symbols if t in lv0}).reindex(columns=symbols)
    vol = pd.DataFrame({t: df[t]["Volume"] for t in symbols if t in lv0}).reindex(index=close.index, columns=symbols)
    close = _cut(close.dropna(how="all"), last_complete); vol = vol.reindex(index=close.index)
    spy = yf.download("SPY", start=start, end=end, auto_adjust=True, progress=False)["Close"].squeeze()
    vix = yf.download("^VIX", start=start, end=end, progress=False)["Close"].squeeze()
    mk = _cut(pd.DataFrame({"spy": spy, "vix": vix}).dropna(), last_complete)
    bad = [s for s in symbols if close[s].isna().all()]
    return close, vol, mk, bad


class KisCache:
    def __init__(self, kis, state_dir, years=3, log=print):
        self.kis, self.dir, self.years, self.log = kis, os.path.join(state_dir, "prices"), years, log
        os.makedirs(self.dir, exist_ok=True)

    def _path(self, s):
        return os.path.join(self.dir, s.replace("^", "_") + ".csv")

    def _read(self, s):
        p = self._path(s)
        if not os.path.exists(p):
            return None
        return pd.read_csv(p, index_col=0, parse_dates=True)

    def _full(self, ex, s, since):
        rows, bymd = [], ""
        while True:
            chunk = self.kis.daily(ex, s, bymd)
            if not chunk:
                break
            rows += chunk
            first = min(r[0] for r in chunk)
            if first <= since.strftime("%Y%m%d") or len(chunk) < 100:
                break
            bymd = (pd.Timestamp(first) - pd.Timedelta(days=1)).strftime("%Y%m%d")
        return rows

    def symbol(self, ex, s, last_complete):
        since = pd.Timestamp(date.today() - timedelta(days=int(365 * self.years) + 10))
        old = self._read(s)
        recent = self.kis.daily(ex, s)
        new = pd.DataFrame(recent, columns=["d", "close", "volume"])
        new.index = pd.to_datetime(new.pop("d")); new = new.sort_index()
        if old is not None and len(new):
            ov = old.index.intersection(new.index)
            drift = (abs(old.loc[ov, "close"] / new.loc[ov, "close"] - 1)).max() if len(ov) else 1.0
            if len(ov) and drift < 0.005:
                merged = pd.concat([old[old.index < new.index[0]], new])
            else:
                merged = None  # 소급 수정(분할 등) 또는 캐시 공백 -> 전체 재수집
        else:
            merged = None
        if merged is None:
            full = pd.DataFrame(self._full(ex, s, since), columns=["d", "close", "volume"])
            full.index = pd.to_datetime(full.pop("d"))
            merged = full[~full.index.duplicated()].sort_index()
        merged = merged[merged.index >= since]
        merged.to_csv(self._path(s))
        return _cut(merged, last_complete)

    def vix(self, last_complete):
        old = self._read("^VIX")
        start = (old.index[-1] - pd.Timedelta(days=10)) if old is not None and len(old) else \
            pd.Timestamp(date.today() - timedelta(days=int(365 * self.years) + 10))
        rows, d1 = [], start
        while d1 <= pd.Timestamp(date.today()):
            d2 = min(d1 + pd.Timedelta(days=130), pd.Timestamp(date.today()))
            rows += self.kis.index_daily("VIX", d1.strftime("%Y%m%d"), d2.strftime("%Y%m%d"))
            d1 = d2 + pd.Timedelta(days=1)
        new = pd.DataFrame(rows, columns=["d", "close"]); new.index = pd.to_datetime(new.pop("d"))
        merged = pd.concat([old, new]) if old is not None else new
        merged = merged[~merged.index.duplicated(keep="last")].sort_index()
        merged.to_csv(self._path("^VIX"))
        return _cut(merged, last_complete)


def fetch_kis(kis, symbols, exmap, last_complete, state_dir, years=3, log=print, progress=None):
    cache = KisCache(kis, state_dir, years, log)
    closes, vols, bad = {}, {}, []
    todo = [s for s in symbols if s in exmap and "-" not in s]
    for i, s in enumerate(todo):
        try:
            d = cache.symbol(exmap[s], s, last_complete)
            closes[s], vols[s] = d["close"], d["volume"]
        except Exception as e:
            bad.append(s); log(f"    {s} 수집 실패: {e}")
        if progress and i % 20 == 0:
            progress(i, len(todo))
    close = pd.DataFrame(closes).sort_index().reindex(columns=symbols)
    vol = pd.DataFrame(vols).reindex(index=close.index, columns=symbols)
    spy = cache.symbol("AMEX", "SPY", last_complete)["close"]
    mk = pd.DataFrame({"spy": spy, "vix": cache.vix(last_complete)["close"]}).dropna()
    bad += [s for s in symbols if s not in todo]
    return close, vol, mk, bad
