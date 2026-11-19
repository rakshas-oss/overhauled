#!/usr/bin/env bash
# ANSI true-colour architecture sketch for Overhauled.
set -euo pipefail

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  cyan=$'\033[38;2;62;224;245m'; blue=$'\033[38;2;94;184;255m'
  violet=$'\033[38;2;190;153;255m'; gold=$'\033[38;2;255;211;116m'
  orange=$'\033[38;2;255;170;103m'; green=$'\033[38;2;103;224;169m'
  dim=$'\033[38;2;145;166;186m'; bold=$'\033[1m'; reset=$'\033[0m'
else
  cyan= blue= violet= gold= orange= green= dim= bold= reset=
fi
emit() { printf '%b\n' "$*"; }

emit "${bold}${cyan}╔══════════════════════════════════════════════════════════════════════════════╗${reset}"
emit "${bold}${cyan}║  OVERHAULED  /  BROKER + EXECUTION PLANE                                   ║${reset}"
emit "${cyan}╚══════════════════════════════════════════════════════════════════════════════╝${reset}"
emit "${blue} CLIENTS${reset}  YuKKi adapter · external callers · optional direct BRK2 benchmark"
emit "    │  trusted network / independently secured proxy"
emit "${cyan}    ▼${reset}  TCP :9090  ·  4-byte BE length prefix"
emit "${bold}${orange}┌────────────────────────────────────────────────────────────────────────────┐${reset}"
emit "${orange}│  BRK1/v1 compatibility  +  BRK2/v2 TLV envelope                            │${reset}"
emit "${orange}├──────────────────────────────────┬─────────────────────────────────────────┤${reset}"
emit "${orange}│${reset} ${cyan}KIND-SPECIFIC HANDLERS${reset}             ${orange}│${reset} ${violet}TASK CLASSIFICATION${reset}                       ${orange}│${reset}"
emit "${orange}│${reset} NXR1 · MED1 · WSM1 lifecycle       ${orange}│${reset} forced CPU · safety · eligibility   ${orange}│${reset}"
emit "${orange}├──────────────────────────────────┴─────────────────────────────────────────┤${reset}"
emit "${orange}│  BACKEND DISPATCH                                                           │${reset}"
emit "${orange}├──────────────────────┬──────────────────────┬──────────────────────────────┤${reset}"
emit "${orange}│${reset} ${green}CPU BASELINE${reset}          ${orange}│${reset} ${blue}GPU PLACEMENT${reset}        ${orange}│${reset} ${violet}NPU EXECUTOR${reset}                ${orange}│${reset}"
emit "${orange}│${reset} reference / fallback ${orange}│${reset} GpuTopology · Placer ${orange}│${reset} simulator / Ethos-U85 DRM   ${orange}│${reset}"
emit "${orange}└──────────────────────┴──────────────────────┴──────────────────────────────┘${reset}"
emit ""
emit "${bold}${cyan}RESPONSE TELEMETRY${reset}"
emit "${cyan}  BRK2${reset}  field 7 = actual backend  ·  selected_gpu = GPU index or -1"
emit "${gold}  BRK1${reset}  no backend identity field; -1 does not distinguish CPU from NPU"
emit ""
emit "${dim}BOUNDARIES: this is not a Wasm runtime, does not own NUMA discovery or"
emit "NUMA scheduling, and supplies no protocol TLS/authentication. NPU simulator"
emit "is CPU emulation. Physical execution needs target hardware qualification.${reset}"
