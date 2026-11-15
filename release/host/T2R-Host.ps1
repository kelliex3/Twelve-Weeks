<#
Twelve Weeks - host a server through playit.gg.

Needs: Python 3 and the playit agent with ONE tunnel of type "TCP+UDP" whose local
address is 127.0.0.1:7001 (see PLAYIT-GUIDE.txt). This script finds that tunnel,
starts the playit agent if needed, starts the server, and prints the address your
friends need (also written to client\server.txt).

  Start Server.bat                 run the server
  Start Server.bat -Play           run the server and also start your own game
  Start Server.bat -Mode interrogation   2013 Interrogation instead of Supply Raid (or -Mode rotate)
  Start Server.bat -NoMapVote      skip the 2013 match flow (map vote -> CHOOSE LOADOUT -> ONE-USE BOOSTER); -MapVote = on (default)
  Start Server.bat -NoHostMigration  turn off host migration (default ON: the match survives the host leaving)
#>
param([switch]$Play, [int]$MatchSize = 8, [int]$MinPlayers = 2, [int]$FillWait = 15, [string]$Map = "rotate", [string]$MapPool = "", [string]$Mode = "supply-raid", [switch]$MapVote, [switch]$NoMapVote, [switch]$HostMigration, [switch]$NoHostMigration)
$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot

function Find-Exe([string[]]$candidates) {
    foreach ($c in $candidates) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        if (Test-Path $c) { return $c }
    }
    return $null
}

function Find-Python {
    foreach ($c in @(@("py", "-3"), @("python"), @("python3"))) {
        $exe = Find-Exe @($c[0])
        if (-not $exe -or $exe -like "*WindowsApps*") { continue }   # skip the Store stub
        $pre = @($c | Select-Object -Skip 1)
        $check = $pre + @("-c", "import sys; print(sys.version_info >= (3, 8))")
        if ((& $exe @check 2>$null) -eq "True") { return [string[]](@($exe) + $pre) }
    }
    return $null
}

function Get-T2RTunnel([string]$playit) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = "Continue"   # native stderr must not throw (Windows PowerShell 5.1)
    try { $json = & $playit tunnels list 2>&1 | Out-String } finally { $ErrorActionPreference = $eap }
    $start = $json.IndexOf("{")
    if ($start -lt 0) {
        $claim = [regex]::Match($json, 'https://playit\.gg/\S+')
        if ($claim.Success) { Start-Process $claim.Value }
        throw ("playit is not set up on this PC yet (the agent is not claimed by your playit account). " +
               "Run 'playit' once, open the claim link it prints (or the one just opened), log in and approve this agent, " +
               "then start the server again. Details: PLAYIT-GUIDE.txt.`n  playit said: " + (($json -replace '\s+', ' ').Trim()))
    }
    $list = $json.Substring($start) | ConvertFrom-Json
    $cands = @($list.tunnels | Where-Object { $_.origin.data.local_port -eq 7001 -or $_.name -match 'T2R|TLOU|tlou' })
    if ($cands.Count -eq 0) { return $null }
    $t = $cands[0]
    $a = $t.alloc.data
    $ip = if ($a.static_ip4) { $a.static_ip4 } else { $a.tunnel_ip }
    return [pscustomobject]@{
        Name = $t.name; Type = $t.port_type; Domain = $a.assigned_domain; Ip = $ip; Port = [int]$a.port_start
        LocalIp = $t.origin.data.local_ip; LocalPort = [int]$t.origin.data.local_port; Status = $t.alloc.status
    }
}

