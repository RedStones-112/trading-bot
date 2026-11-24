"""시점별(point-in-time) 유니버스 수집.
후보 풀(현재 S&P500 + 기존 POOL) 10년+1년 일봉 -> 1년 주기(2016-11-16 이후 매년)로 직전 252거래일 평균 거래대금
상위 100 선정 -> '한 번이라도 들어간' 종목 전부 저장, 행마다 해당 시점 상위100 여부(eligible) 기록.
한계: 과거에 S&P500에서 빠졌거나 상장폐지된 종목은 풀에 없음(yfinance에 데이터 없음) -> 잔여 생존편향."""
import io, requests
from datetime import date, timedelta
import numpy as np, pandas as pd, yfinance as yf
from rl.collect_data import POOL

def main(years=10, out_path="rl/data/us_pit_daily.parquet", top_n=100):
    html = requests.get("https://en.wikipedia.org/wiki/List_of_S%26P_500_companies",
                        headers={"User-Agent": "Mozilla/5.0"}, timeout=30).text
    cur = pd.read_html(io.StringIO(html))[0]["Symbol"].str.replace(".", "-", regex=False).tolist()
    pool = sorted(set(cur) | set(POOL))
    end = date.today(); start = end - timedelta(days=365 * (years + 1) + 10)
    print(f"pool={len(pool)} {start}~{end}", flush=True)
    df = yf.download(pool, start=str(start), end=str(end), auto_adjust=True, group_by="ticker",
                     threads=True, progress=False)
    close = pd.DataFrame({t: df[t]["Close"] for t in pool if t in df.columns.get_level_values(0)})
    vol = pd.DataFrame({t: df[t]["Volume"] for t in close.columns})
    dv = (close * vol)
    first = close.index[close.index >= pd.Timestamp(end - timedelta(days=365 * years))][0]
    rank_dates = [first + pd.DateOffset(years=k) for k in range(years + 1)]
    rank_dates = [close.index[close.index.searchsorted(d)] for d in rank_dates if d < close.index[-1]]
    elig = pd.DataFrame(False, index=close.index, columns=close.columns)
    for i, rd in enumerate(rank_dates):
        pos = close.index.get_loc(rd)
        score = dv.iloc[max(0, pos - 252):pos].mean()           # rd 이전 데이터만 사용
        n_hist = close.iloc[max(0, pos - 252):pos].notna().sum()
        top = score[n_hist >= 200].nlargest(top_n).index          # 직전 1년 거래 이력 충분한 종목만
        nxt = rank_dates[i + 1] if i + 1 < len(rank_dates) else close.index[-1] + pd.Timedelta(days=1)
        elig.loc[(elig.index >= rd) & (elig.index < nxt), top] = True
    union = elig.columns[elig.any()]
    m = close.index >= (close.index[0] if years > 10 else first)  # 장기판은 워밍업 구간도 보존
    rows = []
    for t in union:
        d = df[t].loc[m].dropna(subset=["Close"]).copy()
        d["ticker"] = t; d["eligible"] = elig.loc[d.index, t].values
        rows.append(d.reset_index())
    out = pd.concat(rows); out.columns = [c.lower() for c in out.columns]
    out.to_parquet(out_path, index=False)
    print(f"union={len(union)} rows={len(out)} rank_dates={[str(d.date()) for d in rank_dates]}")
    print("eligible per rank-year:", [int(elig.loc[d].sum()) for d in rank_dates])

if __name__ == "__main__":
    import sys
    y = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    out = sys.argv[2] if len(sys.argv) > 2 else "rl/data/us_pit_daily.parquet"
    main(y, out, int(sys.argv[3]) if len(sys.argv) > 3 else 100)
