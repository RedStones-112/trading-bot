#!/usr/bin/env bash
# 평일(월~금) 23:40 KST 실행 등록 = 미국 동부 09:40(서머타임 10:40), 정규장 중. 로그는 rl/live/state/cron.log (5MB 넘으면 회전).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LINE="40 23 * * 1-5 bash $REPO/deploy/raspberrypi/run_daily.sh >> $REPO/rl/live/state/cron.log 2>&1"
ROT="0 12 * * 0 [ \$(stat -c\%s $REPO/rl/live/state/cron.log 2>/dev/null || echo 0) -gt 5000000 ] && mv $REPO/rl/live/state/cron.log $REPO/rl/live/state/cron.log.1"
mkdir -p "$REPO/rl/live/state"
( crontab -l 2>/dev/null | grep -v "run_daily.sh" | grep -v "cron.log.1" ; echo "$LINE"; echo "$ROT" ) | crontab -
echo "등록됨:"; crontab -l | grep -E "run_daily|cron.log"
echo "해제하려면: crontab -l | grep -v run_daily.sh | grep -v cron.log.1 | crontab -"
