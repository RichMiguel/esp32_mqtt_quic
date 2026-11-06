# ====================================================================
# Script: start_services.ps1
# Menjalankan NanoMQ (MQTT over QUIC, di WSL), TCP Relay, dan Node-RED
#
# Alur:
#   ESP32 --TCP--> Windows 0.0.0.0:1884 (tcp_relay.py)
#         --> 127.0.0.1:1885 (NanoMQ di WSL) --QUIC--> broker.emqx.io:14567
#   Node-RED --> 127.0.0.1:1885 (topik sensor/air_quality/remote)
# ====================================================================

$ScriptDir = Split-Path -Parent -Path $MyInvocation.MyCommand.Definition
Set-Location -Path $ScriptDir

function Test-Port([int]$Port) {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect("127.0.0.1", $Port); $c.Close(); return $true
    } catch { return $false }
}

function Start-Detached([string]$CommandLine) {
    $si = New-CimInstance -ClassName Win32_ProcessStartup -Property @{ ShowWindow = [uint16]7 } -ClientOnly
    Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = $CommandLine
        CurrentDirectory = $ScriptDir
        ProcessStartupInformation = $si
    } | Out-Null
}

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  MEMULAI LAYANAN IOT: NANOMQ (QUIC) + RELAY + NODE-RED" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

if (-not (Test-Path "$ScriptDir\log")) { New-Item -ItemType Directory -Path "$ScriptDir\log" | Out-Null }

# 0. Aktifkan Mobile Hotspot Laptop (2.4 GHz: SSID "raspi", IP 192.168.137.1) agar bebas AP Isolation HP
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction SilentlyContinue
    $connProfile = [Windows.Networking.Connectivity.NetworkInformation,Windows.Networking.Connectivity,ContentType=WindowsRuntime]::GetInternetConnectionProfile()
    if ($connProfile) {
        $tether = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime]::CreateFromConnectionProfile($connProfile)
        if ($tether.TetheringOperationalState -ne 'On') {
            Write-Host "[*] Mengaktifkan Mobile Hotspot Laptop (SSID: raspi | 2.4 GHz)..." -ForegroundColor Green
            $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
            $asTask = $asTaskGeneric.MakeGenericMethod([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
            $netTask = $asTask.Invoke($null, @($tether.StartTetheringAsync()))
            $netTask.Wait(8000) | Out-Null
        }
        Write-Host "[+] Mobile Hotspot Laptop aktif (SSID: raspi | Pass: raspberry | IP: 192.168.137.1)" -ForegroundColor Green
    }
} catch {
    Write-Host "[!] Mobile Hotspot dilewati: $_" -ForegroundColor Yellow
}

# 1. NanoMQ (MsQuic) di WSL
if (Test-Port 1885) {
    Write-Host "[=] NanoMQ sudah berjalan di port 1885" -ForegroundColor Yellow
} else {
    Write-Host "[*] Menjalankan NanoMQ (MQTT over QUIC) di WSL..." -ForegroundColor Green
    $WslDir = $ScriptDir -replace '\\', '/' -replace '^C:', '/mnt/c'
    $CmdNano = "cmd.exe /k title NanoMQ-QUIC && wsl.exe -d Ubuntu-22.04 bash -c `"cd '$WslDir' && exec ./bin/linux/nanomq start --conf nanomq.conf`""
    Start-Detached $CmdNano
    for ($i = 0; $i -lt 15 -and -not (Test-Port 1885); $i++) { Start-Sleep -Seconds 1 }
    if (Test-Port 1885) { Write-Host "[+] NanoMQ aktif (TCP 1885 | QUIC ke EMQX 14567)" -ForegroundColor Green }
    else { Write-Host "[-] NanoMQ gagal start." -ForegroundColor Red }
}

# 2. TCP Relay Windows (0.0.0.0:1884 -> 127.0.0.1:1885)
if (Test-Port 1884) {
    Write-Host "[=] TCP Relay sudah berjalan di port 1884" -ForegroundColor Yellow
} else {
    Write-Host "[*] Menjalankan TCP Relay 0.0.0.0:1884 -> 127.0.0.1:1885..." -ForegroundColor Green
    $CmdRelay = "cmd.exe /k title TCP-Relay-1884 && python `"$ScriptDir\tcp_relay.py`""
    Start-Detached $CmdRelay
    for ($i = 0; $i -lt 10 -and -not (Test-Port 1884); $i++) { Start-Sleep -Seconds 1 }
    if (Test-Port 1884) { Write-Host "[+] TCP Relay aktif" -ForegroundColor Green }
    else { Write-Host "[-] TCP Relay gagal start." -ForegroundColor Red }
}

# 3. Node-RED
if (Test-Port 1880) {
    Write-Host "[=] Node-RED sudah berjalan di port 1880" -ForegroundColor Yellow
} else {
    $NodeRedCmd = (Get-Command "node-red.cmd" -ErrorAction SilentlyContinue).Source
    if ($NodeRedCmd) {
        Write-Host "[*] Menjalankan Node-RED..." -ForegroundColor Green
        Start-Detached "cmd.exe /k title Node-RED && `"$NodeRedCmd`""
        for ($i = 0; $i -lt 15 -and -not (Test-Port 1880); $i++) { Start-Sleep -Seconds 1 }
        if (Test-Port 1880) { Write-Host "[+] Node-RED aktif (http://localhost:1880/ui)" -ForegroundColor Green }
    } else {
        Write-Host "[!] node-red tidak ditemukan di PATH." -ForegroundColor Yellow
    }
}

# 4. Info
$WifiIP = (Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias "Wi-Fi" -ErrorAction SilentlyContinue).IPAddress
Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " ESP32 via Hotspot Laptop (raspi) : 192.168.137.1:1884" -ForegroundColor Yellow
Write-Host " ESP32 via Hotspot HP (Negus)     : ${WifiIP}:1884" -ForegroundColor Yellow
Write-Host " NanoMQ lokal                     : 127.0.0.1:1885 (Node-RED)"
Write-Host " QUIC upstream                    : broker.emqx.io:14567 (UDP)"
Write-Host " Dashboard                        : http://localhost:1880/ui"
Write-Host "======================================================" -ForegroundColor Cyan
