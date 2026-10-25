#!/usr/bin/env bash
# NSS vs DLSS cost in Streamline_Sample (Vulkan, 960x540 -> 1920x1080): two passes in alternating order.
# Usage: run_nss_vs_dlss.sh <output folder name>
set -u
ROOT=/j/neuralupscale/Streamline_Sample
NAME=${1:-nss_vs_dlss}
OUT=J:/neuralupscale/runs/$NAME
mkdir -p /j/neuralupscale/runs/$NAME

run() {   # run <name> <args...>
    local name=$1
    shift
    rm -rf "/j/neuralupscale/runs/$NAME/$name"
    (cd "$ROOT/_bin" && timeout 900 ./StreamlineSample.exe -vk "$@" -benchmark -captureDir "$OUT/$name" -captureFrames 1000 -captureWarmup 60 > /dev/null 2>&1)
    echo "$name exit=$? $(grep -h upscaler_gpu /j/neuralupscale/runs/$NAME/$name/benchmark.txt)"
}

for pass in a b; do
    if [ $pass = a ]; then order="dlss nss_b nss_p nss_q"; else order="nss_q nss_p nss_b dlss"; fi
    for which in $order; do
        case $which in
            dlss)  run dlss_E_$pass -DLSS_mode 1 -DLSS_preset E ;;
            nss_q) run nss_quality_$pass -AA nss -NSS_quality 0 ;;
            nss_b) run nss_balanced_$pass -AA nss -NSS_quality 1 ;;
            nss_p) run nss_performance_$pass -AA nss -NSS_quality 2 ;;
        esac
    done
done
echo done
