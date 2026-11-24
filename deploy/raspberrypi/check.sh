#!/usr/bin/env bash
# 연결 점검(조회만) + 드라이런(주문 없음). 저장소 루트 기준.
set -euo pipefail
cd "$(dirname "$0")/../.."
.venv-live/bin/python -m rl.live.check_account
.venv-live/bin/python -m rl.live.paper_trade --force
