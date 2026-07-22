# 개발 로그 / 작업 인수인계 노트

다음에 이 프로젝트를 이어서 작업할 때(사람이든 Claude든) 여기부터 읽을 것.
`README.md`는 사용법 중심, 이 파일은 "왜 이렇게 만들었는지"와 지금까지의 경위 중심.

## 프로젝트 개요

- 목적: 한국투자증권(KIS) Developers API로 붙는 실제/모의 주식 트레이딩 봇 (C++ 초안)
- 위치: `C:\Users\wogur\OneDrive\Desktop\project\trading`
- GitHub: https://github.com/RedStones-112/trading-bot — **public repo**
- 빌드: CMake + MSYS2 UCRT64 g++. WinHTTP(Windows 내장)로 HTTPS, libcurl 등 외부
  의존성 없음. JSON은 nlohmann/json 헤더 하나만 vendor(`third_party/json.hpp`).

## 아키텍처

- `src/broker.hpp`: `IBroker` 인터페이스 + `StockInfo`(code/name/price/dayChangePct).
  구현체 3개:
  - `KisClient` (src/kis_client.*): 실제 KIS REST API
  - `MockBroker` (src/mock_broker.*): 완전 오프라인, 가상 종목 5개 랜덤워크
  - `SimBroker` (src/sim_broker.*): KIS 실시세는 그대로 쓰되 주문은 로컬 가상 체결
- `src/strategy.hpp`: 순수함수 3개 — `smaCrossSignal`(골든/데드크로스), `smaMomentum`,
  `netProfitPct`(수수료/세금 반영 순손익률)
- `src/news_crawler.hpp/cpp`: RSS 피드 파싱(직접 짠 `<item>`/`<title>` 추출, XML
  라이브러리 없음) + Google News 검색(`news.google.com/rss/search`) + 키워드 기반
  감성 점수화
- `src/main.cpp`: config.json 로드 → mode별 브로커 생성 → 정규장 체크 → (보유 없으면)
  스캔/선택/매수, (보유 중이면) 모니터링/매도 루프. `holdings.txt`를 5초마다 갱신하는
  별도 스레드도 여기서 띄움.
- `tests/test_strategy.cpp`: 결정론적 self-check (SMA 크로스, netProfitPct,
  MockBroker, NewsCrawler 감성점수). `cmake --build build --target test_strategy`로 빌드.

## 동작 흐름 (현재 최종본)

0. `paper`/`live` 모드는 평일 09:00~15:30(로컬 시각)가 아니면 스캔 자체를 스킵 (장
   마감 시 KIS가 주문을 거부하는데, 그 전에 스캔+뉴스크롤링을 다 하는 낭비를 막기 위함).
1. 보유 종목 없으면: KIS 거래량순위 API(세그먼트 `0000`/`1001`/`2001` 병합, 세그먼트당
   최대 30개 → 중복 제거 후 최대 ~90개) → 레버리지/인버스 ETF·ETN 이름 필터링 →
   `watchlist_size`만큼 후보 확보.
2. 후보가 `deep_scan_limit`보다 많으면 전일대비율(`prdy_ctrt`, 순위조회 응답에 이미
   있는 무료 필드)로 정렬해서 상위 N개만 정밀 분석(일봉 히스토리 조회는 비용이 큼).
3. RSS 뉴스(매경/한경/연합뉴스) 크롤링 → 종목명 언급 헤드라인에서 긍정/부정 키워드로
   감성 점수화.
4. 각 후보의 SMA 골든크로스 여부 확인 → Buy 신호 후보 중 기댓값(`이득 × 확률`, 이득은
   `take_profit_pct` 기준 목표수익, 확률은 감성점수를 0.05~0.95로 매핑한 휴리스틱)이
   가장 높은 종목 **하나만** 매수.
5. 보유 중엔 매 poll마다: (a) 데드크로스, (b) 수수료/세금 차감 순손익률이
   `take_profit_pct` 이상, (c) 보유종목명으로 Google News를 직접 검색한 감성점수가
   `bad_news_sentiment_threshold` 이하(악재) — 셋 중 하나만 걸려도 즉시 매도.
   (c)는 보유종목이 스캔의 거래량순위 후보에서 벗어나도 계속 추적하기 위해 스캔의
   뉴스풀과 별개로 매번 직접 검색함.
6. `holdings.txt`(5초 고정 간격) / `trades.log`(체결마다 CSV 한 줄) / 콘솔+`trading.log`
   (실시간 스캔/시그널/체결 로그).

## 사용자가 확인해준 설계 결정

- 기댓값 공식: **이득 × 확률**(표준). "이득/확률"은 채택 안 함.
- 매도 트리거: 데드크로스 **and** 수익실현 **and** 악재감지, 셋 다 사용(OR 조건).
- 뉴스 크롤링 범위: **RSS만** 우선 구현. 유튜브/SNS는 명시적으로 제외
  (공식 API 인증 필요, 비공식 스크래핑은 ToS/차단 위험 커서 이 단계에서는 안 함).
- GitHub repo는 처음 private로 만들었다가 사용자 요청으로 public 전환.

## 실제로 라이브 검증한 것 (KIS 모의투자 서버, 2026-07-22 기준)

- 인증, 시세조회(inquire-price), 일봉조회(inquire-daily-itemchartprice), 거래량순위
  (volume-rank), 매수/매도 주문(order-cash) 전부 실제 호출로 확인.
- 거래량순위: 세그먼트당 정확히 30행, `0000`/`1001`/`2001`이 서로 다른(거의 안 겹치는)
  종목 리스트 반환하는 것 raw JSON 덤프로 확인. `stck_prpr`(현재가), `prdy_ctrt`
  (전일대비율) 필드가 이미 포함돼 있어서 별도 현재가 조회 불필요함을 확인.
