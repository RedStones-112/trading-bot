"""모의투자 계좌/API 연결 점검(조회만, 주문 없음). python -m rl.live.check_account
config.json(또는 cfg dict)의 키/계좌번호로 토큰 재발급 -> 국내 잔고, 해외 잔고, 해외 주문가능금액 조회."""
import os, sys
from rl.live.kis_overseas import KisOverseasPaper, STATE


def check(cfg=None, state_dir=None):
    """반환: (전부 정상 여부, 결과 줄 목록)."""
    sd = state_dir or STATE  # 토큰 캐시는 appkey별로 구분되므로 키를 바꾸면 자동으로 새로 발급됨
    k = KisOverseasPaper(cfg, sd)
    lines = [f"계좌 종류: {'주식 모의계좌(5로 시작)' if k.cano.startswith('5') else '주식 모의계좌 아님 -- 번호 확인 필요'}"]
    tests = [
        ("국내 잔고", "/uapi/domestic-stock/v1/trading/inquire-balance", "VTTC8434R",
         {"CANO": k.cano, "ACNT_PRDT_CD": k.prdt, "AFHR_FLPR_YN": "N", "OFL_YN": "", "INQR_DVSN": "02",
          "UNPR_DVSN": "01", "FUND_STTL_ICLD_YN": "N", "FNCG_AMT_AUTO_RDPT_YN": "N", "PRCS_DVSN": "00",
          "CTX_AREA_FK100": "", "CTX_AREA_NK100": ""},
         lambda j: f"예수금 {(j.get('output2') or [{}])[0].get('dnca_tot_amt')}원"),
        ("해외 잔고", "/uapi/overseas-stock/v1/trading/inquire-balance", "VTTS3012R",
         {"CANO": k.cano, "ACNT_PRDT_CD": k.prdt, "OVRS_EXCG_CD": "NASD", "TR_CRCY_CD": "USD",
          "CTX_AREA_FK200": "", "CTX_AREA_NK200": ""},
         lambda j: f"보유 {len(j.get('output1', []))}종목(NASD)"),
        ("해외 주문가능금액", "/uapi/overseas-stock/v1/trading/inquire-psamount", "VTTS3007R",
         {"CANO": k.cano, "ACNT_PRDT_CD": k.prdt, "OVRS_EXCG_CD": "NASD", "OVRS_ORD_UNPR": "100", "ITEM_CD": "AAPL"},
         lambda j: f"주문가능 ${j.get('output', {}).get('ord_psbl_frcr_amt')}"),
    ]
    ok = 0
    for name, path, tr, params, show in tests:
        try:
            j, _ = k._call("GET", path, tr, params)
            lines.append(f"OK   {name}: {show(j)}"); ok += 1
        except Exception as e:
            lines.append(f"FAIL {name}: {e}")
    good = ok == len(tests)
    lines.append("모두 정상" if good else
                 "실패 있음 -- 모의투자 참가(국내+해외) / KIS Developers API 재신청 / 키·계좌번호 확인")
    return good, lines


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    good, lines = check()
    print("\n".join(lines))
    sys.exit(0 if good else 1)
