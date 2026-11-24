"""exp8r 모의투자 일일 리밸런스 핵심 로직 -- PC/라즈베리파이(paper_trade.py CLI)와 안드로이드 앱(Chaquopy)이 공유.
PyTorch/pyarrow 불필요(numpy 정책 rl/live/model/exp8r.npz, 메모리 DataFrame).

run(cfg, execute, state_dir, ...) 한 번 = 하루치:
 0) 거래일/장중 확인(NYSE 규칙 캘린더) -- 휴장이면 아무것도 안 함
 1) 데이터: yfinance(파이) 또는 KIS 캐시(안드로이드) -> 학습과 같은 특성/순위권(매년 11/29 이후 첫 거래일, 직전
    252일 평균 거래대금 상위 300, 학습 때 464종목 안에서) -- 마지막 '완성된' 종가가 직전 거래일이 아니면 중단
 2) KIS: (execute면) 남은 미체결 취소 -> 잔고/주문가능금액
 3) 정책: 시드 3개 평균 결정적 행동 -> 앵커 0.3 softmax 목표비중 (선택: 상위 max_names 종목만 남기고 재정규화)
 4) 정수 주식수 계획(작은 조정 생략) -> 매도 먼저, 매수 나중(지정가 = 전일종가 ± buffer)
 5) 로그 JSON 저장(60일 지난 로그 삭제), 요약 문자열 반환(알림용)"""
import io, json, os, time
from datetime import datetime, timedelta
import numpy as np
import pandas as pd
import requests

from rl.features import from_frames
from rl.live import nyse_calendar as cal
from rl.live.kis_overseas import KisOverseasPaper
from rl.live.market_data import fetch_kis, fetch_yfinance
from rl.live.np_policy import NpPolicy

HERE = os.path.dirname(os.path.abspath(__file__))
UNIVERSE = os.path.join(HERE, "universe464.json")
WINDOW, ANCHOR, TOP_N = 10, 0.3, 300
DEFAULTS = dict(min_trade_usd=200.0, min_trade_frac=0.25, buffer=0.01, cash_reserve=0.02, max_names=0, years=3)


def eligibility(close, vol):
    """학습 데이터(rl/collect_pit.py)와 같은 규칙: 매년 11/29 이후 첫 거래일에 직전 252일 평균 거래대금 상위 300."""
    dv = close * vol
    idx = close.index
    rds = []
    for y in range(idx[0].year, idx[-1].year + 1):
        pos = idx.searchsorted(pd.Timestamp(f"{y}-11-29"))
        if pos < len(idx) and pos >= 252:
            rds.append(idx[pos])
    elig = pd.DataFrame(False, index=idx, columns=close.columns)
    for i, rd in enumerate(rds):
        pos = idx.get_loc(rd)
        score = dv.iloc[pos - 252:pos].mean()
        top = score[close.iloc[pos - 252:pos].notna().sum() >= 200].nlargest(TOP_N).index
        nxt = rds[i + 1] if i + 1 < len(rds) else idx[-1] + pd.Timedelta(days=1)
        elig.loc[(idx >= rd) & (idx < nxt), top] = True
    return elig


def exchange_map(state_dir):
    """nasdaqtrader 심볼 디렉터리 -> {티커: 주문용 거래소코드}, 7일 캐시."""
    p = os.path.join(state_dir, "exchange_map.json")
    if os.path.exists(p) and time.time() - os.path.getmtime(p) < 7 * 86400:
        return json.load(open(p))
    h = {"User-Agent": "Mozilla/5.0"}
    m = {}
    a = pd.read_csv(io.StringIO(requests.get("https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt",
                                             headers=h, timeout=30).text), sep="|")
    for s in a["Symbol"].dropna(): m[str(s)] = "NASD"
    b = pd.read_csv(io.StringIO(requests.get("https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt",
                                             headers=h, timeout=30).text), sep="|")
    code = {"N": "NYSE", "A": "AMEX", "P": "AMEX"}  # NYSE Arca(P)는 KIS에서 AMEX 코드로 주문(SPY 시세 AMS로 확인)
    for s, e in zip(b["ACT Symbol"], b["Exchange"]):
        if e in code: m[str(s)] = code[e]
    json.dump(m, open(p, "w"))
    return m


