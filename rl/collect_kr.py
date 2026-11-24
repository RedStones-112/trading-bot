"""국내(KRX) 시점별 유니버스 수집 -- **상장폐지 종목 포함**(생존편향 제거). python -m rl.collect_kr [top_n]

후보 풀 = 현재 상장(코스피/코스닥, KONEX 제외) + 2015년 이후 상장폐지된 주권 보통주. 우선주(코드 끝자리 != 0)/스팩 제외.
시세: FinanceDataReader(네이버, 수정주가) -- 2015년 무렵부터만 제공되어 약 10년치.
매년 11/29 이후 첫 거래일에 직전 252거래일 평균 거래대금(종가x거래량) 상위 top_n -> 한 번이라도 든 종목 전부 저장.
시장특성(market): spy 자리에 코스피(KS11), vix 자리에 코스피 20일 실현변동성(연율 %, VKOSPI 대용 -- 무료 소스 없음).
20년치가 필요하면 pykrx(KRX 로그인 필요: 환경변수 KRX_ID/KRX_PW)로 확장 가능."""
import sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import pandas as pd
import FinanceDataReader as fdr

START = "2015-01-01"


def pool():
    cur = fdr.StockListing("KRX")
    cur = cur[cur["Market"].isin(["KOSPI", "KOSDAQ", "KOSDAQ GLOBAL"])][["Code", "Name"]]
    dl = fdr.StockListing("KRX-DELISTING")
    dl["DelistingDate"] = pd.to_datetime(dl["DelistingDate"], errors="coerce")
    dl = dl[(dl.SecuGroup == "주권") & (dl.Kind == "보통주") & (dl.DelistingDate >= START)]
    dl = dl.rename(columns={"Symbol": "Code"})[["Code", "Name"]]
    p = pd.concat([cur.assign(delisted=False), dl.assign(delisted=True)]).drop_duplicates("Code")
    p = p[p.Code.str.fullmatch(r"\d{5}0") & ~p.Name.str.contains("스팩")]
    return p.reset_index(drop=True)


def fetch(code):
    for _ in range(3):
        try:
            d = fdr.DataReader(code, START)
            return code, d[["Close", "Volume"]] if len(d) else None
        except Exception:
            pass
    return code, None


def fix_unadjusted(close, vol, delisted, limit=0.305, tail=15):
    """수정주가에 반영 안 된 감자/병합 등으로 생긴 가격제한폭 초과 점프를 제거: 점프 이전 가격을 비율만큼 소급 조정
    (거래량은 역비율 -- 거래대금 보존). 상장폐지 종목의 마지막 tail거래일(정리매매, 가격제한 없음)은 실제 손익이라 유지."""
    close, vol, n = close.copy(), vol.copy(), 0
    for c in close.columns:
        s = close[c]
        valid = s.dropna()
        if len(valid) < 2:
            continue
        r = valid.pct_change()
        keep_from = valid.index[-tail] if (c in delisted and len(valid) > tail) else None
        for d in r.index[r.abs() > limit]:
            if keep_from is not None and d >= keep_from:
                continue
            f = valid.loc[d] / valid.shift(1).loc[d]
            before = close.index < d
            close.loc[before, c] *= f
            vol.loc[before, c] /= f
            valid = close[c].dropna()
            n += 1
    return close, vol, n


def main(top_n=100):
    p = pool()
    print(f"후보 풀 {len(p)} (상폐 {int(p.delisted.sum())})", flush=True)
    with ThreadPoolExecutor(8) as ex:
        got = dict(ex.map(fetch, p.Code))
    ok = {c: d for c, d in got.items() if d is not None and len(d) > 20}
    print(f"시세 수집 {len(ok)}/{len(p)}", flush=True)
    close = pd.DataFrame({c: d["Close"] for c, d in ok.items()}).sort_index()
    vol = pd.DataFrame({c: d["Volume"] for c, d in ok.items()}).reindex(close.index)
    close = close.where(close > 0)  # 거래정지일 0 처리 방지
    close, vol, nfix = fix_unadjusted(close, vol, set(p.Code[p.delisted]))
    print(f"가격제한(±30%) 초과 비정상 점프 {nfix}건 소급 보정(상폐 직전 15거래일 정리매매는 유지)", flush=True)
    dv = close * vol
    idx = close.index
    rds = []
    for y in range(idx[0].year, idx[-1].year + 1):
        pos = idx.searchsorted(pd.Timestamp(f"{y}-11-29"))
        if pos < len(idx) and pos >= 200:
            rds.append(idx[pos])
    elig = pd.DataFrame(False, index=idx, columns=close.columns)
    for i, rd in enumerate(rds):
        pos = idx.get_loc(rd)
        lo = max(0, pos - 252)
        score = dv.iloc[lo:pos].mean()
        top = score[close.iloc[lo:pos].notna().sum() >= 150].nlargest(top_n).index
        nxt = rds[i + 1] if i + 1 < len(rds) else idx[-1] + pd.Timedelta(days=1)
        elig.loc[(idx >= rd) & (idx < nxt), top] = True
    union = elig.columns[elig.any()]
    rows = []
    for c in union:
        d = pd.DataFrame({"close": close[c], "volume": vol[c], "eligible": elig[c]}).dropna(subset=["close"])
        d["ticker"] = c
        rows.append(d.rename_axis("date").reset_index())
    out = pd.concat(rows)
    path = f"rl/data/kr_pit{top_n}.parquet"
    out.to_parquet(path, index=False)
    names = p.set_index("Code").loc[union]
    names.to_csv(f"rl/data/kr_pit{top_n}_names.csv", encoding="utf-8-sig")
    ks = fdr.DataReader("KS11", START)["Close"]
    rv = ks.pct_change().rolling(20).std() * np.sqrt(252) * 100
    pd.DataFrame({"spy": ks, "vix": rv}).dropna().to_parquet("rl/data/kr_market.parquet")
    print(f"저장 {path}: union {len(union)}종목(상폐 {int(names.delisted.sum())}), rows {len(out)}, "
          f"순위일 {[str(d.date()) for d in rds]}")


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 100)
