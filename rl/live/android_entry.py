"""안드로이드 앱(Chaquopy)에서 부르는 진입점. Kotlin -> Python 인자는 문자열/숫자만, 반환은 JSON 문자열.
데이터 백엔드는 KIS(yfinance의 curl_cffi가 Chaquopy에 없음). 진행 로그는 state_dir/progress.txt에 기록(앱이 표시)."""
import json, os, traceback
from datetime import datetime


def _logger(state_dir):
    os.makedirs(state_dir, exist_ok=True)
    p = os.path.join(state_dir, "progress.txt")
    open(p, "w", encoding="utf-8").close()

    def log(msg):
        with open(p, "a", encoding="utf-8") as f:
            f.write(f"{datetime.now():%H:%M:%S} {msg}\n")
    return log


def _cfg(cfg_json):
    c = json.loads(cfg_json)
    c["mode"] = "paper"  # 앱은 모의투자 전용
    return c


def check(cfg_json, state_dir):
    from rl.live.check_account import check as _check
    try:
        good, lines = _check(_cfg(cfg_json), state_dir)
        return json.dumps({"status": "ok" if good else "fail", "summary": "\n".join(lines)}, ensure_ascii=False)
    except Exception:
        return json.dumps({"status": "error", "summary": traceback.format_exc()[-1500:]}, ensure_ascii=False)


def prefetch(cfg_json, state_dir):
    """첫 실행 전 KIS 3년치 캐시 채우기(~1시간). 이후 일일 실행은 종목당 1회 호출."""
    log = _logger(state_dir)
    try:
        import json as _j
        from rl.live import nyse_calendar as cal
        from rl.live.kis_overseas import KisOverseasPaper
        from rl.live.market_data import fetch_kis
        from rl.live.runner import UNIVERSE, exchange_map
        _, _, lc = cal.session_status()
        kis = KisOverseasPaper(_cfg(cfg_json), state_dir)
        tickers = _j.load(open(UNIVERSE))
        close, _, mk, bad = fetch_kis(kis, tickers, exchange_map(state_dir), lc, state_dir, log=log,
                                      progress=lambda i, n: log(f"수집 {i}/{n}"))
        s = f"데이터 준비 완료: {close.shape[1] - len(bad)}종목, 최신 {close.index[-1].date()}, 실패 {len(bad)}"
        log(s)
        return json.dumps({"status": "ok", "summary": s}, ensure_ascii=False)
    except Exception:
        tb = traceback.format_exc()[-1500:]; log(tb)
        return json.dumps({"status": "error", "summary": tb}, ensure_ascii=False)


def run(cfg_json, execute, state_dir, max_names=0, force=False):
    log = _logger(state_dir)
    try:
        from rl.live.runner import run as _run
        r = _run(_cfg(cfg_json), execute=bool(execute), state_dir=state_dir, backend="kis",
                 options={"max_names": int(max_names)}, log=log, force=bool(force),
                 progress=lambda i, n: log(f"수집 {i}/{n}"))
        return json.dumps({"status": r["status"], "summary": r["summary"]}, ensure_ascii=False)
    except Exception:
        tb = traceback.format_exc()[-1500:]; log(tb)
        return json.dumps({"status": "error", "summary": "[모의투자 오류]\n" + tb}, ensure_ascii=False)
