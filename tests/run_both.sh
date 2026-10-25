#!/usr/bin/env bash
set -uo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
export RUNTIME_SECONDS=600
bash "$DIR/run_benchmark.sh" fifo nbd5 58231
bash "$DIR/run_benchmark.sh" lifetime nbd6 58232
echo "ALL BENCHMARKS DONE"
