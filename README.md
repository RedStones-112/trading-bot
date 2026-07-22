# trading-bot (draft)

한국투자증권(KIS) Developers API로 붙는 최소 트레이딩 봇 초안. 모의투자(paper trading) 기본,
단순 이동평균(SMA) 골든/데드 크로스 전략. WinHTTP(Windows 내장)로 HTTPS 처리해서
libcurl 등 외부 의존성 없음. JSON은 nlohmann/json 헤더 하나만 vendor.

## 준비

1. https://apiportal.koreainvestment.com 가입 → 앱 등록 → App Key / App Secret 발급
2. 한국투자증권 계좌 개설 후 모의투자 신청 (실제 앱에서 "모의투자 신청" 메뉴)
3. `config.example.json`을 `config.json`으로 복사, appkey/appsecret/cano(계좌번호 앞 8자리)/
   acnt_prdt_cd(계좌상품코드 뒤 2자리) 채우기. `config.json`은 `.gitignore`에 이미 등록되어 있음.

## 빌드 (MSYS2 UCRT64 g++ 또는 MSVC)

```
cmake -G "MinGW Makefiles" -S . -B build
cmake --build build
```

`build/` 안에 `config.json`을 두고 실행:

```
cd build
./trading_bot.exe
```

## 동작

`config.json`의 `code`(종목코드) 하루봉 종가로 SMA(short/long) 계산, 골든크로스면 시장가 매수,
데드크로스면 보유분 시장가 매도. `poll_seconds`마다 반복. 모든 요청/주문은 `trading.log`에 기록.

## 검증 필요

- KIS API의 tr_id, 필드명(`stck_prpr`, `stck_clpr` 등)은 문서 기준으로 작성했지만 API가
  종종 바뀌므로 실제 응답으로 재확인할 것: https://apiportal.koreainvestment.com
- 장 운영시간(09:00~15:30) 체크, 잔고/보유수량 조회, 주문 체결 확인 로직은 아직 없음 —
  paper trading으로 먼저 충분히 굴려보고 필요한 부분만 추가.
- `paper_trading: true`에서 `false`로 바꾸고 실계좌 appkey로 교체하면 실거래로 전환됨.
  절대 검증 없이 바로 바꾸지 말 것.
