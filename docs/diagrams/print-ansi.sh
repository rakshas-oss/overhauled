#!/usr/bin/env bash
set -euo pipefail
# ANSI true-colour architecture sketch for Overhauled v1.2.
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
 cyan=$'\033[38;2;62;224;245m'; violet=$'\033[38;2;190;153;255m'; gold=$'\033[38;2;255;211;116m'; orange=$'\033[38;2;255;170;103m'; green=$'\033[38;2;103;224;169m'; dim=$'\033[38;2;145;166;186m'; bold=$'\033[1m'; reset=$'\033[0m'
else cyan= violet= gold= orange= green= dim= bold= reset=; fi
emit(){ printf '%b\n' "$*"; }
emit "${bold}${cyan}╔══════════════════════════════════════════════════════════════════════════════╗"
emit "║  OVERHAULED / BROKER WORKER FABRIC + EXECUTION                             ║"
emit "╚══════════════════════════════════════════════════════════════════════════════╝${reset}"
emit "BRK1 / BRK2 → bounded admission → class queues sharded by NUMA node"
emit "${cyan}Dedicated pools${reset} Control · RealtimeSpatial · Throughput · ExactCpu · Inference"
emit "${violet}Elastic capacity${reset} RealtimeSpatial / Throughput / Inference only; weighted rotation"
emit "${gold}Elastic ceilings${reset} RT=1 · Throughput=4 · Inference=2 · max 64 elastic / 256 total"
emit "${orange}Node routing${reset} preferred node → logical affinity key → round-robin; worker stays local"
emit "${green}BACKENDS${reset} CPU baseline · CUDA GPU · optional NPU → BRK2 telemetry fields 7–12"
emit "${dim}64 MiB default in-flight memory budget; Control / ExactCpu never borrow."
emit "EDF remains within RealtimeSpatial node queue; no hard RT/preemption; restricted hosts do not claim NUMA pinning."
emit "YuKKi emits hints; Overhauled applies local placement. BRK1 has no worker"
emit "directive or backend identity. Raw TCP has no built-in TLS / authentication.${reset}"
