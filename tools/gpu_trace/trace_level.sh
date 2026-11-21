#!/bin/bash
# Records a Metal System Trace of the graphics service in the R&C level, labels on; writes the
# GPU busy and per-label summaries to ../reports/$1-summary.txt.
cd "$(dirname "$0")"
name=$1; X=/Applications/Xcode.app/Contents/Developer; T=../reports/$name.trace
KK_LABEL_ENCODERS=1 SPARTAN_GPU_PROFILE=labels PS4MAC_SERVICE=$HOME/0_Games/ps4mac_port/out/service/ps4mac_service \
  SPARTAN_FRAME_STATS=1 PORT_DIR=$HOME/0_Games/ps4mac_port timeout 400 python3 drive.py ../reports/$name.log level6.txt > /dev/null 2>&1 &
sleep 20
for i in $(seq 1 400); do sleep 1; c=$(grep 'Frame stats' ../reports/$name.log 2>/dev/null | awk '$3>=29 {t=1} t && $3<20 && $3>5 {n++} END {print n+0}'); [ "$c" -ge 3 ] && break; done
P=$(pgrep -x ps4mac_service)
extra=(); [ -n "$EXTRA_INSTRUMENT" ] && extra=(--instrument "$EXTRA_INSTRUMENT")
DEVELOPER_DIR=$X timeout 300 xcrun xctrace record --template "${TEMPLATE:-Metal System Trace}" "${extra[@]}" --attach $P --time-limit 3s --output $T > ../reports/$name-rec.log 2>&1
echo "record rc=$?" > ../reports/$name-summary.txt
wait
DEVELOPER_DIR=$X timeout 300 xcrun xctrace export --input $T --xpath '/trace-toc/run/data/table[@schema="metal-gpu-intervals"]' > ../reports/$name.xml 2>/dev/null
fps=$(grep 'Frame stats' ../reports/$name.log | tail -3 | awk '{s+=$3} END {printf "%.1f", s/3}')
echo "fps $fps" >> ../reports/$name-summary.txt
python3 -I gpu_busy.py ../reports/$name.xml ps4mac_service >> ../reports/$name-summary.txt
python3 -I gpu_labels.py ../reports/$name.xml $fps 40 >> ../reports/$name-summary.txt