try {
    Write-Host "=== Twelve Weeks - server ===" -ForegroundColor Cyan
    $py = @(Find-Python)
    if (-not $py -or -not $py[0]) { throw "Python 3.8 or newer was not found (the Microsoft Store 'python' stub does not count). Install it from https://www.python.org/downloads/ and tick 'Add python.exe to PATH' in the installer, then close this window and start the server again." }

    $playit = Find-Exe @("playit", "C:\Program Files\playit_gg\bin\playit.exe", "$env:LOCALAPPDATA\playit_gg\playit.exe")
    if (-not $playit) { throw "playit agent not found. Install it from https://playit.gg/download (see PLAYIT-GUIDE.txt)." }

    $tun = Get-T2RTunnel $playit
    if (-not $tun) {
        Write-Host "No T2R tunnel found on your playit account." -ForegroundColor Yellow
        Write-Host "Create one at https://playit.gg/account/tunnels :"
        Write-Host "  Tunnel type: TCP+UDP   Local address: 127.0.0.1   Local port: 7001"
        Start-Process "https://playit.gg/account/tunnels"
        throw "Run this again after creating the tunnel."
    }
    if ($tun.Type -ne "both") {
        Write-Host "WARNING: tunnel '$($tun.Name)' is '$($tun.Type)', not TCP+UDP. Gameplay needs UDP." -ForegroundColor Yellow
    }

    if (-not (Get-Process playit -ErrorAction SilentlyContinue)) {
        Write-Host "Starting the playit agent..."
        Start-Process $playit -ArgumentList "start" -WindowStyle Minimized
    }

    $public = "$($tun.Domain):$($tun.Port)"
    Write-Host ""
    Write-Host "  Server address for players:  $public" -ForegroundColor Green
    Write-Host "  (IP form: $($tun.Ip):$($tun.Port))"
    Write-Host ""
    try { Set-Clipboard $public; Write-Host "  (copied to clipboard)" } catch {}
    try { Set-Content (Join-Path $Here "..\client\server.txt") $public } catch { Write-Host "  (no client folder next to this one: send the address above to your players)" }
    $verFile = Join-Path $Here "version.txt"
    if (Test-Path $verFile) { Write-Host "  Server build: $((Get-Content $verFile -Raw).Trim()) - players need the same build (Play.bat tells them if not)" }

    # The server tells clients where the gameplay relay is (the public UDP address).
    $env:T2R_HOST = if ($tun.LocalIp) { $tun.LocalIp } else { "127.0.0.1" }
    $env:T2R_PORT = "$($tun.LocalPort)"
    $env:T2R_PUB_IP = $tun.Ip
    $env:T2R_PUB_UDP_PORT = "$($tun.Port)"
    $env:T2R_MATCH_SIZE = "$MatchSize"
    $env:T2R_MIN_PLAYERS = "$MinPlayers"
    $env:T2R_FILL_WAIT = "$FillWait"
    # One database for every build (clans, stats, boosters survive a new playtest folder).
    $dbDir = Join-Path $env:LOCALAPPDATA "T2R-TwelveWeeks"
    # (data folder name before the product rename: used as it is while the new one does not exist)
    $legacyDbDir = Join-Path $env:LOCALAPPDATA "T2R-Factions2"
    if (-not (Test-Path $dbDir) -and (Test-Path $legacyDbDir)) { $dbDir = $legacyDbDir }
    New-Item -ItemType Directory -Force $dbDir | Out-Null
    $db = Join-Path $dbDir "t2r.db"
    if (-not (Test-Path $db)) {
        # first run of a build with the shared database: every playtest t2r.db found next to this folder is merged in (clans further
        # along win, stats / names kept), so nobody's clan restarts
        $old = @(Get-ChildItem -Path (Join-Path $Here "..\..\..") -Directory -Filter "playtest*" -ErrorAction SilentlyContinue |
                 ForEach-Object { Get-ChildItem -Path $_.FullName -Filter "t2r.db" -Recurse -Depth 3 -ErrorAction SilentlyContinue }) +
               @(Get-ChildItem -Path $Here -Filter "t2r.db" -ErrorAction SilentlyContinue) | Select-Object -ExpandProperty FullName -Unique
        if ($old) {
            $env:T2R_DB = $db
            $pyPre = @($py | Select-Object -Skip 1)
            & $py[0] @pyPre (Join-Path $Here "daemon.py") --merge-db @old
        }
    }
    $env:T2R_DB = $db
    Write-Host "  Database: $db"
    $env:T2R_MAP = $Map
    $env:T2R_MAP_POOL = $MapPool
    $env:T2R_MODE = $Mode
    # SP-section maps the matching client was built with (build_release.py --sp-maps writes sp-maps.txt); the server only offers those
    $spFile = Join-Path $Here "sp-maps.txt"
    $sp = if (Test-Path $spFile) { (Get-Content $spFile -Raw).Trim() } else { "" }
    $env:T2R_SP_MAPS = if ($sp -and $sp -ne "none") { $sp } else { "" }
    # 2013 match flow on by default: map vote -> CHOOSE LOADOUT (5 s) -> ONE-USE BOOSTER (5 s) -> horn -> level
    if (-not $NoMapVote) { $env:T2R_MAP_VOTE = "1" }
    $env:T2R_HOST_MIGRATION = $(if ($NoHostMigration) { "0" } else { "1" })
    # the host comes from the majority region; while the owner (jas) is in a match it is always the EU region (steamid:region, comma list; "" = off)
    if (-not $env:T2R_HOST_REGION_PIN) { $env:T2R_HOST_REGION_PIN = "" }
    if (-not $env:T2R_REQUIRE_VERSION) { $env:T2R_REQUIRE_VERSION = "1" }   # a client that never reports its version (an old launcher) is kept out of matches

    # port busy? (an old server window still open, or another program on that port)
    $busy = $null
    try {
        $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Parse($env:T2R_HOST), [int]$env:T2R_PORT)
        $l.ExclusiveAddressUse = $true
        $l.Start(); $l.Stop()
    } catch { $busy = $_.Exception.Message }
    if ($busy) {
        $who = ""
        try {
            $c = Get-NetTCPConnection -LocalPort ([int]$env:T2R_PORT) -State Listen -ErrorAction Stop | Select-Object -First 1
            $pr = Get-Process -Id $c.OwningProcess -ErrorAction Stop
            $who = " It is used by '$($pr.ProcessName)' (pid $($pr.Id))."
        } catch {}
        throw "Port $($env:T2R_PORT) is already in use on this PC.$who Is another server window still open? Close it (or that program) and start again. ($busy)"
    }

    if ($Play) {
        Start-Process "powershell" -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $Here '..\client\T2R-Play.ps1')`""
    }

    Write-Host "Server running ($MinPlayers-$MatchSize players per match, map $Map). Close this window to stop it." -ForegroundColor Cyan
    Write-Host "Log file: $(Join-Path $Here 'daemon.log')"
    $pyExe = $py[0]; $pyArgs = @($py | Select-Object -Skip 1) + @("-u", (Join-Path $Here "daemon.py"))
    # keep the server up: a crash restarts it (up to 5 times in 10 minutes); exit 3 = port busy, never restarted
    $starts = New-Object System.Collections.Generic.List[datetime]
    while ($true) {
        $starts.Add([datetime]::Now)
        & $pyExe @pyArgs
        $code = $LASTEXITCODE
        if ($code -eq 0) { break }
        if ($code -eq 4) {   # planned restart (admin site: Schedule restart): not a crash, not counted
            Write-Host "Planned restart - starting the server again..." -ForegroundColor Cyan
            $starts.Clear(); Start-Sleep -Seconds 2; continue
        }
        if ($code -eq 3) { throw "Port $($env:T2R_PORT) is already in use (another server still running?). Close it and start again." }
        $recent = @($starts | Where-Object { $_ -gt [datetime]::Now.AddMinutes(-10) }).Count
        $logFile = Join-Path $Here "daemon.log"
        Write-Host "The server stopped unexpectedly (exit code $code). Last log lines:" -ForegroundColor Yellow
        if (Test-Path $logFile) { Get-Content $logFile -Tail 8 | ForEach-Object { Write-Host "  $_" } }
        if ($recent -ge 5) { throw "The server keeps crashing ($recent times in 10 minutes). Send daemon.log (and daemon.log.1) to the developer." }
        Write-Host "Restarting in 3 seconds..." -ForegroundColor Yellow
        Start-Sleep -Seconds 3
    }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}