- 뉴스: 매경/한경/연합뉴스 RSS 실제 파싱 성공(220건). Google News 검색으로 "삼성전자"
  검색 시 실제 106건/감성점수 +7 확인.
- 매수→매도 전체 사이클: mock 모드에서 익절 매도까지 확인. 실계좌 앱키로 매수 주문
  1건, 매도 주문 1건 각각 accepted(주문번호 발급) 확인(초기 검증 단계, 별도 스크래치
  테스트).
- "왜 아무것도 안 사나" 이슈(2026-07-22 16:2x): 65개 후보 스캔이 끝까지 돌고 최고
  기댓값 종목(삼익제약, EV 61.0)을 골라 매수 시도했으나 KIS가 "모의투자 장종료
  입니다"로 거부 — **버그 아니었음**, 정규장(15:30) 마감 후였음. 이후 정규장 시간
  체크를 추가해서 장 마감 땐 스캔 자체를 안 하도록 고침.

## 발견하고 고친 버그들 (시간순)

1. WinHTTP `GetLastError()`를 핸들 정리(`WinHttpCloseHandle`) *이후*에 호출해서 항상 0으로
   나오던 버그 — 실패 직후에 캡처하도록 수정.
2. WinHTTP 연결 포트를 443으로 하드코딩 — KIS는 모의투자 29443, 실서버 9443 커스텀
   포트를 씀. `http::request`에 포트 파라미터 추가.
3. 콘솔에 한글이 깨져 보임 — `SetConsoleOutputCP(CP_UTF8)` 누락. (로그 파일 자체는
   원래도 정상이었음, 콘솔 표시만 문제)
4. KIS 레이트리밋(`EGW00201`, 모의투자 계좌가 특히 엄격) — 시작 시 여러 API 호출이
   연달아 나가서 발생. 호출 사이 1.1초 pause 추가 + 일봉 히스토리는 매수 시점에
   한 번만 캐시(매 poll마다 재조회 안 함)로 호출 수 자체를 줄임.
5. `watchlist_size=100`을 줘도 10개만 반환 — 거래량순위 API가 세그먼트 하나당 최대
   30개까지만 응답하는 게 원인. 세그먼트 3개(`0000`/`1001`/`2001`) 병합+중복제거로
   해결(최대 ~90개까지 확보 가능).
6. MockBroker 회귀 버그: 순위조회 최적화(현재가를 랭킹 응답에서 재사용) 이후, mock
   모드의 랜덤워크가 `getCurrentPrice()`에서만 진행되고 `getTopVolumeStocks()`에서는
   안 움직여서 스캔 중 가격이 고정돼 있던 문제 — `advance()` 헬퍼로 공유해서 해결.

## 보안 관련 메모

- 사용자가 채팅에 실제 KIS appkey/appsecret를 붙여넣은 적 있음(요청: "api key값 모두
  넣었고") → `config.example.json`(placeholder 전용, git 추적됨)에 실수로 들어갔던 걸
  커밋 전에 되돌리고 `config.json`(`.gitignore` 등록됨)으로만 옮김.
- GitHub 계정 로그인은 비밀번호가 아니라 `gh auth login --web` 브라우저 OAuth로 진행
  (GitHub는 2021년부터 git/API에 비밀번호 인증 미지원). 사용자가 채팅에 계정
  비밀번호를 평문으로 준 적 있는데, 그건 아예 사용하지 않고 폐기 권장만 전달함.
- **이 repo는 public** — `config.json`, `trades.log`, `holdings.txt`, `sim_fills.log`
  전부 `.gitignore`에 등록되어 있음. 새 파일 추가 시 실키/계좌번호가 안 들어가는지
  항상 재확인할 것.

## 지금 config.json 상태 (실제 파일은 gitignore됨, 값만 참고용으로 기록)

- `mode`: paper, 계좌번호는 `config.json`(gitignore됨)에만 있음 (모의투자, 실계좌
  앱키 사용 — 모의투자 신청 완료 상태)
- `watchlist_size`: 50, `deep_scan_limit`: 20, `qty`: 1, `sma_short/long`: 5/20,
  `poll_seconds`: 5
- `take_profit_pct`: 0.02, `fee_rate`: 0.00015, `tax_rate`: 0.0018,
  `bad_news_sentiment_threshold`: -2.0
- `news_feeds`: 매경/한경/연합뉴스 기본값 그대로

## 알려진 한계 / 다음에 할 만한 것

- 기댓값의 "확률"은 뉴스 감성 점수를 선형 매핑한 휴리스틱일 뿐 백테스트로 보정된
  값이 아님 — 실거래(`mode: live`) 투입 전에 반드시 백테스트 필요.
- 정규장 체크는 요일+시각만 봄, 공휴일 캘린더 없음 — 공휴일엔 스캔은 돌지만 주문
  단계에서 거부됨.
- 잔고 조회(예수금/보유수량 검증) 로직 없음.
- ETF/ETN 필터는 이름 문자열에 `KODEX`/`TIGER`/`레버리지`/`인버스` 등이 포함되는지
  보는 휴리스틱 — 완벽하지 않음.
- 매수 후 모니터링용 일봉 히스토리는 매수 시점에 한 번만 캐시 — 포지션을 자정 넘어
  여러 날 들고 있으면 하루치 갱신이 안 됨(알려진 한계, README에도 명시).
- tr_id/FID 파라미터/필드명은 실제 라이브 호출로 검증했지만 KIS API가 바뀔 수 있으니
  주기적으로 재확인 권장: https://apiportal.koreainvestment.com
- `mode: "live"`로 전환하면 진짜 돈이 나감 — 충분히 검증 후에만 전환할 것.