def target_weights(action, elig, max_names=0):
    N = len(elig)
    a = np.asarray(action, np.float64)
    base = np.zeros(N + 1); base[-1] = -2.0
    a = base + ANCHOR * a
    a[:-1][~elig] = -1e9
    e = np.exp(a - a.max()); w = e / e.sum()
    if max_names and max_names < int(elig.sum()):
        keep = np.argsort(-w[:-1])[:max_names]
        s = np.zeros(N); s[keep] = w[:-1][keep]
        w = np.append(s / s.sum() * w[:-1].sum(), w[-1])
    return w


def run(cfg, execute=False, state_dir=None, backend="yfinance", options=None, log=print, progress=None,
        force=False, sim_cash=0.0):
    o = {**DEFAULTS, **(options or {})}
    state_dir = state_dir or os.path.join(HERE, "state")
    logs = os.path.join(state_dir, "logs"); os.makedirs(logs, exist_ok=True)
    now = datetime.now()
    rec = {"run_at": now.isoformat(timespec="seconds"), "execute": execute, "backend": backend, "options": o}

    def finish(summary, status):
        rec.update(status=status, summary=summary)
        json.dump(rec, open(os.path.join(logs, f"{now:%Y-%m-%d_%H%M%S}.json"), "w"), indent=1, default=str)
        for f in os.listdir(logs):  # 60일 지난 로그 정리
            fp = os.path.join(logs, f)
            if time.time() - os.path.getmtime(fp) > 60 * 86400:
                os.remove(fp)
        log(summary)
        return {"status": status, "summary": summary, "orders": rec.get("orders", [])}

    today, in_session, last_complete = cal.session_status()
    rec["et_date"], rec["last_complete"] = str(today), str(last_complete)
    if not force and not cal.is_trading_day(today):
        return finish(f"[모의투자] {today} 미국 휴장일 -- 실행 안 함", "holiday")
    if execute and not in_session and not force:
        return finish(f"[모의투자] 정규장 시간이 아님(ET {cal.now_et():%H:%M}) -- 주문 안 함", "closed")

    tickers = json.load(open(UNIVERSE))
    exmap = exchange_map(state_dir)
    kis = None if sim_cash > 0 else KisOverseasPaper(cfg, state_dir)
    log(f"[1] 데이터 갱신({backend}): {len(tickers)}종목, 기준 종가일 {last_complete}")
    if backend == "kis":
        close, vol, mk, bad = fetch_kis(kis or KisOverseasPaper(cfg, state_dir), tickers, exmap, last_complete,
                                        state_dir, o["years"], log, progress)
    else:
        close, vol, mk, bad = fetch_yfinance(tickers, last_complete, o["years"])
    asof = close.index[-1].date()
    rec["asof_close"], rec["fetch_failed"] = str(asof), bad
    if asof != last_complete and not force:
        return finish(f"[모의투자] 데이터 최신 종가 {asof} != 기대 {last_complete} -- 데이터 지연으로 중단", "stale")

    elig_df = eligibility(close, vol)
    d = from_frames(close, vol, elig_df, mk, ext=True)
    t = len(d.dates) - 1
    elig = d.elig[t] & ~np.isnan(close.values[t])
    price = close.ffill().values[t]
    tradable = np.array([("-" not in s) and (s in exmap) for s in tickers])
    log(f"    순위권 {int(elig.sum())}종목, 주문가능 {int((elig & tradable).sum())}, 수집실패 {len(bad)}")

    if execute:
        left = kis.open_orders()
        for oo in left:
            try:
                kis.cancel(oo)
            except Exception as e:
                log(f"    미체결 취소 실패 {oo['pdno']}: {e}")
        rec["cancelled"] = left
        if left: log(f"[2] 전날 미체결 {len(left)}건 취소")
    if kis is None:
        hold, cash = {}, sim_cash
    else:
        hold, cash = kis.balance(), kis.buying_power_usd()
    qty = np.array([hold.get(s, (0, ""))[0] for s in tickers], float)
    pos_val = qty * np.nan_to_num(price)
    equity = cash + pos_val.sum()
    rec.update(cash=cash, equity=equity, holdings={k: v[0] for k, v in hold.items()},
               outside_universe=[k for k in hold if k not in tickers])
    log(f"[2] 현금 ${cash:,.0f}, 보유 {int((qty > 0).sum())}종목 ${pos_val.sum():,.0f}, 총 ${equity:,.0f}")
    if equity <= 0:
        return finish("[모의투자] 평가금액 0 -- 계좌 USD 잔고 확인 필요", "no_equity")

    w = np.append(pos_val / equity, cash / equity).astype(np.float32)
    feats = d.feats[t - WINDOW + 1:t + 1] * d.elig[t - WINDOW + 1:t + 1][:, :, None]
    obs = np.concatenate([feats.ravel(), w, d.elig[t].astype(np.float32)]).astype(np.float32)
    tw = target_weights(NpPolicy().act(obs), elig, int(o["max_names"]))
    tw[:-1][~tradable] = 0.0
    rec["target_top"] = {tickers[i]: round(float(tw[i]), 4) for i in np.argsort(-tw[:-1])[:10]}

    tgt_q = np.floor(tw[:-1] * equity * (1 - o["cash_reserve"]) / np.where(price > 0, price, np.inf))
    plan = []
    for i, s in enumerate(tickers):
        dq = int(tgt_q[i] - qty[i])
        if dq == 0 or not np.isfinite(price[i]) or not tradable[i]:
            continue
        dv = abs(dq) * price[i]
        exit_all = tgt_q[i] == 0 and qty[i] > 0
        if not exit_all and (dv < o["min_trade_usd"] or dv < o["min_trade_frac"] * max(tgt_q[i], qty[i]) * price[i]):
            continue
        side = "sell" if dq < 0 else "buy"
        plan.append({"ticker": s, "ex": exmap[s], "side": side, "qty": abs(dq), "ref_close": round(float(price[i]), 2),
                     "limit": round(float(price[i] * (1 - o["buffer"] if side == "sell" else 1 + o["buffer"])), 2)})
    sells = sum(p["qty"] * p["ref_close"] for p in plan if p["side"] == "sell")
    buys = sum(p["qty"] * p["limit"] for p in plan if p["side"] == "buy")
    if buys > cash + sells * 0.98:
        sc = (cash + sells * 0.98) / buys
        for p in plan:
            if p["side"] == "buy": p["qty"] = int(p["qty"] * sc)
        plan = [p for p in plan if p["qty"] > 0]
        buys = sum(p["qty"] * p["limit"] for p in plan if p["side"] == "buy")
    rec["orders"] = plan
    n_s, n_b = sum(p["side"] == "sell" for p in plan), sum(p["side"] == "buy" for p in plan)
    log(f"[3] 계획: 매도 {n_s}건 ${sells:,.0f}, 매수 {n_b}건 ${buys:,.0f} (목표 주식비중 {tw[:-1].sum():.0%})")

    if not execute:
        return finish(f"[모의투자 드라이런] {asof} 종가 기준, 총 ${equity:,.0f}, 계획 매도 {n_s}/매수 {n_b}건", "dry_run")
    fails = 0
    for p in sorted(plan, key=lambda p: p["side"] != "sell"):
        try:
            p["odno"] = kis.order(p["side"], p["ticker"], p["ex"], p["qty"], p["limit"])
        except Exception as e:
            p["error"] = str(e); fails += 1
    status = "ok" if fails == 0 else "partial"
    return finish(f"[모의투자] {asof} 종가 기준 주문 {len(plan) - fails}/{len(plan)}건 전송 "
                  f"(매도 {n_s}, 매수 {n_b}), 총평가 ${equity:,.0f}" + (f", 실패 {fails}건" if fails else ""), status)
