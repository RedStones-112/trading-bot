# trading-bot (draft)

한국투자증권(KIS) Developers API로 붙는 최소 트레이딩 봇 초안. 특정 종목 하나를 보는 게
아니라, KIS 거래량순위 API로 후보 종목 리스트를 받아와서 그중 SMA 골든크로스가 가장
강하게 나타난 종목 하나를 골라 투자함. WinHTTP(Windows 내장)로 HTTPS 처리해서 libcurl 등
외부 의존성 없음. JSON은 nlohmann/json 헤더 하나만 vendor.

## 실행 모드 (`config.json`의 `mode`)

| mode | 시세/종목리스트 | 주문 | 필요한 것 |
|---|---|---|---|
| `mock` (기본) | 로컬 가상 종목 5개, 랜덤워크로 합성 | 로컬 가상 체결 | 없음 — 네트워크/계좌 전부 불필요 |
| `sim` | KIS 실계좌 앱키로 실시간 거래량순위+시세 | 로컬 가상 체결 (KIS에 주문 안 보냄) | 실계좌 App Key만 (모의투자 신청 불필요) |
| `paper` | KIS 모의투자 서버 | KIS 모의투자 주문 | 모의투자 신청 완료 + 모의투자 계좌 |
| `live` | KIS 실서버 | **진짜 돈** 주문 | 실계좌, 신중하게 |

`sim`이 가장 현실적인 테스트 방법: 실제 거래량순위/시세로 전략을 검증하면서도 KIS의
모의투자 신청 절차나 가상계좌 설정을 거칠 필요가 없음 — 주문 자체를 로컬에서만 가상
체결(`sim_fills.log`에 기록)하고 KIS 주문 API는 아예 호출하지 않음.

## 동작

1. 보유 중인 종목이 없으면: KIS 거래량순위 API로 상위 `watchlist_size`개 후보를 받아옴
   (레버리지/인버스 ETF·ETN은 자동 제외 — 시간가치 감소가 있고 변동성도 커서 단순
   SMA 크로스 전략에 안 맞음).
2. 후보 각각의 하루봉 종가로 SMA(short/long)를 계산해서 골든크로스가 뜨는 종목을 찾고,
   그중 크로스 강도(`(단기SMA-장기SMA)/장기SMA`)가 가장 큰 종목 하나를 시장가로 매수.
3. 매수 후에는 그 종목만 계속 모니터링하다가 데드크로스가 뜨면 시장가로 매도하고,
   다시 1번으로 돌아가 새 후보를 스캔.

`poll_seconds`마다 반복. 모든 요청/주문은 콘솔과 `trading.log`에 기록.

## 준비

- `mock`: 아무 준비 없이 바로 실행 가능.
- `sim`: https://apiportal.koreainvestment.com 가입 → 앱 등록 → App Key/App Secret 발급
  (실계좌 연결, 모의투자 신청은 불필요).
- `paper`/`live`: 위 App Key/Secret에 더해 계좌번호(`cano`, `acnt_prdt_cd`) 필요.
  `paper`는 모의투자 신청까지 완료해야 함.

`config.example.json`을 `config.json`으로 복사해서 값 채우기. `config.json`은
`.gitignore`에 이미 등록되어 있음.

## 빌드 (MSYS2 UCRT64 g++ 또는 MSVC)

```
cmake -S . -B build
cmake --build build
```

`build/` 안에 `config.json`을 두고 실행:

```
cd build
./trading_bot.exe
```

## 테스트

전략 로직(SMA 크로스/모멘텀)과 `MockBroker`를 결정론적으로 검증하는 self-check:

```
cmake --build build --target test_strategy
./build/test_strategy.exe
```

## 실시간 확인

콘솔(및 `trading.log`)에 스캔마다 후보 종목 각각의 `종목명(코드) 현재가=... 시그널=...`가
찍히고, 매수/매도가 체결되면 `>>> 매수 체결 ...` / `<<< 매도 체결 ... 손익 ...` 줄이
추가로 찍힘. 콘솔 한글이 깨지면 터미널 자체가 아니라 프로그램이
`SetConsoleOutputCP(CP_UTF8)`로 맞춰두니, 그래도 깨지면 터미널 폰트/코드페이지 문제일
가능성이 높음.

## 검증 필요

- 실제 KIS 모의투자 서버로 인증·거래량순위·시세 조회·매수/매도 체결까지 확인 완료
  (2026-07-22).
- KIS는 초당 호출 횟수 제한이 빡빡함(모의투자 계좌에서 `EGW00201` 레이트리밋을 실제로
  겪음). 그래서 API 호출 사이(후보 하나당 일봉조회+현재가조회 2번)에 1.1초 간격을 둠 —
  `watchlist_size`가 클수록 스캔 한 번에 걸리는 시간이 선형으로 늘어남
  (대략 `watchlist_size × 2.2초`). mock 모드는 실제 API가 아니라서 이 간격이 없음.
- 매수 후 모니터링용 하루봉 히스토리는 매수 시점에 한 번만 캐시함 — 포지션을 자정
  넘어 여러 날 들고 있으면 히스토리가 하루치 갱신이 안 되는 게 알려진 한계.
- ETF/ETN 필터는 이름에 `KODEX`/`TIGER`/`레버리지`/`인버스` 등 문자열이 포함되는지로
  판단하는 간단한 휴리스틱 — 모든 상품군을 완벽히 걸러내진 못함.
- tr_id, 필드명 등은 문서 기준으로 작성했지만 API가 종종 바뀌므로 실제 응답으로
  재확인할 것: https://apiportal.koreainvestment.com
- 장 운영시간(09:00~15:30) 체크, 잔고 조회, 체결 확인 로직은 아직 없음 —
  `sim`/`paper` 모드로 먼저 충분히 굴려보고 필요한 부분만 추가.
- `mode: "live"`로 바꾸면 진짜 돈으로 주문이 나감. 절대 검증 없이 바로 바꾸지 말 것.
