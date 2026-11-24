#!/usr/bin/env bash
# cron이 평일 23:40(KST)에 실행. 미국 휴장일/장외시간/데이터지연이면 runner가 알아서 아무것도 안 함.
# EXTRA_ARGS로 옵션 전달 가능 (예: --max-names 60). 동시 실행 방지(flock).
cd "$(dirname "$0")/../.."
mkdir -p rl/live/state
exec 9>rl/live/state/run.lock
flock -n 9 || { echo "$(date) 이미 실행 중"; exit 0; }
EXTRA_ARGS="$(cat deploy/raspberrypi/extra_args 2>/dev/null || true)"
echo "===== $(date '+%F %T') 시작 $EXTRA_ARGS"
.venv-live/bin/python -m rl.live.paper_trade --execute $EXTRA_ARGS
echo "===== $(date '+%F %T') 종료 (exit $?)"
