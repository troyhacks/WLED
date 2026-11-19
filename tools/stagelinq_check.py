#!/usr/bin/env bash
# build -> flash -> wait -> verify, the whole loop for the StageLinq usermod.
set -e
cd /c/Users/troys/GitHub/WLED

ENV=esp32p4_8MB_troyhacks_personal
PIO=/c/Users/troys/.platformio/penv/Scripts/pio.exe
ESP=/c/Users/troys/.platformio/penv/Scripts/esptool.exe
BOOT=/c/Users/troys/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
B=.pio/build/$ENV

echo "===== BUILD ====="
$PIO run -e $ENV 2>&1 | grep -E "error:|Error [0-9]|SUCCESS|FAILED|Took" || true

# A build that failed leaves the OLD firmware.bin in place, and flashing it
# is how you spend an afternoon chasing a bug that was never in the flash.
if ! grep -q "SUCCESS" <<< "$($PIO run -e $ENV 2>&1)"; then
  echo "THE BUILD FAILED - not flashing"; exit 1
fi
echo "firmware: $(stat -c %y $B/firmware.bin)"

echo "===== FLASH ====="
PYTHONIOENCODING=utf-8 $ESP --chip esp32p4 --port COM4 --baud 921600 \
  --before default-reset --after hard-reset write-flash \
  0x2000 $B/bootloader.bin \
  0x8000 $B/partitions.bin \
  0xE000 $BOOT \
  0x10000 $B/firmware.bin 2>&1 | grep -E "Hash of data verified|Wrote|error|Error" | tail -8

echo "===== WAIT FOR THE BOARD ====="
for i in $(seq 1 40); do
  if curl -s --max-time 5 http://192.168.6.249/json/ar -o ar_now.json 2>/dev/null && [ -s ar_now.json ]; then
    echo "board answered after ~$((i*5)) s"; break
  fi
  sleep 5
done

echo "===== VERIFY ====="
python - <<'PY'
import json
d = json.load(open('ar_now.json'))
t = d.get('AudioReactive', {}).get('tempo', {})
print("tempo.source     :", t.get('source'))
print("tempo.source_id  :", t.get('source_id'))
print("tempo.bpm        :", t.get('bpm'))
print("tempo.source_beat:", t.get('source_beat'))
print("tempo.source_bar :", t.get('source_bar'))
print("tempo.source_32  :", t.get('source_32'))
print("StageLinq:")
print(json.dumps(d.get('StageLinq'), indent=2))
PY
