# 라즈베리파이 모의투자 운영 -- 이어서 하기 문서

> 상태(2026-11-24 기준): **파이에는 아직 아무것도 설치 안 함.** PC에서 실행기 코드/설치 스크립트까지 준비 완료,
> 첫 실제 주문은 사용자 지시로 보류 중. 파이를 가져오면 아래 "설치 순서"부터 진행.

## 무엇을 돌리나

- 모델: `exp8r` -- 2006-11~2016-11로 학습한 종목공유 PPO(상위 300 유니버스, 앵커 0.3), 시드 3개 평균.
  PyTorch 없이 numpy로 추론(`rl/live/model/exp8r.npz`, torch 결과와 최대 차이 6e-7 확인).
- 매 평일 23:40 KST(미국 동부 09:40, 서머타임 10:40 -- 둘 다 정규장 중)에 1회:
  1. NYSE 휴장일/장외시간이면 아무것도 안 함 (`rl/live/nyse_calendar.py`, 규칙 기반이라 연도 갱신 불필요)
  2. yfinance로 464종목 + SPY/VIX 3년치 -> 학습과 같은 특성/순위권 계산. 최신 종가가 직전 거래일이 아니면 중단
  3. KIS 모의계좌: 전날 미체결 취소 -> 잔고/주문가능금액
  4. 목표비중 -> 정수 주식수 -> 매도 먼저, 매수 나중 (지정가 = 전일 종가 ±1%)
  5. 결과 JSON `rl/live/state/logs/` (60일 보관), 텔레그램 알림(설정 시)
- 핵심 코드: `rl/live/runner.py` (안드로이드 앱과 공유), CLI: `rl/live/paper_trade.py`

**성과 기대치(중요)**: exp8r은 2016~2026 시험에서 동일비중 매일 리밸런스보다 낮았음(평균 273% vs 305%,
비용 0이면 거의 같음 -- 알파 ~0, 초과 매매 비용만큼 열위). 이 모의투자는 실전 파이프라인 검증 목적.

## 준비물

- **Raspberry Pi 4 또는 5, RAM 2GB 이상, 64비트 Raspberry Pi OS** (32비트/Pi 3은 pandas 설치가 느리거나 실패 가능)
- 상시 전원 + 유선 LAN 권장, SD카드 16GB 이상
- PC에서 가져갈 것: **`config.json`의 모의투자 키 3개만** (appkey, appsecret, cano). 저장소에는 키가 없음
  - 현재 모의계좌: 해외주식 포함 상시모의투자, 계좌 `5022****`(전체 번호는 PC의 config.json), USD $100,000 / KRW 1,000만
  - 이 키/계좌는 PC의 국내 C++ 봇(`trading_bot.exe`)도 같이 씀 -- 파이로 옮겨도 공유는 문제없음(해외/국내 잔고 분리)

## 설치 순서

```bash
# 1) 저장소 (public) -- PC에서 rl/live 작업분을 commit/push 했는지 먼저 확인!
git clone https://github.com/RedStones-112/trading-bot.git ~/trading-bot
cd ~/trading-bot

# 2) 설치 (시간대 Asia/Seoul 설정, venv, 패키지)
bash deploy/raspberrypi/setup.sh

# 3) 키 입력 (모의투자 appkey/appsecret/cano, 텔레그램은 선택)
nano config.json

# 4) 점검: 조회만 + 드라이런(주문 없음)
bash deploy/raspberrypi/check.sh
#   기대: OK 국내 잔고 / OK 해외 잔고 / OK 해외 주문가능금액, 드라이런 계획 출력

# 5) (선택) 옵션 -- 예: 상위 60종목만 매수(정수주 문제 완화, 아래 "결정 대기" 참고)
echo "--max-names 60" > deploy/raspberrypi/extra_args

# 6) 매일 자동 실행 등록
bash deploy/raspberrypi/install_cron.sh
```

첫 실제 주문을 수동으로 확인하고 싶으면 23:30 KST 이후에:
`.venv-live/bin/python -m rl.live.paper_trade --execute` 실행 후 `rl/live/state/logs/` 최신 JSON의 `orders[].odno`
(주문번호) / `error` 확인.

## 결정 대기 (사용자)

1. **종목 수**: $10만을 300종목에 나누면 종목당 ~$330이라 고가주를 1주도 못 사서 매수 211건/$6만만 투자됨
   (2026-11-24 드라이런). 선택지: 그대로 / `--max-names 60`(종목당 ~$1,650, 95%+ 투자) / 다른 N.
2. **첫 주문 시작 시점**: 사용자가 "지금 당장 시작하지 말 것"이라고 함 -- 파이 설치 후 사용자 확인 받고 cron 등록.
3. **알림 수단**: 텔레그램(구현됨) / 카카오톡(PC kakao_bridge는 PC가 꺼지면 못 씀 -- 파이에서 카카오 공식 API
   직접 호출은 미구현).

## 확인된 것 / 안 된 것 (2026-11-24, PC에서)

- 확인: 토큰 발급, 해외 현재가/일봉(수정주가)/VIX 지수일봉, 국내·해외 잔고, 주문가능금액, 미체결 조회(VTTS3018R),
  드라이런 전체(데이터->특성->정책->계획), numpy 정책 == torch 정책.
- **미확인: 실제 주문(VTTT1002U/1001U)과 취소(VTTT1004U)의 응답** -- 첫 `--execute`에서 로그로 확인할 것.
  모의투자는 미국 시장가 미지원이라 지정가(00)만 사용.
- 체결내역 조회(VTTS3001R)는 모의투자에서 "없는 서비스 코드" -- 사용 안 함.
- 클래스주 `BRK-B`는 KIS 티커 표기 미확인이라 주문 제외(그 비중은 현금).

## 문제 해결

| 증상 | 원인/조치 |
|---|---|
| `41100000 모의투자 신청내역을 확인하세요` | 모의투자 기간 만료. 홈페이지 트레이딩>모의투자>주식/선물옵션 모의투자>모의투자안내>신청/재도전에서 **"국내주식+해외주식"** 리그로 재신청 -> KIS Developers에서 옛 모의 API 삭제 후 새 계좌로 API 재신청 -> 새 appkey/appsecret/cano를 config.json에 (2026-11-24에 실제로 겪음) |
| `INVALID_CHECK_ACNO` | appkey가 그 계좌에 연결 안 됨 -> KIS Developers에서 해당 모의계좌로 API 신청한 키인지 확인 |
| `EGW00201 초당 거래건수 초과` | 자동 재시도함(호출 간격 1.1초). PC의 국내 봇과 같은 키를 동시에 쓰면 더 잘 남 |
| `데이터 지연으로 중단` | yfinance가 아직 전일 종가를 안 줌 -> 다음날 자동 재시도, 반복되면 `pip install -U yfinance` |
| cron이 안 돈 것 같음 | `rl/live/state/cron.log`, `timedatectl`(Asia/Seoul인지), `crontab -l` |

## 관련 파일

- `rl/live/runner.py` 핵심 로직 · `paper_trade.py` CLI · `kis_overseas.py` KIS 모의 클라이언트 ·
  `market_data.py` 데이터(yfinance/KIS 캐시) · `nyse_calendar.py` 휴장일 · `np_policy.py` numpy 정책(+ `export`/`verify`) ·
  `check_account.py` 연결 점검 · `model/exp8r.npz` 가중치 · `universe464.json` 모델 종목 순서
- 학습/연구 경위: 저장소 루트 `PROGRESS.md`의 "미국 상위 100종목 강화학습 연구 착수 (2026-11-24)" 섹션
- 안드로이드 앱 버전: `mobile/android/README.md`
