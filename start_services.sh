#!/usr/bin/env bash
# ====================================================================
# Script: start_services.sh  (Git Bash di Windows)
# Menjalankan NanoMQ (QUIC, di WSL) + TCP Relay + Node-RED
# Delegasi ke start_services.ps1 agar logika tetap satu tempat.
# ====================================================================
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR" || exit 1

powershell.exe -ExecutionPolicy Bypass -File "$(cygpath -w "$PROJECT_DIR/start_services.ps1" 2>/dev/null || echo start_services.ps1)"
