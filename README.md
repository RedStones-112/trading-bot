# trading-bot (draft)

한국투자증권(KIS) Developers API로 붙는 최소 트레이딩 봇 초안. 단순 이동평균(SMA)
골든/데드 크로스 전략. WinHTTP(Windows 내장)로 HTTPS 처리해서 libcurl 등 외부
의존성 없음. JSON은 nlohmann/json 헤더 하나만 vendor.

## 실행 모드 (`config.json`의 `mode`)

| mode | 시세 | 주문 | 필요한 것 |
|---|---|---|---|
| `mock` (기본) | 로컬 랜덤워크로 합성 | 로컬 가상 체결 | 없음 — 네트워크/계좌 전부 불필요 |
| `sim` | KIS 실계좌 앱키로 실시간 시세 | 로컬 가상 체결 (KIS에 주문 안 보냄) | 실계좌 App Key만 (모의투자 신청 불필요) |
| `paper` | KIS 모의투자 서버 | KIS 모의투자 주문 | 모의투자 신청 완료 + 모의투자 계좌 |
| `live` | KIS 실서버 | **진짜 돈** 주문 | 실계좌, 신중하게 |

`sim`이 가장 현실적인 테스트 방법: 실제 시세로 전략을 검증하면서도 KIS의 모의투자
신청 절차나 가상계좌 설정을 거칠 필요가 없음 — 주문 자체를 로컬에서만 가상 체결
(`sim_fills.log`에 기록)하고 KIS 주문 API는 아예 호출하지 않음.

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

전략 로직(SMA 크로스)과 `MockBroker`를 결정론적으로 검증하는 self-check:

```
cmake --build build --target test_strategy
./build/test_strategy.exe
```

## 동작

`config.json`의 `code`(종목코드) 하루봉 종가로 SMA(short/long) 계산, 골든크로스면 시장가 매수,
데드크로스면 보유분 시장가 매도. `poll_seconds`마다 반복. 모든 요청/주문은 `trading.log`에 기록.

## 검증 필요

- KIS API의 tr_id, 필드명(`stck_prpr`, `stck_clpr` 등)은 문서 기준으로 작성했지만 API가
  종종 바뀌므로 실제 응답으로 재확인할 것: https://apiportal.koreainvestment.com
- 장 운영시간(09:00~15:30) 체크, 잔고/보유수량 조회, 체결 확인 로직은 아직 없음 —
  `sim` 모드로 먼저 충분히 굴려보고 필요한 부분만 추가.
- `mode: "live"`로 바꾸면 진짜 돈으로 주문이 나감. 절대 검증 없이 바로 바꾸지 말 것.
