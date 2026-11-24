"""국내 시가총액 상위 유니버스 수집(상장폐지 포함, 생존편향 없음). python -m rl.collect_kr_cap [top_n]

순위: FinanceData/marcap(KRX 전종목 일별 시가총액, 상폐 종목 포함) -- 매년 11/29 이후 첫 거래일에 직전 20거래일 평균
시가총액 상위 top_n. 대상은 코스피/코스닥 보통주(코드 끝자리 0)만 -- 우선주/스팩/KONEX 제외, 레버리지·인버스·ETF/ETN류는
이름으로 한 번 더 제외(장기 보유 시 시간가치 손실/변동성 끌림). 수익률용 가격은 FDR(네이버 수정주가, 2015~) --
marcap의 Close는 수정주가가 아니라 순위에만 씀. 시장특성은 collect_kr과 같은 rl/data/kr_market.parquet."""
import os, re, sys
import numpy as np
import pandas as pd
import requests
from concurrent.futures import ThreadPoolExecutor
from rl.collect_kr import START, fetch, fix_unadjusted

MARCAP_URL = "https://raw.githubusercontent.com/FinanceData/marcap/master/data/marcap-{y}.parquet"
MARCAP_DIR = "rl/data/marcap"
EXCLUDE = re.compile(r"스팩|레버리지|인버스|ETN|ETF|KODEX|TIGER|KBSTAR|ARIRANG|선물|2X|3X")


def marcap(years, refresh_last=True):
    os.makedirs(MARCAP_DIR, exist_ok=True)
    out = []
    for y in years:
        p = f"{MARCAP_DIR}/marcap-{y}.parquet"
        if not os.path.exists(p) or (refresh_last and y == years[-1]):
            r = requests.get(MARCAP_URL.format(y=y), timeout=300)
            r.raise_for_status()
            open(p, "wb").write(r.content)
        out.append(pd.read_parquet(p, columns=["Date", "Code", "Name", "Market", "Marcap"]))
    m = pd.concat(out)
    m = m[m.Market.isin(["KOSPI", "KOSDAQ", "KOSDAQ GLOBAL"]) & m.Code.str.fullmatch(r"\d{5}0")
          & ~m.Name.str.contains(EXCLUDE)]
    return m


def rank_dates(idx):
    return [idx[p] for y in range(idx[0].year, idx[-1].year + 1)
            if 0 < (p := idx.searchsorted(pd.Timestamp(f"{y}-11-29"))) < len(idx)]


def main(top_n=100):
    years = list(range(int(START[:4]), pd.Timestamp.today().year + 1))
    m = marcap(years)
    cap = m.pivot_table(index="Date", columns="Code", values="Marcap", aggfunc="last").sort_index()
    names = m.drop_duplicates("Code", keep="last").set_index("Code").Name
    rds = rank_dates(cap.index)
    tops = {}
    for rd in rds:
        pos = cap.index.get_loc(rd)
        tops[rd] = cap.iloc[max(0, pos - 20):pos].mean().nlargest(top_n).index
    union = sorted(set().union(*tops.values()))
    print(f"순위일 {[str(d.date()) for d in rds]}, union {len(union)}종목", flush=True)

    with ThreadPoolExecutor(8) as ex:
        got = dict(ex.map(fetch, union))
    ok = {c: d for c, d in got.items() if d is not None and len(d) > 20}
    print(f"시세 수집 {len(ok)}/{len(union)} (실패 {sorted(set(union) - set(ok))})", flush=True)
    close = pd.DataFrame({c: d["Close"] for c, d in ok.items()}).sort_index().astype(float)
    vol = pd.DataFrame({c: d["Volume"] for c, d in ok.items()}).reindex(close.index).astype(float)
    close = close.where(close > 0)
    last = cap.reindex(columns=close.columns).apply(lambda s: s.last_valid_index())
    delisted = {c for c, d in last.items() if d is not None and d < cap.index[-1] - pd.Timedelta(days=10)}
    close, vol, nfix = fix_unadjusted(close, vol, delisted)
    print(f"비정상 점프 {nfix}건 소급 보정", flush=True)

    idx = close.index
    elig = pd.DataFrame(False, index=idx, columns=close.columns)
    for i, rd in enumerate(rds):
        nxt = rds[i + 1] if i + 1 < len(rds) else idx[-1] + pd.Timedelta(days=1)
        cols = [c for c in tops[rd] if c in elig.columns]
        elig.loc[(idx >= rd) & (idx < nxt), cols] = True
    capd = cap.reindex(index=idx, columns=close.columns).ffill(limit=5)
    rows = []
    for c in close.columns:
        d = pd.DataFrame({"close": close[c], "volume": vol[c], "eligible": elig[c], "marcap": capd[c]}).dropna(subset=["close"])
        d["ticker"] = c
        rows.append(d.rename_axis("date").reset_index())
    path = f"rl/data/kr_cap{top_n}.parquet"
    pd.concat(rows).to_parquet(path, index=False)
    pd.DataFrame({"Name": names.reindex(close.columns), "delisted": [c in delisted for c in close.columns]},
                 index=close.columns).to_csv(f"rl/data/kr_cap{top_n}_names.csv", encoding="utf-8-sig")
    print(f"저장 {path}: {len(close.columns)}종목(상폐 {len(delisted)}), {idx[0].date()}~{idx[-1].date()}")


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 100)
