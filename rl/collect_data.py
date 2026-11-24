"""미국 주식 최근 10년 일봉 수집 -> 평균 거래대금 상위 100개 선정 -> rl/data/ 저장.

거래순위 상위 100개 = 후보 풀(대형 유동성 종목) 중 최근 1년 평균 거래대금(Close*Volume) 상위 100.
주의: 현재 시점 기준 선정이라 생존편향이 있음(10년 전 상위였다가 사라진 종목 제외).
"""
import sys
from datetime import date, timedelta
import pandas as pd
import yfinance as yf

POOL = """AAPL MSFT NVDA AMZN GOOGL META TSLA AVGO AMD NFLX INTC ORCL CRM ADBE CSCO QCOM TXN MU AMAT
LRCX KLAC PLTR UBER SHOP SNOW PANW CRWD ANET SMCI ARM COIN HOOD SOFI RIVN LCID NIO F GM T VZ CMCSA DIS
JPM BAC WFC C GS MS SCHW V MA PYPL AXP BRK-B XOM CVX OXY SLB COP PFE MRK JNJ LLY ABBV BMY UNH CVS MRNA
GILD AMGN WMT COST HD LOW TGT NKE SBUX MCD KO PEP PG MDLZ CAT DE BA LMT GE RTX HON UPS FDX
MARA RIOT AAL DAL UAL CCL NCLH CMG ABNB DKNG ROKU SNAP PINS TWLO DDOG NET ZS MDB U RBLX AFRM UPST
CHWY ETSY EBAY BABA PDD JD NU VALE PBR KMI WBD PARA CLF X FCX NEM AA HAL DVN""".split()

def main():
    end = date.today()
    start = end - timedelta(days=365 * 10 + 10)
    print(f"pool={len(POOL)} {start}~{end}")
    df = yf.download(POOL, start=str(start), end=str(end), auto_adjust=True,
                     group_by="ticker", threads=True, progress=False)
    frames, score = [], {}
    for t in POOL:
        try:
            d = df[t].dropna(subset=["Close"]).copy()
        except KeyError:
            continue
        if len(d) < 252 * 3:  # 상장 3년 미만 제외
            continue
        d["ticker"] = t
        frames.append(d.reset_index())
        score[t] = (d["Close"] * d["Volume"]).tail(252).mean()
    top = sorted(score, key=score.get, reverse=True)[:100]
    out = pd.concat([f for f in frames if f["ticker"].iloc[0] in top])
    out.columns = [c.lower() for c in out.columns]
    out.to_parquet("rl/data/us_top100_daily.parquet", index=False)
    pd.Series(top).to_csv("rl/data/universe.csv", index=False, header=["ticker"])
    print(f"saved {len(top)} tickers, {len(out)} rows; "
          f"history {out.date.min().date()}~{out.date.max().date()}")
    print("dropped(<3y or missing):", sorted(set(POOL) - set(score)))

if __name__ == "__main__":
    sys.exit(main())
