@echo off
title IoT Air Quality Services (NanoMQ QUIC + Node-RED)
cd /d "%~dp0"

echo ======================================================
echo     MEMULAI LAYANAN IOT: NANOMQ (QUIC) ^& NODE-RED
echo ======================================================

powershell -ExecutionPolicy Bypass -File "%~dp0start_services.ps1"

pause

