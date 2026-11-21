#!/bin/bash
# Metal capture of ~2 frames of the graphics service in the R&C level (KosmicKrisp with the
# ps4mac patch: KK_CAPTURE_TRIGGER). Output: ../reports/capture/ps4mac_service.gputrace
cd "$(dirname "$0")"
out=$PWD/../reports/capture; mkdir -p "$out"; trig=$out/trigger
[ -e "$out/ps4mac_service.gputrace" ] && mv "$out/ps4mac_service.gputrace" "$out/ps4mac_service-$(date +%s).gputrace"
MTL_CAPTURE_ENABLED=1 KK_CAPTURE_TRIGGER=$trig KK_CAPTURE_DIRECTORY=$out KK_CAPTURE_SUBMITS=${1:-60} \
  PS4MAC_SERVICE=$HOME/0_Games/ps4mac_port/out/service/ps4mac_service SPARTAN_FRAME_STATS=1 \
  PORT_DIR=$HOME/0_Games/ps4mac_port timeout 600 python3 drive.py ../reports/capture.log level6.txt > /dev/null 2>&1 &
sleep 20
# The level: three reports under 20 FPS after the title screen (30 FPS) was seen.
for i in $(seq 1 400); do sleep 1; c=$(grep 'Frame stats' ../reports/capture.log 2>/dev/null | awk '$3>=29 {t=1} t && $3<20 && $3>5 {n++} END {print n+0}'); [ "$c" -ge 3 ] && break; done
touch "$trig"
for i in $(seq 1 240); do sleep 1; grep -q 'KK capture: written' ../reports/capture.log && break; done
grep 'KK capture' ../reports/capture.log
wait
du -sh "$out"/*.gputrace 2>/dev/null
