"""KIS 모의투자 해외주식(미국) 최소 클라이언트 -- **모의투자 서버 전용**(실서버 주소/tr_id는 일부러 넣지 않음).

설정: cfg dict(appkey/appsecret/cano/acnt_prdt_cd, mode == "paper") -- 없으면 저장소 config.json(JSONC, 기존
C++ 봇과 공유)을 읽음. 키/계좌번호는 로그에 남기지 않음. 토큰은 state_dir/token.json 캐시(KIS 발급 1분 1회 제한).
모의투자 레이트리밋: 초당 건수 초과(EGW00201)가 확인돼 호출 간격 1.1초.
확인된 것(2026-11-24, 모의계좌): 토큰, 해외 현재가/일봉/지수일봉, 잔고, 주문가능금액, 미체결 조회 정상.
미확인: 주문/취소 실제 응답(첫 실행 때 로그 확인)."""
import json, os, re, time
from datetime import datetime, timedelta
import requests

HOST = "https://openapivts.koreainvestment.com:29443"  # 모의투자 전용
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATE = os.path.join(ROOT, "rl", "live", "state")
TR = {"price": "HHDFS00000300", "daily": "HHDFS76240000", "index_daily": "FHKST03030100",
      "balance": "VTTS3012R", "psamount": "VTTS3007R", "nccs": "VTTS3018R",
      "buy": "VTTT1002U", "sell": "VTTT1001U", "cancel": "VTTT1004U"}
ORD_EXCG = ("NASD", "NYSE", "AMEX")                          # 주문/잔고용
PRICE_EXCG = {"NASD": "NAS", "NYSE": "NYS", "AMEX": "AMS"}   # 시세용


def load_config(path=None):
    t = open(path or os.path.join(ROOT, "config.json"), encoding="utf-8").read()
    return json.loads(re.sub(r"(?m)^\s*//.*$", "", t))


