#!/usr/bin/env bash
# Заливка прошивки, сброс контроллера, чтение логов 2 минуты.
# Использование: из корня проекта: bash scripts/upload_and_log.sh

set -e
cd "$(dirname "$0")/.."
PORT="${1:-/dev/ttyACM0}"
LOG="${2:-esp_2min.log}"

echo "=== Upload ==="
pio run -e esp32devdeb_https -t upload

echo "=== Reset controller (RTS) ==="
python3 -c "
import serial
import time
s = serial.Serial('$PORT', 115200)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.2)
s.setRTS(False)
time.sleep(0.2)
s.close()
"

echo "=== Reading serial for 120 s (log: $LOG) ==="
sleep 1
stty -F "$PORT" 115200 raw -echo 2>/dev/null || true
timeout 120 cat "$PORT" 2>/dev/null | tee "$LOG" || true
echo ""
echo "=== Done. Size: $(wc -c < "$LOG" 2>/dev/null || echo 0) bytes ==="
