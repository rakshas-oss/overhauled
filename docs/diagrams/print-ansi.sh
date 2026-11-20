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
emit "BRK1 / BRK2 → bounded admission → five isolated worker class pools"
emit "${cyan}Control${reset} FIFO/CPU · ${violet}RealtimeSpatial${reset} EDF with deadline, else FIFO"
emit "${gold}Throughput${reset} FIFO · ${green}ExactCpu${reset} FIFO/forced CPU · Inference FIFO"
emit "${orange}Linux NUMA${reset} sysfs node cpulists ∩ process CPU mask → best-effort pthread pin"
emit "${green}BACKENDS${reset} CPU baseline · CUDA GPU · optional NPU → BRK2 telemetry fields 7–12"
emit "${dim}64 MiB default in-flight memory budget; worker total <=256. No cross-class"
emit "global order; no hard RT/preemption. Restricted hosts do not claim NUMA pinning."
emit "YuKKi emits hints; Overhauled applies local placement. BRK1 has no worker"
emit "directive or backend identity. Raw TCP has no built-in TLS / authentication.${reset}"