class KisOverseasPaper:
    def __init__(self, cfg=None, state_dir=None, min_interval=1.1):
        c = cfg or load_config()
        if c.get("mode", "paper") != "paper":
            raise SystemExit("mode가 paper가 아님 -- 이 실행기는 모의투자 전용이라 중단")
        self.key, self.secret = c["appkey"], c["appsecret"]
        self.cano, self.prdt = str(c["cano"]), str(c.get("acnt_prdt_cd", "01"))
        self.state = state_dir or STATE
        self.min_interval, self._last = min_interval, 0.0
        self.token = self._token()

    def _token(self):
        os.makedirs(self.state, exist_ok=True)
        p = os.path.join(self.state, "token.json")
        if os.path.exists(p):
            d = json.load(open(p))
            if d.get("appkey_tail") == self.key[-4:] and \
                    datetime.fromisoformat(d["expires"]) > datetime.now() + timedelta(minutes=10):
                return d["token"]
        for attempt in range(2):  # 발급 제한(1분 1회, HTTP 403 EGW00133)에 걸리면 1분 뒤 한 번 더
            r = requests.post(HOST + "/oauth2/tokenP", timeout=15, json={
                "grant_type": "client_credentials", "appkey": self.key, "appsecret": self.secret})
            if r.status_code == 403 and attempt == 0:
                time.sleep(65); continue
            break
        if r.status_code != 200:
            j = r.json() if r.content else {}
            raise RuntimeError(f"KIS 토큰 발급 실패: HTTP {r.status_code} {j.get('error_code')} {j.get('error_description')}")
        j = r.json()
        exp = datetime.now() + timedelta(seconds=int(j.get("expires_in", 86400)))
        json.dump({"token": j["access_token"], "expires": exp.isoformat(), "appkey_tail": self.key[-4:]}, open(p, "w"))
        return j["access_token"]

    def _call(self, method, path, tr_id, params=None, body=None, tr_cont=""):
        for attempt in range(3):
            wait = self.min_interval - (time.time() - self._last)
            if wait > 0: time.sleep(wait)
            h = {"content-type": "application/json; charset=utf-8", "authorization": f"Bearer {self.token}",
                 "appkey": self.key, "appsecret": self.secret, "tr_id": tr_id, "custtype": "P", "tr_cont": tr_cont}
            r = requests.request(method, HOST + path, headers=h, params=params, json=body, timeout=20)
            self._last = time.time()
            j = r.json() if r.content else {}
            if j.get("msg_cd") == "EGW00201" and attempt < 2:  # 초당 건수 초과 -> 잠시 후 재시도
                time.sleep(2); continue
            if r.status_code != 200 or j.get("rt_cd") not in ("0", None):
                raise RuntimeError(f"KIS {tr_id} 실패: HTTP {r.status_code} {j.get('msg_cd')} {j.get('msg1')}")
            return j, r.headers.get("tr_cont", "")

    def _paged(self, path, tr_id, params, list_key, fk="CTX_AREA_FK200", nk="CTX_AREA_NK200"):
        rows, cont = [], ""
        while True:
            j, cont = self._call("GET", path, tr_id, params, tr_cont=cont)
            rows += j.get(list_key, []) or []
            if cont not in ("F", "M"):
                return rows
            params = {**params, fk: j.get(fk.lower(), ""), nk: j.get(nk.lower(), "")}
            cont = "N"

    # ---- 계좌 ----
    def balance(self):
        """보유종목 {ticker: (qty, 거래소코드)}. 모의투자는 거래소별로 조회."""
        out = {}
        for ex in ORD_EXCG:
            for row in self._paged("/uapi/overseas-stock/v1/trading/inquire-balance", TR["balance"], {
                    "CANO": self.cano, "ACNT_PRDT_CD": self.prdt, "OVRS_EXCG_CD": ex, "TR_CRCY_CD": "USD",
                    "CTX_AREA_FK200": "", "CTX_AREA_NK200": ""}, "output1"):
                q = int(float(row.get("ovrs_cblc_qty", 0) or 0))
                if q > 0:
                    out[row["ovrs_pdno"]] = (q, row.get("ovrs_excg_cd", ex))
        return out

    def buying_power_usd(self, symbol="AAPL"):
        j, _ = self._call("GET", "/uapi/overseas-stock/v1/trading/inquire-psamount", TR["psamount"], {
            "CANO": self.cano, "ACNT_PRDT_CD": self.prdt, "OVRS_EXCG_CD": "NASD", "OVRS_ORD_UNPR": "1",
            "ITEM_CD": symbol})
        o = j.get("output", {})
        return float(o.get("ord_psbl_frcr_amt") or o.get("ovrs_ord_psbl_amt") or 0)

    def open_orders(self):
        """미체결 주문 목록 [{odno, pdno, ex, qty, side}]."""
        out = []
        for ex in ORD_EXCG:
            for r in self._paged("/uapi/overseas-stock/v1/trading/inquire-nccs", TR["nccs"], {
                    "CANO": self.cano, "ACNT_PRDT_CD": self.prdt, "OVRS_EXCG_CD": ex, "SORT_SQN": "DS",
                    "CTX_AREA_FK200": "", "CTX_AREA_NK200": ""}, "output"):
                q = int(float(r.get("nccs_qty", 0) or 0))
                if q > 0:
                    out.append({"odno": r.get("odno"), "pdno": r.get("pdno"), "ex": r.get("ovrs_excg_cd", ex),
                                "qty": q, "side": r.get("sll_buy_dvsn_cd")})
        return out

    def cancel(self, o):
        j, _ = self._call("POST", "/uapi/overseas-stock/v1/trading/order-rvsecncl", TR["cancel"], body={
            "CANO": self.cano, "ACNT_PRDT_CD": self.prdt, "OVRS_EXCG_CD": o["ex"], "PDNO": o["pdno"],
            "ORGN_ODNO": o["odno"], "RVSE_CNCL_DVSN_CD": "02", "ORD_QTY": str(o["qty"]), "OVRS_ORD_UNPR": "0",
            "MGCO_APTM_ODNO": "", "ORD_SVR_DVSN_CD": "0"})
        return j.get("msg1", "")

    def order(self, side, ticker, ex, qty, price):
        """지정가 주문(모의투자는 미국 시장가 미지원). 반환: 주문번호."""
        j, _ = self._call("POST", "/uapi/overseas-stock/v1/trading/order", TR[side], body={
            "CANO": self.cano, "ACNT_PRDT_CD": self.prdt, "OVRS_EXCG_CD": ex, "PDNO": ticker,
            "ORD_QTY": str(int(qty)), "OVRS_ORD_UNPR": f"{price:.2f}", "ORD_SVR_DVSN_CD": "0", "ORD_DVSN": "00"})
        return j.get("output", {}).get("ODNO", "")

    # ---- 시세 ----
    def daily(self, ex, sym, bymd=""):
        """수정주가 일봉 최대 100행 [(YYYYMMDD, close, volume)], bymd(포함) 이전으로. ex는 주문용 코드."""
        j, _ = self._call("GET", "/uapi/overseas-price/v1/quotations/dailyprice", TR["daily"], {
            "AUTH": "", "EXCD": PRICE_EXCG[ex], "SYMB": sym, "GUBN": "0", "BYMD": bymd, "MODP": "1"})
        return [(r["xymd"], float(r["clos"]), float(r.get("tvol") or 0)) for r in j.get("output2", []) if r.get("xymd")]

    def index_daily(self, code, d1, d2):
        """해외지수 일봉 [(YYYYMMDD, close)] -- 예: VIX, SPX."""
        j, _ = self._call("GET", "/uapi/overseas-price/v1/quotations/inquire-daily-chartprice", TR["index_daily"], {
            "FID_COND_MRKT_DIV_CODE": "N", "FID_INPUT_ISCD": code, "FID_INPUT_DATE_1": d1,
            "FID_INPUT_DATE_2": d2, "FID_PERIOD_DIV_CODE": "D"})
        return [(r["stck_bsop_date"], float(r["ovrs_nmix_prpr"])) for r in j.get("output2", [])
                if r.get("stck_bsop_date") and r.get("ovrs_nmix_prpr")]
