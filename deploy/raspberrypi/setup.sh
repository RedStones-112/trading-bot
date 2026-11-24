#!/usr/bin/env bash
# 라즈베리파이(64비트 Raspberry Pi OS, Pi 4/5 권장) 1회 설치. 저장소 루트에서: bash deploy/raspberrypi/setup.sh
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
echo "[1/5] 시간대 Asia/Seoul (cron 시각 기준)"
sudo timedatectl set-timezone Asia/Seoul
echo "[2/5] 패키지"
sudo apt-get update -y
sudo apt-get install -y python3-venv python3-pip git
echo "[3/5] 가상환경 .venv-live"
python3 -m venv .venv-live
.venv-live/bin/pip install --upgrade pip
.venv-live/bin/pip install -r rl/live/requirements-live.txt
echo "[4/5] 필수 파일 확인"
for f in rl/live/model/exp8r.npz rl/live/universe464.json; do
  [ -f "$f" ] || { echo "없음: $f (git pull 했는지 확인)"; exit 1; }
done
if [ ! -f config.json ]; then
  cp deploy/raspberrypi/config.pi.example.json config.json
  chmod 600 config.json
  echo "  config.json 생성됨 -> 키/계좌번호를 채우세요: nano config.json"
fi
chmod 600 config.json
echo "[5/5] 완료. 다음: nano config.json -> bash deploy/raspberrypi/check.sh -> bash deploy/raspberrypi/install_cron.sh"
