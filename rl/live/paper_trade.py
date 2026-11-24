"""exp8r 모의투자 일일 리밸런스 CLI (PC/라즈베리파이). 핵심 로직은 rl/live/runner.py (안드로이드 앱과 공유).

    python -m rl.live.paper_trade                      # 드라이런: 계획만 (KIS는 조회만)
    python -m rl.live.paper_trade --execute            # 모의계좌에 지정가 주문 (정규장 시간에만)
    python -m rl.live.paper_trade --sim-cash 100000    # KIS 없이 빈 계좌 가정 점검
    옵션: --backend yfinance|kis, --max-names N (상위 N종목만), --force (휴장/시간/데이터지연 검사 무시, 점검용)

설정: 저장소 config.json(mode는 반드시 "paper"). 알림(선택): config.json에 "telegram_bot_token", "telegram_chat_id"를
넣으면 실행 결과/오류를 텔레그램으로 보냄. 실패해도 매매에는 영향 없음."""
import argparse, os, sys, traceback

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
os.chdir(ROOT)

from rl.live.kis_overseas import load_config
from rl.live.runner import run


def notify(cfg, text):
    tok, chat = cfg.get("telegram_bot_token"), cfg.get("telegram_chat_id")
    if not (tok and chat):
        return
    try:
        import requests
        requests.post(f"https://api.telegram.org/bot{tok}/sendMessage", timeout=15,
                      json={"chat_id": chat, "text": text[:3500]})
    except Exception as e:
        print("텔레그램 알림 실패:", e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--execute", action="store_true")
    ap.add_argument("--backend", choices=["yfinance", "kis"], default="yfinance")
    ap.add_argument("--max-names", type=int, default=0)
    ap.add_argument("--sim-cash", type=float, default=0.0)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--config", default=None, help="config.json 경로(기본: 저장소 루트)")
    a = ap.parse_args()
    if a.sim_cash and a.execute:
        raise SystemExit("--sim-cash와 --execute는 함께 쓸 수 없음")
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    cfg = load_config(a.config)
    try:
        r = run(cfg, execute=a.execute, backend=a.backend, options={"max_names": a.max_names},
                force=a.force, sim_cash=a.sim_cash, log=lambda m: print(m, flush=True))
        if a.execute or r["status"] not in ("dry_run",):
            notify(cfg, r["summary"])
    except Exception:
        msg = "[모의투자 오류]\n" + traceback.format_exc()[-1500:]
        print(msg)
        notify(cfg, msg)
        sys.exit(1)


if __name__ == "__main__":
    main()
