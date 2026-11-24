# 안드로이드 모의투자 앱 (PaperTrader) -- 이어서 하기 문서

> 상태(2026-11-24): **소스만 작성, 아직 한 번도 빌드 안 함**(이 PC에 Android Studio/JDK/SDK 없음).
> Python 실행 로직은 PC에서 같은 진입점(`rl/live/android_entry.py`)으로 검증. 첫 실제 주문은 사용자 지시로 보류.

## 구조

- **Chaquopy**로 앱 안에 Python 3.11을 넣고, 저장소의 실행기 코드를 빌드 때 그대로 복사해 씀
  (`app/build.gradle.kts`의 `syncPython` 태스크: `rl/features.py`, `rl/live/*.py`, `rl/live/model/exp8r.npz`,
  `rl/live/universe464.json`). 라즈베리파이와 같은 `rl/live/runner.py` 로직.
- 데이터는 **KIS 시세 API**(`--backend kis`): yfinance가 쓰는 curl_cffi를 Chaquopy에서 못 쓰고, 야후 직접 호출은 429로
  막힘. 처음 "데이터 준비"로 3년치를 받아 `files/state/prices/`에 캐시(~1시간), 이후 매일 종목당 1회 호출(~9분).
  PC 검증: KIS 수정주가 vs yfinance 일간수익률 차 최대 0.0065(XOM 배당일 하루), VIX 99.4% 일치.
- 일정: `AlarmManager.setExactAndAllowWhileIdle` 평일 23:40 KST -> `TradeService`(포그라운드, 웨이크락) -> Python.
  미국 휴장일/장외시간/데이터지연이면 Python이 아무것도 안 하고 알림만. 재부팅 시 `BootReceiver`가 재예약.
- 키 저장: `EncryptedSharedPreferences`(Android Keystore). 앱은 모의투자 전용(`mode`를 항상 `paper`로 고정,
  서버 주소도 모의투자만 들어 있음).

| 파일 | 역할 |
|---|---|
| `app/src/main/java/.../MainActivity.kt` | 키/계좌/종목수 입력, 버튼(점검·데이터준비·드라이런·주문·자동 on/off), 진행 로그 |
| `.../TradeService.kt` | 포그라운드 서비스, Python 호출, 결과 알림 |
| `.../Scheduler.kt`, `Receivers.kt` | 평일 23:40 정확 알람, 재부팅 재예약 |
| `.../SecureConfig.kt` | 암호화 설정 저장 |
| `rl/live/android_entry.py` (저장소) | 앱이 부르는 Python 함수: `check`, `prefetch`, `run` |

## 빌드/설치 순서 (처음 한 번)

1. PC에 **Android Studio** 설치(JDK/SDK 포함, 수 GB). `mobile/android` 폴더를 열기.
2. Gradle 동기화. 실패하면 대개 버전 문제:
   - `build.gradle.kts`의 AGP(8.7.3)/Kotlin(2.0.21)/Chaquopy(16.0.0) 버전을 IDE 제안대로 올리기.
   - Chaquopy가 `pandas`/`numpy` 휠을 못 찾으면 `version = "3.11"`을 Chaquopy 문서의 지원 버전(3.10/3.12 등)으로 바꿔보기.
   - Gradle wrapper가 없으므로 IDE가 생성하게 두면 됨.
3. 폰(안드로이드 8.0+, arm64)을 USB 디버깅으로 연결 -> Run. 에뮬레이터는 `abiFilters`에 `x86_64` 추가 필요.
4. 앱에서: 키/계좌 입력 -> 저장 -> **1) 연결 점검** -> **2) 데이터 준비**(1시간, 충전기 연결) -> **3) 드라이런**.
5. 권한: 알림 허용, **정확한 알람 허용**, **배터리 최적화 제외**(삼성은 추가로 설정 > 배터리 > 백그라운드 사용 제한 >
   "절전 예외 앱"에 등록). 안 하면 23:40 실행이 빠질 수 있음.
6. 사용자 확인 후 "자동 실행 켜기".

## 운영상 한계 (라즈베리파이 대비)

- 폰이 꺼져 있거나 오프라인이면 그날은 건너뜀(다음 평일 정상). 매일 쓰는 폰이면 23:40에 몇~수십 분 데이터/배터리 사용.
- 안드로이드 15+는 dataSync 포그라운드 서비스가 하루 6시간 제한 -- 일일 실행(~10분)은 문제없음.
- 여러 기기(파이 + 폰)를 동시에 켜면 같은 계좌에 주문이 두 번 나감 -- **한 곳만 자동 실행할 것**.

## 결정 대기 / 미확인

- 종목 수 상한(`max_names`): 0(모델 전체, $10만이면 고가주 못 사서 ~60%만 투자) vs 60 등. 앱 입력칸에서 설정.
- KIS 주문/취소 응답, Chaquopy에서 pandas 동작, 실기기 알람 정확도 -- 모두 첫 빌드/실행 때 확인.
- 라즈베리파이 버전 문서: `deploy/raspberrypi/README.md`
