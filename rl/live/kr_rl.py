"""국내 RL(코스피/코스닥 시가총액 상위 100, 월 1회 리밸런스, rl/collect_kr_cap.py 데이터로 학습한 모델)
-- C++ 봇의 strategy_mode "rl"이 부르는 헬퍼.

  python -m rl.live.kr_rl targets --holdings rl_holdings.json --out rl_targets.json
      FDR(네이버) 일봉으로 학습과 같은 특성/순위권을 만들고, 시드 모델들의 결정적 행동 평균 -> 앵커 0.3 softmax
      목표비중을 out에 씀. holdings = C++이 쓴 {"cash": 원, "values": {코드: 평가금액}} (정책 입력의 현재 비중).
      오늘(KST) 날짜 행은 장중 미완성 봉이라 버리고 '직전 완성 종가' 기준으로 계산.
  python -m rl.live.kr_rl send --file msg.txt
      kakao_bridge(127.0.0.1:8765)로 카톡 "나에게 보내기". 안 떠 있으면 한 번 띄워보고, 실패해도 종료코드 0(매매와 무관).

종목 집합 = 학습 때 종목(universe_kc100.json, 열 순서 = 모델 종목 순서, 순위일별 상위 목록 "tops" 포함).
학습 데이터 이후의 새 순위일(매년 11/29 이후 첫 거래일)이 오면 FinanceData/marcap으로 그날 시가총액 상위 100을 다시
뽑되, 학습 집합 밖 신규 종목은 담을 수 없음(관측 크기 고정) -> 연 1회 collect_kr_cap + 재학습 권장.
레버리지/인버스/ETF/ETN은 유니버스(보통주만)와 이름 필터로 제외."""
import argparse, glob, json, os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta
import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
UNIVERSE = os.path.join(HERE, "universe_kc100.json")
EXTRA_TOPS = "rl_tops_extra.json"  # 학습 이후 새 순위일 결과 캐시(봇 작업 폴더)
MODELS = os.path.join(HERE, "model", "kc_s*.zip")
WINDOW, ANCHOR, TOP_N, YEARS = 10, 0.3, 100, 3
BRIDGE = "http://127.0.0.1:8765"
BRIDGE_DIR = r"C:\Users\wogur\OneDrive\Desktop\project\kakao_bridge"


def eligibility(close, tops):
    """순위일별 상위 목록(tops: {날짜: [코드]})으로 날짜별 순위권 마스크. 학습 이후 새 순위일은 marcap으로 추가."""
    idx = close.index
    known = dict(tops)
    try:
        known.update(json.load(open(EXTRA_TOPS, encoding="utf-8")))
    except Exception:
        pass
    last = max(known)
    for y in range(int(last[:4]), idx[-1].year + 1):
        p = idx.searchsorted(pd.Timestamp(f"{y}-11-29"))
        if p < len(idx) and str(idx[p].date()) > last:
            rd = idx[p]
            from rl.collect_kr_cap import marcap
            m = marcap([rd.year - 1, rd.year])
            cap = m.pivot_table(index="Date", columns="Code", values="Marcap", aggfunc="last").sort_index()
            cap = cap[cap.index < rd]
            top = list(cap.iloc[-20:].mean().nlargest(TOP_N).index)
            inside = [c for c in top if c in close.columns]
            print(f"새 순위일 {rd.date()}: 시총 상위 {TOP_N} 중 학습 집합 안 {len(inside)}종목(밖 {TOP_N - len(inside)} -- 재학습 필요)")
            known[str(rd.date())] = inside
            extra = {k: v for k, v in known.items() if k not in tops}
            json.dump(extra, open(EXTRA_TOPS, "w", encoding="utf-8"))
    elig = pd.DataFrame(False, index=idx, columns=close.columns)
    keys = sorted(known)
    for i, k in enumerate(keys):
        nxt = pd.Timestamp(keys[i + 1]) if i + 1 < len(keys) else idx[-1] + pd.Timedelta(days=1)
        cols = [c for c in known[k] if c in elig.columns]
        elig.loc[(idx >= pd.Timestamp(k)) & (idx < nxt), cols] = True
    return elig


