#!/usr/bin/env bash
# Bit-exact NSS checks on identical inputs (the scene renderer itself is not deterministic run to run).
#   nss_replay.sh record <name> [quality=1] [frames=12]   -> runs/raw/<name>: inputs + params + NSS output
#   nss_replay.sh replay <name> <recorded name> [quality=1] [frames=12]  -> NSS output on the recorded inputs
# then: python compare_raw.py runs/raw/<a> runs/raw/<b>   (upscaled_NNNN.bin, must print IDENTICAL)
set -u
MODE=$1; NAME=$2
if [ "$MODE" = record ]; then Q=${3:-1}; N=${4:-12}; else SRC=$3; Q=${4:-1}; N=${5:-12}; fi
OUT=/j/neuralupscale/runs/raw/$NAME
rm -rf "$OUT"; mkdir -p "$OUT"
cd /j/neuralupscale/Streamline_Sample/_bin
if [ "$MODE" = record ]; then
    NSS_RECORD_DIR="J:/neuralupscale/runs/raw/$NAME" CAPTURE_DUMP_UPSCALED=1 CAPTURE_DUMP_MV=1 timeout 600 ./StreamlineSample.exe -vk -AA nss -NSS_quality $Q \
        -captureDir "J:/neuralupscale/runs/raw/$NAME" -captureFrames $N -captureWarmup 0 > /dev/null 2>&1
else
    NSS_REPLAY_DIR="J:/neuralupscale/runs/raw/$SRC" CAPTURE_DUMP_UPSCALED=1 timeout 600 ./StreamlineSample.exe -vk -AA nss -NSS_quality $Q \
        -captureDir "J:/neuralupscale/runs/raw/$NAME" -captureFrames $N -captureWarmup 0 > /dev/null 2>&1
    rm -f "$OUT"/input_*.bin
fi
echo "$MODE $NAME exit=$? outputs=$(ls $OUT/upscaled_*.bin 2>/dev/null | wc -l)"
