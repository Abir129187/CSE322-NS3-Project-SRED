#!/bin/bash
# ============================================================
# run_experiments.sh — Batch runner for Delay-Aware SRED
# (Wireless Mobile and Wired Static experiments)
# ============================================================

NS3_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="${NS3_DIR}/scratch/results"
SIM_TIME=30   # Steady-state reached quickly in these topologies

mkdir -p "${RESULTS_DIR}"

HEADER="nNodes,nFlows,packetsPerSec,varParam,throughput_kbps,avg_delay_ms,PDR,dropRatio,energy_J"

# ── Defaults ──
DEF_NODES=20
DEF_FLOWS=10
DEF_PPS=100
DEF_SPEED=5
DEF_RANGE=1

echo "============================================"
echo " Running SRED Wired & Wireless Experiments"
echo "============================================"

run_sim() {
    local type=$1
    local script=$2
    local nodes=$3
    local flows=$4
    local pps=$5
    local varParam=$6
    local outfile=$7
    local paramName=$8

    echo "  ${type}: ${paramName}=${varParam} ..."
    
    # Run based on type
    if [ "$type" == "wireless" ]; then
        cd "${NS3_DIR}" && ./ns3 run "scratch/sred-802154-sim" -- --nNodes=$nodes --nFlows=$flows --packetsPerSec=$pps --speed=$varParam --simTime=${SIM_TIME} 2>/dev/null | tail -1 >> "$outfile"
    else
        cd "${NS3_DIR}" && ./ns3 run "scratch/sred-wired-sim" -- --nNodes=$nodes --nFlows=$flows --packetsPerSec=$pps --rangeScale=$varParam --simTime=${SIM_TIME} 2>/dev/null | tail -1 >> "$outfile"
    fi
}

# Experiments List
# 1. Vary Nodes
# 2. Vary Flows
# 3. Vary PPS
# 4. Vary Speed (Wireless) / Range (Wired)

types=("wireless" "wired")

for type in "${types[@]}"; do
    echo "--- Processing ${type} ---"
    
    # Exp 1: Nodes
    OUT="${RESULTS_DIR}/${type}_vary_nodes.csv"
    if [ ! -f "$OUT" ]; then
        echo "${HEADER}" > "$OUT"
        for v in 20 40 60 80 100; do
            run_sim "$type" "" "$v" "$DEF_FLOWS" "$DEF_PPS" "$([ "$type" == "wireless" ] && echo $DEF_SPEED || echo $DEF_RANGE)" "$OUT" "nNodes"
        done
    else
        echo "  Skipping Vary Nodes (${OUT} already exists)"
    fi

    # Exp 2: Flows
    OUT="${RESULTS_DIR}/${type}_vary_flows.csv"
    if [ ! -f "$OUT" ]; then
        echo "${HEADER}" > "$OUT"
        for v in 10 20 30 40 50; do
            run_sim "$type" "" "$DEF_NODES" "$v" "$DEF_PPS" "$([ "$type" == "wireless" ] && echo $DEF_SPEED || echo $DEF_RANGE)" "$OUT" "nFlows"
        done
    else
        echo "  Skipping Vary Flows (${OUT} already exists)"
    fi

    # Exp 3: PPS
    OUT="${RESULTS_DIR}/${type}_vary_pps.csv"
    if [ ! -f "$OUT" ]; then
        echo "${HEADER}" > "$OUT"
        for v in 100 200 300 400 500; do
            run_sim "$type" "" "$DEF_NODES" "$DEF_FLOWS" "$v" "$([ "$type" == "wireless" ] && echo $DEF_SPEED || echo $DEF_RANGE)" "$OUT" "PPS"
        done
    else
        echo "  Skipping Vary PPS (${OUT} already exists)"
    fi

    # Exp 4: Speed / Range
    if [ "$type" == "wireless" ]; then
        OUT="${RESULTS_DIR}/${type}_vary_speed.csv"
        if [ ! -f "$OUT" ]; then
            echo "${HEADER}" > "$OUT"
            for v in 5 10 15 20 25; do
                run_sim "$type" "" "$DEF_NODES" "$DEF_FLOWS" "$DEF_PPS" "$v" "$OUT" "speed"
            done
        else
            echo "  Skipping Vary Speed (${OUT} already exists)"
        fi
    else
        OUT="${RESULTS_DIR}/${type}_vary_range.csv"
        if [ ! -f "$OUT" ]; then
            echo "${HEADER}" > "$OUT"
            for v in 1 2 3 4 5; do
                run_sim "$type" "" "$DEF_NODES" "$DEF_FLOWS" "$DEF_PPS" "$v" "$OUT" "range"
            done
        else
            echo "  Skipping Vary Range (${OUT} already exists)"
        fi
    fi
done

echo "============================================"
echo " All experiments complete!"
echo "============================================"