def fetch(codes, start):
    import FinanceDataReader as fdr

    def one(c):
        for _ in range(3):
            try:
                d = fdr.DataReader(c, start)
                return c, d[["Close", "Volume"]] if len(d) else None
            except Exception:
                time.sleep(1)
        return c, None
    with ThreadPoolExecutor(8) as ex:
        got = dict(ex.map(one, codes))
    ok = {c: d for c, d in got.items() if d is not None}
    close = pd.DataFrame({c: d["Close"] for c, d in ok.items()}).sort_index()
    vol = pd.DataFrame({c: d["Volume"] for c, d in ok.items()}).reindex(close.index)
    ks = fdr.DataReader("KS11", start)["Close"]
    return close, vol, ks, [c for c in codes if c not in ok]


def targets(holdings_path, out_path):
    from stable_baselines3 import PPO
    from rl.collect_kr import fix_unadjusted
    from rl.features import from_frames

    uni = json.load(open(UNIVERSE, encoding="utf-8"))
    tickers, live = uni["tickers"], [c for c in uni["tickers"] if c not in set(uni["delisted"])]
    today = date.today()
    close, vol, ks, bad = fetch(live, str(today - timedelta(days=365 * YEARS)))
    close, vol, ks = close[close.index < pd.Timestamp(today)], vol[vol.index < pd.Timestamp(today)], ks[ks.index < pd.Timestamp(today)]
    close, vol = close.where(close > 0).astype(float), vol.astype(float)
    close, vol, _ = fix_unadjusted(close, vol, set())
    close, vol = close.reindex(columns=tickers), vol.reindex(columns=tickers)
    rv = ks.pct_change().rolling(20).std() * np.sqrt(252) * 100
    mk = pd.DataFrame({"spy": ks, "vix": rv}).dropna()
    d = from_frames(close, vol, eligibility(close, uni["tops"]), mk, ext=True)
    t = len(d.dates) - 1
    asof = pd.Timestamp(d.dates[t]).date()
    elig = d.elig[t] & ~np.isnan(close.values[t])

    h = json.load(open(holdings_path, encoding="utf-8"))
    vals = np.array([float(h["values"].get(c, 0.0)) for c in tickers])
    equity = float(h["cash"]) + float(sum(h["values"].values()))
    w = np.append(vals, float(h["cash"]) + sum(v for c, v in h["values"].items() if c not in tickers)) / max(equity, 1.0)
    feats = d.feats[t - WINDOW + 1:t + 1] * d.elig[t - WINDOW + 1:t + 1][:, :, None]
    obs = np.concatenate([feats.ravel(), w, d.elig[t]]).astype(np.float32)

    paths = sorted(glob.glob(MODELS))
    if not paths:
        raise SystemExit(f"모델 없음: {MODELS}")
    acts = [PPO.load(p, device="cpu").predict(obs, deterministic=True)[0] for p in paths]
    a = np.zeros(len(tickers) + 1); a[-1] = -2.0
    a = a + ANCHOR * np.mean(acts, axis=0)
    a[:-1][~elig] = -1e9
    e = np.exp(a - a.max()); tw = e / e.sum()

    res = {"generated": str(today), "asof": str(asof), "models": [os.path.basename(p) for p in paths],
           "eligible": int(elig.sum()), "fetch_failed": bad, "cash_weight": round(float(tw[-1]), 6),
           "weights": {c: round(float(x), 6) for c, x in zip(tickers, tw[:-1]) if x > 1e-4}}
    json.dump(res, open(out_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"targets asof={asof} eligible={res['eligible']} names={len(res['weights'])} cash={tw[-1]:.3f} "
          f"failed={len(bad)} models={len(paths)}")


def send(path):
    import requests
    msg = open(path, encoding="utf-8").read().strip()
    if not msg:
        return

    def up():
        try:
            return requests.get(BRIDGE + "/health", timeout=3).ok
        except Exception:
            return False
    if not up() and os.path.isdir(BRIDGE_DIR):
        py = os.path.join(BRIDGE_DIR, ".venv", "Scripts", "python.exe")
        subprocess.Popen([py, "-m", "uvicorn", "app.main:app", "--port", "8765", "--host", "127.0.0.1"],
                         cwd=BRIDGE_DIR, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(15):
            time.sleep(2)
            if up():
                break
    try:
        r = requests.post(BRIDGE + "/send", json={"message": msg}, timeout=120)
        print("kakao", r.status_code)
    except Exception as e:
        print("kakao 실패(무시):", e)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["targets", "send"])
    ap.add_argument("--holdings", default="rl_holdings.json")
    ap.add_argument("--out", default="rl_targets.json")
    ap.add_argument("--file", default="rl_message.txt")
    a = ap.parse_args()
    targets(a.holdings, a.out) if a.cmd == "targets" else send(a.file)
