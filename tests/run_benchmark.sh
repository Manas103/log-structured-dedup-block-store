#!/usr/bin/env bash
# Runs the write-amplification / utilization / read-tail-latency benchmark
# behind the project's README numbers, for one GC policy at a time.
# Usage: run_benchmark.sh <lifetime|fifo> <nbd-device e.g. nbd5> <port>
set -euo pipefail
POLICY="$1"
DEV="/dev/$2"
PORT="$3"
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"
WORK="$HOME/build/dedup-bench-$POLICY"
mkdir -p "$WORK"
IMG="$WORK/backing.img"
STATS="$WORK/stats.json"
SERVER_LOG="$WORK/server.log"
PIDFILE="$WORK/server.pid"

NUM_BLOCKS=524288          # 2 GiB logical
BACKING_MIB=2560           # 2.5 GiB physical, ~0.8 ceiling utilization, settles near the 0.85 reserve target
BACKING_BYTES=$((BACKING_MIB * 1024 * 1024))

rm -f "$IMG" "$STATS" "$SERVER_LOG"

"$BUILD_DIR/nbd_server" "$IMG" "$BACKING_BYTES" "$NUM_BLOCKS" "$PORT" "$POLICY" create "$STATS" > "$SERVER_LOG" 2>&1 &
echo $! > "$PIDFILE"
sleep 1

sudo -n nbd-client 127.0.0.1 "$PORT" "$DEV" -b 4096
sleep 1

echo "=== prep: one sequential pass over the whole logical device (establishes cold baseline) ==="
sudo -n fio --name=prep --filename="$DEV" --rw=write --bs=1M --size=2048M --direct=1 \
  --iodepth=1 --ioengine=sync --dedupe_percentage=30 > "$WORK/prep_output.txt" 2>&1
tail -15 "$WORK/prep_output.txt"

echo "=== main: skewed hot/cold churn with reads mixed in, driving GC continuously ==="
sudo -n fio --name=churn --filename="$DEV" --rw=randrw --rwmixread=20 --bs=4096 \
  --offset=0 --size=512M --direct=1 --iodepth=1 --ioengine=sync \
  --dedupe_percentage=50 --time_based --runtime="${RUNTIME_SECONDS:-600}" \
  --percentile_list=50:95:99:99.9:99.99 > "$WORK/main_output.txt" 2>&1
tail -40 "$WORK/main_output.txt"

echo "=== engine stats ($POLICY) ==="
cat "$STATS"
echo

sudo -n nbd-client -d "$DEV" || true
kill -TERM "$(cat "$PIDFILE")" || true
sleep 2
echo "done: $POLICY"
