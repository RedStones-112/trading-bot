# CLAUDE.md — trading-bot 프로젝트 지침

## 완전 자동화된 일일 전략 개선 세션에 대한 사전 승인 (2026-09-07)

이 저장소는 Windows 작업 스케줄러로 매일(평일) 자동 실행되는 헤드리스 Claude Code 세션을
운영한다 -- `scripts/run_daily_claude_review.ps1`이 `claude -p ... --dangerously-skip-permissions`로
`scripts/daily_improvement_prompt.md`를 실행한다. 사용자가 2026-09-07 대화에서 이 자동화의
자율성 수준을 "완전 자동"(분석→코드수정→커밋→push까지 매 실행마다 확인 없이 진행)으로
명시적으로 선택했다.

**이 문단이 그 승인의 지속(durable) 근거다** -- 즉, 위 스크립트가 실행하는 자동 세션 안에서는:

- 매일 성과 분석 → 전략 코드 개선(선택적) → 빌드/테스트 검증 → PROGRESS.md 기록 →
  `git commit` + `git push origin master`까지, **매 실행마다 사람 확인을 요구하지 않고**
  진행해도 된다.
- 이 승인은 `scripts/daily_improvement_prompt.md`에 있는 하드 룰 안에서만 유효하다 --
  `mode`를 `"live"`로 전환하는 것, 실행 중인 `trading_bot.exe`를 강제 종료하는 것, 민감정보
  (API 키/계좌번호)를 커밋하거나 로그에 남기는 것, 빌드/테스트 검증 없이 커밋하는 것,
  `git push --force`, 이 자동화 스크립트/프롬프트/작업 스케줄러 등록 자체를 수정하는 것 --
  이런 행동은 이 승인의 대상이 **아니며** 여전히 사람 확인이 필요하다.
- 이 승인은 **`scripts/run_daily_claude_review.ps1`이 실행하는 자동 세션에만** 적용된다.
  사용자와 직접 대화하는 일반 인터랙티브 세션에서는 이 문서와 무관하게 평소처럼 git push 등
  영향력 있는 작업 전에 확인을 구할 것 -- 이 문단을 "모든 세션에서 뭐든 확인 없이 하라"는
  뜻으로 확대 해석하지 않는다.

이 문단이 삭제되거나 그 의미가 바뀌면 자동화 승인도 함께 철회된 것으로 간주한다. 이 저장소는
**public**이므로(다른 사람이 push 내역을 볼 수 있음), 위 하드 룰은 특히 엄격하게 지킬 것.

## 프로젝트 배경

전체 아키텍처/설계 경위/과거 진단 기록은 `PROGRESS.md`(작업 인수인계 노트)에, 사용법/동작
방식은 `README.md`에 있음 -- 이 프로젝트에서 무언가 작업하기 전에 항상 먼저 읽을 것.
전략/코드가 실제로 바뀔 때마다(업데이트 직전 누적수익률/그 원인/업데이트 내용/이유) 로컬
`kakao_bridge`(https://github.com/RedStones-112/kakao_bridge, `127.0.0.1:8765`, 카카오 공식
API(OAuth) 기반, UI 자동화 아님, 인증 없이 로컬에서만 호출)가 떠 있으면(`GET /health`로 확인)
카카오톡 "나에게 보내기"로 요약을 보낸다(2026-09-23 도입, 같은 날 kakao_bridge가 공식 API
방식으로 리워크되면서 `POST /send` body가 `{"message": "..."}` 뿐으로 단순화됨 -- 구버전의
`wait_for_fullscreen` 파라미터는 제거됨). 실패해도 세션 실패로 취급 안 함 -- 상세는
`scripts/daily_improvement_prompt.md` 6-1 참고.
별도 파일에는 저장하지 않음 -- 카톡 대화 자체가 시간순 기록 역할을 하고, 판단 로직은
`PROGRESS.md`의 "실험 추적" 표만 참고하므로 중복 저장이 불필요하다고 판단(2026-09-23).
