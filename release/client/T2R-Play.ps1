<#
Twelve Weeks - client launcher.

Play.bat is the only thing to run. First run: finds the game, installs the mod files and
makes a desktop shortcut (asks only if the game folder can't be found). Every run: reads
the server address from server.txt, points the mod at the server's current IP (playit
addresses can change), picks the player name (in-game custom name > Steam name >
Windows user name) and starts the game.

  Play.bat               install if needed, then play
  Play.bat -Reinstall    find the game again and reinstall the mod files
  Play.bat -SkipCheck    do not check the server / its version before starting (emergencies only)
#>
param([switch]$Reinstall, [switch]$SkipCheck)
$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
# Files from a downloaded zip carry the "from the internet" mark; SmartScreen then asks before t2r-steam.exe / t2r-slim.exe
# start and closing that prompt makes Start-Process fail with "The operation was canceled by the user". Clear the mark.
try { Get-ChildItem $Here -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue } catch {}
$CfgPath = Join-Path $Here "t2r-client.json"

# ---- t2r-play.log: every step of every start is written here (send this file when the game does not start)
$LogPath = Join-Path $Here "t2r-play.log"
try { if ((Test-Path $LogPath) -and (Get-Item $LogPath).Length -gt 524288) { Move-Item $LogPath "$LogPath.old" -Force } } catch {}
function Write-Log([string]$msg) {
    try { Add-Content -Path $LogPath -Value ("{0:yyyy-MM-dd HH:mm:ss.fff} {1}" -f (Get-Date), $msg) -Encoding UTF8 } catch {}
}
$ExpectedGameVersion = "1.6.10721.0105"      # the tlou-ii.exe build winmm.dll / t2r-ext.dll are patched for
$script:LaunchProblems = New-Object System.Collections.Generic.List[string]   # collected facts, used if the game exits early
function Get-Sha([string]$p) { try { (Get-FileHash $p -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower() } catch { "?" } }
function Describe-File([string]$p) {
    if (-not (Test-Path $p)) { return "MISSING" }
    $i = Get-Item $p
    # winmm.dll is ~850 MB and this is logged several times per start: hashing it each time cost seconds (more on a hard disk); size + time identify it
    $sha = if ($i.Length -gt 100MB) { "(not hashed, large)" } else { Get-Sha $p }
    return ("{0} bytes, modified {1:yyyy-MM-dd HH:mm:ss}, sha256 {2}" -f $i.Length, $i.LastWriteTime, $sha)
}
Write-Log "==== T2R-Play start ===="
Write-Log ("script folder: $Here | PowerShell $($PSVersionTable.PSVersion) | $([Environment]::OSVersion.VersionString) | user $env:USERNAME | args: Reinstall=$Reinstall SkipCheck=$SkipCheck")
try { Write-Log ("client version.txt: " + (Get-Content (Join-Path $Here "version.txt") -Raw -ErrorAction Stop).Trim()) } catch { Write-Log "client version.txt: missing" }

# winmm.dll patch sites (see tools/patch_winmm.py in the project)
$XorKey = [uint32]0x5A3C9E17
$IpOffsets = @(0x56402, 0x58725)
$PortOffsets = @(0x563F1, 0x58742)

function Find-GameDirs {
    $found = New-Object System.Collections.Generic.List[string]
    $roots = New-Object System.Collections.Generic.List[string]
    try {
        $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw -Encoding UTF8), '"path"\s+"([^"]+)"')) {
                $roots.Add([IO.Path]::Combine(($m.Groups[1].Value -replace '\\\\', '\'), "steamapps\common"))
            }
        }
    } catch {}
    foreach ($d in (Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Free -ne $null })) {
        foreach ($sub in "Games", "Program Files\Epic Games", "Program Files (x86)\Steam\steamapps\common",
                         "SteamLibrary\steamapps\common", "Program Files\Steam\steamapps\common") {
            $roots.Add([IO.Path]::Combine($d.Root, $sub))
        }
    }
    foreach ($r in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path $r)) { continue }
        Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "tlou-ii.exe") } |
            ForEach-Object { if (-not $found.Contains($_.FullName)) { $found.Add($_.FullName) } }
    }
    return $found
}

function Select-GameDir {
    $dirs = @(Find-GameDirs)
    if ($dirs.Count -eq 1) {
        Write-Host "Found the game at '$($dirs[0])'."
        return $dirs[0]
    }
    $named = @($dirs | Where-Object { (Split-Path $_ -Leaf) -match 'Last of Us' })
    if ($named.Count -eq 1) {
        Write-Host "Found the game at '$($named[0])'."
        return $named[0]
    }
    if ($dirs.Count -gt 1) {
        for ($i = 0; $i -lt $dirs.Count; $i++) { Write-Host "  [$($i + 1)] $($dirs[$i])" }
        $n = Read-Host "Which one is The Last of Us Part II? (number, or Enter to browse)"
        if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $dirs.Count) { return $dirs[[int]$n - 1] }
    }
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Select the folder that contains tlou-ii.exe"
    while ($true) {
        if ($dlg.ShowDialog() -ne "OK") { throw "No game folder selected." }
        if (Test-Path (Join-Path $dlg.SelectedPath "tlou-ii.exe")) { return $dlg.SelectedPath }
        Write-Host "tlou-ii.exe is not in that folder, try again." -ForegroundColor Yellow
    }
}

function New-TokenId {
    $rnd = New-Object byte[] 12
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($rnd)
    return -join ($rnd | ForEach-Object { $_.ToString("x2") })
}

function New-NameToken([string]$name, [string]$idHex) {
    # 64 hex chars = hex("T2N:" + name + 0x00 [+ 0x00 pad] + 12-byte player id). The game
    # only accepts hex and the server reads the name out of the token. The id is fixed
    # per install, so the same name always gives the same token (stats stay together).
    $nb = [Text.Encoding]::UTF8.GetBytes($name)
    if ($nb.Count -gt 15) { $nb = [byte[]]$nb[0..14] }
    $raw = New-Object byte[] 32
    [Text.Encoding]::ASCII.GetBytes("T2N:").CopyTo($raw, 0)
    $nb.CopyTo($raw, 4)
    for ($i = 0; $i -lt 12; $i++) { $raw[20 + $i] = [Convert]::ToByte($idHex.Substring($i * 2, 2), 16) }
    return -join ($raw | ForEach-Object { $_.ToString("x2") })
}

function Clean-Name([string]$name) {
    # printable ASCII only, max 15 bytes; the game font and the token both want this
    $n = ($name -replace '[^\x20-\x7E]', '').Trim()
    if ($n.Length -gt 15) { $n = $n.Substring(0, 15).Trim() }
    return $n
}

function Start-SteamHelper($cfg, [string]$server) {
    # Starts the Spacewar helper and returns the Steam persona name, or $null if Steam is
    # closed. The helper stays running while the game runs: Steam shows "Playing Spacewar",
    # and when everyone in a match has Steam open, gameplay goes peer-to-peer through Steam
    # (the helper is the game's local relay on 127.0.0.1:7777) instead of via the server.
    $exe = Join-Path $Here "steam\t2r-steam.exe"
    if (-not (Test-Path $exe)) { return $null }
    Get-Process t2r-steam -ErrorAction SilentlyContinue | Stop-Process -Force
    $out = Join-Path $env:TEMP "t2r-steam-name.txt"
    Remove-Item $out -ErrorAction SilentlyContinue
    $tok = Join-Path $cfg.game_dir "tlou-ii-mp.json"
    $log = Join-Path $env:TEMP "t2r-steam.log"
    $p = Start-Process $exe -ArgumentList "`"$out`"", "tlou-ii.exe", "--server", $server,
        "--token", "`"$tok`"", "--port", "7777", "--log", "`"$log`"" -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 40 -and -not (Test-Path $out) -and -not $p.HasExited; $i++) { Start-Sleep -Milliseconds 250 }
    Start-Sleep -Milliseconds 200
    if (-not (Test-Path $out)) { return $null }
    $lines = [IO.File]::ReadAllLines($out, [Text.Encoding]::UTF8)
    if ($lines.Count -lt 1) { return $null }
    $n = Clean-Name $lines[0]
    if ($n) { return $n } else { return $null }
}

function Write-Token($cfg, [string]$name) {
    $tokPath = Join-Path $cfg.game_dir "tlou-ii-mp.json"
    [IO.File]::WriteAllText($tokPath, (@{ token = (New-NameToken $name $cfg.token_id) } | ConvertTo-Json -Compress))
}

function Get-TokenName([string]$token) {
    if ($token -notmatch '^[0-9a-fA-F]{64}$') { return $null }
    [byte[]]$b = for ($i = 0; $i -lt 64; $i += 2) { [Convert]::ToByte($token.Substring($i, 2), 16) }
    if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne "T2N:") { return $null }
    $end = [Array]::IndexOf($b, [byte]0, 4)
    if ($end -lt 0) { return $null }
    return [Text.Encoding]::UTF8.GetString($b, 4, $end - 4)
}

function Resolve-Server([string]$server) {
    $server = ($server -replace '^\s*[a-zA-Z]+://', '').Trim().TrimEnd('/')
    if ($server -notmatch '^([^:\s]+):(\d+)$') {
        throw "The server address '$server' is wrong. It must look like name:port (for example abc-123.joinmc.link:12345). Ask your host for the address and put it into server.txt next to Play.bat."
    }
    $hostPart, $port = $Matches[1], [int]$Matches[2]
    if ($port -lt 1 -or $port -gt 65535) { throw "The server port $port in '$server' is not valid (1-65535). Ask your host for the address again." }
    $ip = $null
    if ([Net.IPAddress]::TryParse($hostPart, [ref]$ip)) { return @($ip.ToString(), $port) }
    try {
        $addr = [Net.Dns]::GetHostAddresses($hostPart) | Where-Object { $_.AddressFamily -eq "InterNetwork" } | Select-Object -First 1
    } catch { $addr = $null }
    if (-not $addr) { throw "The server name '$hostPart' was not found. Check the address in server.txt for typos (ask your host), and that your internet connection works." }
    return @($addr.ToString(), $port)
}

function Test-Server([string]$ip, [int]$port, [string]$token, [string]$server) {
    # Before the game starts: can we reach the server, and is it the same build as this client? Throws a plain
    # message otherwise. An older server that does not know the version question just does not answer: carry on.
    $tcp = New-Object Net.Sockets.TcpClient
    try {
        $iar = $tcp.BeginConnect($ip, $port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(6000)) { throw "timeout" }
        $tcp.EndConnect($iar)
    } catch {
        $tcp.Close()
        throw "Cannot reach the server at $server. Is the host's server window open and running? Is the address right (it changes if the host re-created the tunnel)? Ask your host, then try again."
    }
    try {
        $verFile = Join-Path $Here "version.txt"
        $ver = if (Test-Path $verFile) { (Get-Content $verFile -Raw).Trim() } else { "" }
        if (-not $ver -or $ver -notmatch '^[A-Za-z0-9._+-]{1,40}$') { return }
        $payload = [byte[]](@([byte]$ver.Length) + [Text.Encoding]::ASCII.GetBytes($ver) + [Text.Encoding]::ASCII.GetBytes($token))
        $frame = [byte[]](@([byte](($payload.Length + 1) -band 0xFF), [byte](($payload.Length + 1) -shr 8), [byte]0xB0) + $payload)
        $ns = $tcp.GetStream()
        $ns.ReadTimeout = 4000
        $ns.Write($frame, 0, $frame.Length)
        $buf = New-Object byte[] 1024
        try { $n = $ns.Read($buf, 0, $buf.Length) } catch { $n = 0 }
        if ($n -lt 4 -or $buf[2] -ne 0xB1) {
            Write-Host "(The server did not answer the version check - it is an older server. Continuing.)" -ForegroundColor DarkGray
            return
        }
        $text = [Text.Encoding]::UTF8.GetString($buf, 3, $n - 3)
        if ($text -match 'ok=0') {
            $srv = if ($text -match 'server=(\S+)') { $Matches[1] } else { "?" }
            throw "VERSION MISMATCH: your game build is '$ver' but the server runs '$srv'. Get the current TLOU2 Multiplayer download from your host, unpack it over this folder (or into a new one) and start it with Play.bat."
        }
        Write-Host "Server reachable, same build ($ver)." -ForegroundColor DarkGray
    } finally { $tcp.Close() }
}

function Get-DllTarget([byte[]]$b) {
    $ips = $IpOffsets | ForEach-Object {
        $v = [uint32]([BitConverter]::ToUInt32($b, $_) -bxor $XorKey)
        ([Net.IPAddress]::new([BitConverter]::GetBytes($v))).ToString()
    } | Select-Object -Unique
    $ports = $PortOffsets | ForEach-Object { [BitConverter]::ToUInt16($b, $_) } | Select-Object -Unique
    if (@($ips).Count -ne 1 -or @($ports).Count -ne 1) { throw "winmm.dll is not a T2R client build." }
    return @($ips, $ports)
}

function Set-DllTarget([string]$dll, [string]$ip, [int]$port) {
    $b = [IO.File]::ReadAllBytes($dll)
    $cur = Get-DllTarget $b
    if ($cur[0] -eq $ip -and $cur[1] -eq $port) { return $false }
    $v = [uint32]([BitConverter]::ToUInt32([Net.IPAddress]::Parse($ip).GetAddressBytes(), 0) -bxor $XorKey)
    foreach ($o in $IpOffsets) { [BitConverter]::GetBytes($v).CopyTo($b, $o) }
    foreach ($o in $PortOffsets) { [BitConverter]::GetBytes([uint16]$port).CopyTo($b, $o) }
    [IO.File]::WriteAllBytes($dll, $b)
    return $true
}

function Get-StaleMaps([string]$game) {
    # t2r-maps\** beside winmm.dll: the mod data (main\t2r-data.bin, which winmm.dll needs to start at all) and the Part I map data.
    # Copy-Item keeps the write time, so size + write time tell a current copy from an older build's or a damaged one.
    $root = Join-Path $Here "t2r-maps"
    $stale = @()
    if (-not (Test-Path $root)) { return $stale }
    foreach ($f in Get-ChildItem $root -Recurse -File) {
        $rel = $f.FullName.Substring($root.Length).TrimStart('\')
        $p = Join-Path (Join-Path $game "t2r-maps") $rel
        $g = Get-Item $p -ErrorAction SilentlyContinue
        if (-not $g -or $g.Length -ne $f.Length -or $g.LastWriteTimeUtc -ne $f.LastWriteTimeUtc) { $stale += $rel }
    }
    return $stale
}

function Sync-ModMaps([string]$game) {
    $root = Join-Path $Here "t2r-maps"
    foreach ($rel in Get-StaleMaps $game) {
        $p = Join-Path (Join-Path $game "t2r-maps") $rel
        New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
        Write-Host "Copying t2r-maps\$rel ..." -ForegroundColor Cyan
        Copy-Item (Join-Path $root $rel) $p -Force
        Write-Log ("t2r-maps\$rel copied: " + (Describe-File $p))
    }
}

function Install-Mod($cfg) {
    $game = $cfg.game_dir
    $dst = Join-Path $game "winmm.dll"
    $src = Join-Path $Here "winmm.dll"
    if ((Test-Path $dst) -and -not (Test-Path "$dst.t2r-original")) {
        # keep a foreign winmm.dll (another mod) once; an older T2R build is just replaced
        $isT2R = $false
        try { $null = Get-DllTarget ([IO.File]::ReadAllBytes($dst)); $isT2R = $true } catch {}
        if (-not $isT2R) { Copy-Item $dst "$dst.t2r-original" }
    }
    $slimDat = Join-Path $Here "t2r-slim.dat"
    if (Test-Path $slimDat) {
        # slim download: winmm.dll here lacks the files the game already has; t2r-slim.exe rebuilds the full one from the
        # player's own game files (about a minute, once per version) and checks it against the mod's own checksum
        $slimExe = Join-Path $Here "slim\t2r-slim.exe"
        if (-not (Test-Path $slimExe)) { throw "slim\t2r-slim.exe is missing: unpack the whole download." }
        Write-Host "Preparing the mod from your game files (one time per update, about a minute) ..." -ForegroundColor Cyan
        $slimOut = Join-Path $env:TEMP "t2r-slim.out.txt"
        $slimErr = Join-Path $env:TEMP "t2r-slim.err.txt"
        $sp = Start-Process $slimExe -ArgumentList "`"$src`"", "`"$slimDat`"", "`"$game`"", "`"$dst`"" -Wait -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $slimOut -RedirectStandardError $slimErr
        if ($sp.ExitCode -ne 0) {
            $why = ((Get-Content $slimErr -ErrorAction SilentlyContinue | Select-Object -Last 1) -replace '^t2r-slim: ', '')
            throw "Could not prepare the mod from your game files: $why"
        }
    } else {
        Copy-Item $src $dst -Force
    }
    # feature extension loaded by winmm.dll (crafting, melee, throwables sync, supply boxes)
    $ext = Join-Path $Here "t2r-ext.dll"
    if (Test-Path $ext) { Copy-Item $ext (Join-Path $game "t2r-ext.dll") -Force }
    Write-Log ("installing mod files into $game | shipped winmm.dll: " + (Describe-File $src) + " | shipped t2r-ext.dll: " + (Describe-File $ext))
    New-Item -ItemType Directory -Force (Join-Path $game "tlou-ii-mp") | Out-Null
    Copy-Item (Join-Path $Here "tlou-ii-mp\*") (Join-Path $game "tlou-ii-mp") -Force
    Write-Log ("tlou-ii-mp\: {0} files copied" -f @(Get-ChildItem (Join-Path $game "tlou-ii-mp") -File -ErrorAction SilentlyContinue).Count)
    Sync-ModMaps $game
    $ver = Join-Path $Here "version.txt"
    if (Test-Path $ver) { Copy-Item $ver (Join-Path $game "t2r-version.txt") -Force }
    # UC4 downed animations flag (only in builds that bake the clips in)
    $uc4 = Join-Path $Here "t2r-uc4-downed.txt"
    if (Test-Path $uc4) { Copy-Item $uc4 (Join-Path $game "t2r-uc4-downed.txt") -Force }
    else { Remove-Item (Join-Path $game "t2r-uc4-downed.txt") -Force -ErrorAction SilentlyContinue }
    # the other baked-in-feature flags the build ships (own UC4 crawl, WLF / Seraphite models): copied when shipped, removed when not
    foreach ($flag in @("t2r-uc4-own.txt", "t2r-faction-looks.txt", "t2r-faction-guns.txt", "t2r-melee-duel.txt")) {
        $src = Join-Path $Here $flag
        if (Test-Path $src) { Copy-Item $src (Join-Path $game $flag) -Force }
        else { Remove-Item (Join-Path $game $flag) -Force -ErrorAction SilentlyContinue }
    }
    Write-Token $cfg (Clean-Name $env:USERNAME)
}

function Test-ModCurrent($cfg) {
    # winmm.dll is rewritten with the server address after install, so compare the shipped
    # version stamp instead of file hashes
    $ver = Join-Path $Here "version.txt"
    $installed = Join-Path $cfg.game_dir "t2r-version.txt"
    if (-not (Test-Path (Join-Path $cfg.game_dir "winmm.dll"))) { Write-Log "mod check: winmm.dll missing in the game folder -> install"; return $false }
    $shipExt = Join-Path $Here "t2r-ext.dll"
    $gameExt = Join-Path $cfg.game_dir "t2r-ext.dll"
    if (Test-Path $shipExt) {
        if (-not (Test-Path $gameExt)) { Write-Log "mod check: t2r-ext.dll missing in the game folder (removed by an antivirus?) -> install"; return $false }
        if ((Get-Item $shipExt).Length -ne (Get-Item $gameExt).Length) { Write-Log "mod check: t2r-ext.dll differs from the shipped one -> install"; return $false }
    }
    $stale = @(Get-StaleMaps $cfg.game_dir)
    if ($stale.Count) { Write-Log ("mod check: t2r-maps files missing or not the shipped ones (" + ($stale -join ', ') + ") -> install"); return $false }
    if (-not (Test-Path $ver)) { return $true }
    if (-not (Test-Path $installed)) { return $false }
    return (Get-Content $ver -Raw).Trim() -eq (Get-Content $installed -Raw).Trim()
}

function Test-GameVersion([string]$game) {
    # The mod's code patches fixed offsets of tlou-ii.exe 1.6.10721.0105 (the Steam build). Another build (newer patch, other store,
    # cracked/modified exe) usually dies a moment after the start with no message at all.
    $exe = Join-Path $game "tlou-ii.exe"
    try {
        $vi = (Get-Item $exe).VersionInfo
        $fv = "$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
        $want = [version]"1.6.10721.105"
        $ok = ([version]$fv -eq $want)
        Write-Log ("tlou-ii.exe: {0} | FileVersion '{1}' ProductVersion '{2}' (parsed {3}) | expected {4} -> {5}" -f (Describe-File $exe), $vi.FileVersion, $vi.ProductVersion, $fv, $ExpectedGameVersion, $(if ($ok) { "OK" } else { "MISMATCH" }))
        if (-not $ok) {
            $m = "Your tlou-ii.exe is version $fv but the mod is built for $ExpectedGameVersion."
            $script:LaunchProblems.Add("WRONG GAME VERSION: $m Update/verify the game in Steam (Properties > Installed Files > Verify) so it is exactly the current Steam build, and do not use a modified tlou-ii.exe.")
            Write-Host "WARNING: $m The game will probably close right after starting." -ForegroundColor Yellow
        }
        return $ok
    } catch { Write-Log "tlou-ii.exe version check failed: $($_.Exception.Message)"; return $true }
}

function Confirm-ModFiles($cfg, [string]$when) {
    # After copying (and again just before the start): the files must really be there. An antivirus (Windows Defender included) often
    # removes a freshly copied winmm.dll / t2r-ext.dll a second or two later, and the game then starts without the mod and exits.
    $game = $cfg.game_dir
    $need = @("winmm.dll")
    if (Test-Path (Join-Path $Here "t2r-ext.dll")) { $need += "t2r-ext.dll" }
    if (Test-Path (Join-Path $Here "t2r-maps\main\t2r-data.bin")) { $need += "t2r-maps\main\t2r-data.bin" }
    $missing = @()
    foreach ($f in $need) {
        $p = Join-Path $game $f
        Write-Log ("[$when] $f in game folder: " + (Describe-File $p))
        if (-not (Test-Path $p)) { $missing += $f }
    }
    $shipExt = Join-Path $Here "t2r-ext.dll"
    $gameExt = Join-Path $game "t2r-ext.dll"
    if ((Test-Path $shipExt) -and (Test-Path $gameExt) -and ((Get-Item $shipExt).Length -ne (Get-Item $gameExt).Length)) {
        Write-Log "[$when] t2r-ext.dll size differs from the shipped one (truncated by an antivirus, or copy failed)"
        $missing += "t2r-ext.dll (damaged)"
    }
    $shipData = Join-Path $Here "t2r-maps\main\t2r-data.bin"
    $gameData = Join-Path $game "t2r-maps\main\t2r-data.bin"
    if ((Test-Path $shipData) -and (Test-Path $gameData) -and ((Get-Item $shipData).Length -ne (Get-Item $gameData).Length)) {
        Write-Log "[$when] t2r-maps\main\t2r-data.bin size differs from the shipped one (copy failed or the disk is full)"
        $missing += "t2r-maps\main\t2r-data.bin (damaged)"
    }
    if ($missing.Count) {
        try {
            $thr = Get-MpThreatDetection -ErrorAction Stop | Where-Object { ($_.Resources -join ' ') -match 'winmm|t2r|tlou' } | Select-Object -First 5
            foreach ($t in $thr) { Write-Log ("Defender detection: " + ($t.Resources -join ';') + " at " + $t.InitialDetectionTime) }
        } catch { Write-Log "Defender detection query not available: $($_.Exception.Message)" }
    }
    return $missing
}

function New-Shortcut {
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "TLOU2 Multiplayer.lnk"
    $ws = New-Object -ComObject WScript.Shell
    $s = $ws.CreateShortcut($lnk)
    $s.TargetPath = Join-Path $Here "Play.bat"
    $s.WorkingDirectory = $Here
    $s.Save()
    Write-Host "Desktop shortcut created: $lnk"
}

function Get-ServerAddress($cfg) {
    # server.txt (shipped by the host) wins, so a new address only needs a new server.txt
    $f = Join-Path $Here "server.txt"
    if (Test-Path $f) {
        $s = (Get-Content $f -Raw).Trim()
        if ($s -match '^[^:\s]+:\d+$') { return $s }
    }
    if ($cfg -and $cfg.server) { return $cfg.server }
    throw "No server address: put host:port into $f"
}

function Initialize-Client {
    # First run: find the game, install the mod, make the desktop shortcut. No questions
    # unless the game folder can't be found automatically.
    Write-Host "=== Twelve Weeks - first run ===" -ForegroundColor Cyan
    $old = if (Test-Path $CfgPath) { Get-Content $CfgPath -Raw | ConvertFrom-Json } else { $null }
    $game = if ($old -and $old.game_dir -and (Test-Path (Join-Path $old.game_dir "tlou-ii.exe"))) { $old.game_dir } else { Select-GameDir }
    $tid = if ($old -and $old.token_id) { $old.token_id } else { New-TokenId }
    $custom = if ($old -and $old.custom_name) { $old.custom_name } else { "" }
    $cfg = [pscustomobject]@{ game_dir = $game; token_id = $tid; custom_name = $custom }
    if (Get-Process tlou-ii -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Install-Mod $cfg
    Start-Sleep -Seconds 2
    $null = Confirm-ModFiles $cfg "after first install"
    $cfg | ConvertTo-Json | Set-Content $CfgPath -Encoding UTF8     # (Windows PowerShell 5.1 writes ANSI otherwise: a game folder with non-ANSI letters came back as '?')
    New-Shortcut
    Write-Host "Installed into $game" -ForegroundColor Green
    return $cfg
}

function Get-PlayerName($cfg, [string]$steam) {
    # overwritten name (in-game Change Name, saved as custom_name) > Steam name > Windows user
    # t2r-name.txt next to tlou-ii.exe (written by the in-game OPTIONS > CHANGE NAME button, line 1 = name) wins
    $nf = Join-Path $cfg.game_dir "t2r-name.txt"
    if (Test-Path $nf) {
        $l = Get-Content $nf -TotalCount 1
        $ln = if ($l) { Clean-Name ([string]$l) } else { "" }
        if ($ln) { return @($ln, "custom") }
    }
    if ($cfg.custom_name) { return @((Clean-Name $cfg.custom_name), "custom") }
    if ($steam) { return @($steam, "steam") }
    $n = Clean-Name $env:USERNAME
    if (-not $n) { $n = "Player" }
    return @($n, "windows")
}

function Get-ExitText([int]$code) {
    $u = [uint32]([BitConverter]::ToUInt32([BitConverter]::GetBytes($code), 0))
    switch ($u) {
        0 { return "0 (normal exit, or the process handed over to another one)" }
        3221225781 { return "0xC0000135 = a required DLL was not found (missing Visual C++ runtime or a mod DLL cannot load its dependencies)" }
        3221225595 { return "0xC000007B = bad image (a 32/64-bit DLL mismatch or a damaged mod DLL)" }
        3221225794 { return "0xC0000142 = a DLL failed to initialise" }
        3221225477 { return "0xC0000005 = access violation (crash)" }
        3221226505 { return "0xC0000409 = fast-fail / stack buffer overrun (crash)" }
        default { return ("0x{0:X8}" -f $u) }
    }
}

function Get-EarlyExitReport($cfg, $startedAt, [string]$bootFile, [int]$code, [double]$lived) {
    # The game ended soon after it started: collect what is known and tell the player the likely cause.
    $game = $cfg.game_dir
    $facts = New-Object System.Collections.Generic.List[string]
    $causes = New-Object System.Collections.Generic.List[string]
    foreach ($p in $script:LaunchProblems) { $causes.Add($p) }
    $bootLines = @()
    if (Test-Path $bootFile) { $bootLines = @(Get-Content $bootFile -ErrorAction SilentlyContinue) }
    foreach ($l in $bootLines) { Write-Log "t2r-ext-boot.txt: $l" }
    $extLog = Join-Path $game "t2r-ext.log"
    $extRan = (Test-Path $extLog) -and ((Get-Item $extLog).LastWriteTime -ge $startedAt.AddSeconds(-2))
    if ($extRan) {
        Write-Log "t2r-ext.log was written this run; last lines:"
        Get-Content $extLog -Tail 8 -ErrorAction SilentlyContinue | ForEach-Object { Write-Log "  ext| $_" }
    } else { Write-Log "t2r-ext.log was NOT written this run" }
    $hl = Join-Path $env:TEMP "t2r-steam.log"
    if (Test-Path $hl) { Write-Log "Steam helper log (last lines):"; Get-Content $hl -Tail 6 -ErrorAction SilentlyContinue | ForEach-Object { Write-Log "  steam| $_" } }
    $dumps = @()
    foreach ($d in @($game, (Join-Path $env:LOCALAPPDATA "CrashDumps"), (Join-Path $game "crash"))) {
        if (Test-Path $d) { $dumps += Get-ChildItem $d -Filter *.dmp -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $startedAt.AddSeconds(-2) } }
    }
    foreach ($d in $dumps) { Write-Log "crash dump this run: $($d.FullName) ($($d.Length) bytes)" }
    $fault = $null
    try {
        $ev = Get-WinEvent -FilterHashtable @{ LogName = "Application"; Id = 1000, 1001; StartTime = $startedAt.AddSeconds(-2) } -MaxEvents 20 -ErrorAction Stop |
            Where-Object { $_.Message -match 'tlou-ii' } | Select-Object -First 1
        if ($ev) {
            $fault = ($ev.Message -split "`n" | Where-Object { $_ -match 'Faulting module name|Exception code|Fault offset' }) -join " | "
            Write-Log "Windows error report: $fault"
        }
    } catch { Write-Log "event log query: $($_.Exception.Message)" }
    $vcok = (Test-Path (Join-Path $env:WINDIR "System32\vcruntime140_1.dll")) -and (Test-Path (Join-Path $env:WINDIR "System32\vcruntime140.dll"))
    Write-Log "Visual C++ 2015-2022 x64 runtime files present: $vcok"
    $steamUp = [bool](Get-Process steam -ErrorAction SilentlyContinue)
    Write-Log "Steam running: $steamUp"
    $miss = @(Confirm-ModFiles $cfg "after exit")

    if ($miss.Count) { $causes.Add("ANTIVIRUS: " + ($miss -join ", ") + " vanished from the game folder. Restore it in Windows Security > Protection history and add the game folder and this client folder to the exclusions.") }
    if ($fault -and $fault -match 'winmm\.dll|t2r-ext') { $causes.Add("MOD DLL CRASH: $fault. Send t2r-play.log, t2r-ext.log and t2r-ext-boot.txt from the game folder to your host.") }
    elseif ($fault) { $causes.Add("CRASH REPORTED BY WINDOWS: $fault") }
    if (-not $bootLines.Count) {
        $causes.Add("THE MOD DID NOT LOAD at all (t2r-ext-boot.txt was not written). Either winmm.dll is not being loaded (game files replaced/verified by Steam, another winmm.dll, or the game build differs) or the game died before loading it.")
    } elseif (-not $extRan) {
        $causes.Add("THE MOD LOADED BUT THE GAME CLOSED BEFORE ITS FIRST FRAME (t2r-ext-boot.txt has the load line, no t2r-ext.log). Send t2r-ext-boot.txt to your host.")
    }
    if (-not $vcok) { $causes.Add("MISSING VISUAL C++ RUNTIME: install 'Microsoft Visual C++ Redistributable 2015-2022 x64' (aka.ms/vs/17/release/vc_redist.x64.exe) and try again.") }
    if (-not $steamUp) { $causes.Add("STEAM IS NOT RUNNING: start Steam first (the game needs it).") }
    if ($code -eq -1073741515 -or $code -eq -1073741701) { $causes.Add("EXIT CODE says a DLL could not be loaded: " + (Get-ExitText $code)) }
    if ($dumps.Count) { $causes.Add("A crash dump was written: " + $dumps[0].FullName + " - send it to your host.") }
    if (-not $causes.Count) { $causes.Add("No clear cause found. Check that the game starts without the mod (Steam > Play), then send t2r-play.log to your host.") }
    return $causes
}

function Watch-Game($cfg, $gp, $startedAt, [string]$bootFile) {
    # Watches the game for its first 25 s: an exit in that window is a failed start and is explained. After that it only logs the end of the session.
    $cur = $gp
    $curId = $gp.Id
    while ($true) {
        Start-Sleep -Milliseconds 500
        if ($cur.HasExited) {
            $code = $cur.ExitCode
            $lived = ((Get-Date) - $startedAt).TotalSeconds
            Write-Log ("game process pid $curId ended: exit code {0} after {1:N1} s" -f (Get-ExitText $code), $lived)
            # the exe may hand over to a second process (store wrapper): follow it
            $next = Get-Process tlou-ii -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $curId } | Select-Object -First 1
            if ($next) { Write-Log "continuing with tlou-ii pid $($next.Id)"; $cur = $next; $curId = $next.Id; $null = $cur.Handle; continue }
            if ($lived -lt 25) {
                $causes = Get-EarlyExitReport $cfg $startedAt $bootFile $code $lived
                Write-Host ""
                Write-Host ("The game closed {0:N0} seconds after it started (exit code: {1})." -f $lived, (Get-ExitText $code)) -ForegroundColor Red
                Write-Host "Likely cause:" -ForegroundColor Red
                foreach ($c in $causes) { Write-Host "  - $c" -ForegroundColor Yellow; Write-Log "likely cause: $c" }
                Write-Host ""
                Write-Host "Full details: $LogPath" -ForegroundColor Gray
                throw "The game did not stay open."
            }
            break
        }
        if (((Get-Date) - $startedAt).TotalSeconds -ge 25 -and -not $script:ReportedUp) {
            $script:ReportedUp = $true
            Write-Log "game is up after 25 s (pid $curId); the mod boot trail:"
            if (Test-Path $bootFile) { Get-Content $bootFile -ErrorAction SilentlyContinue | ForEach-Object { Write-Log "  boot| $_" } } else { Write-Log "  boot| (no t2r-ext-boot.txt: the mod is not loaded)" }
            Write-Host "The game is running. This window can be closed." -ForegroundColor DarkGray
        }
    }
    Write-Log "session over"
}

try {
    $fresh = $Reinstall -or -not (Test-Path $CfgPath)
    $cfg = if ($fresh) { Initialize-Client } else { Get-Content $CfgPath -Raw | ConvertFrom-Json }
    if (-not $cfg.token_id -or -not (Test-Path (Join-Path $cfg.game_dir "tlou-ii.exe"))) {
        $cfg = Initialize-Client                            # old config format or game moved
    }
    # A crashed or closing game can leave a tlou-ii process without a window behind for a while
    # ("already running" although nothing is open): give it a few seconds, then end leftovers
    # that still have no window. Only a game with a window counts as running.
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Process tlou-ii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 }) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    Get-Process tlou-ii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 } | ForEach-Object {
        Write-Host "Ending a leftover game process (pid $($_.Id), no window)." -ForegroundColor Yellow
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
    $dll = Join-Path $cfg.game_dir "winmm.dll"
    Write-Log "game folder: $($cfg.game_dir)"
    $null = Test-GameVersion $cfg.game_dir
    if (-not (Test-ModCurrent $cfg)) {
        if (Get-Process tlou-ii -ErrorAction SilentlyContinue) { throw "Close the game first (the mod needs updating)." }
        Install-Mod $cfg
        Write-Host "Mod files updated." -ForegroundColor Green
        Write-Log "mod files updated"
    }
    # Checked before the Steam helper starts: a game started some other way (Steam library,
    # an old shortcut, another install) logs in with a different token than the helper,
    # so it would show up as PLAYER and never get Steam P2P.
    if (Get-Process tlou-ii -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }) {
        throw "The game is already running. Close it and start it only with the 'TLOU2 Multiplayer' shortcut (Play.bat)."
    }
    $server = Get-ServerAddress $cfg
    $ip, $port = Resolve-Server $server

    # the Steam helper is optional (names + P2P): if Windows blocks it, play without it
    try { $steamName = Start-SteamHelper $cfg "$ip`:$port" }
    catch { $steamName = $null; Write-Host "Steam helper could not start ($($_.Exception.Message)) - playing without it." -ForegroundColor Yellow }
    $playName, $source = Get-PlayerName $cfg $steamName
    Write-Token $cfg $playName
    if ($source -eq "steam") { Write-Host "Steam is running: playing as '$playName' (Steam shows Spacewar)." }
    if (Set-DllTarget $dll $ip $port) { Write-Host "Server address updated: $server -> $ip`:$port" }
    if (-not $SkipCheck) {
        try { Test-Server $ip $port (New-NameToken $playName $cfg.token_id) $server }
        catch {
            Get-Process t2r-steam -ErrorAction SilentlyContinue | Stop-Process -Force    # nothing will run: do not leave the helper behind
            throw
        }
    }

    # Skip the game's Play launcher; the mod boots straight to its own menu.
    $k = "HKCU:\Software\Naughty Dog\The Last of Us Part II"
    if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
    Set-ItemProperty $k ShowLauncher 0 -Type DWord

    # The mod files must still be there (an antivirus may have removed them since the copy): one reinstall, then a clear message
    Start-Sleep -Milliseconds 1500
    $miss = @(Confirm-ModFiles $cfg "before start")
    if ($miss.Count) {
        Write-Log ("mod files missing before start: " + ($miss -join ", ") + " - reinstalling once")
        Install-Mod $cfg
        Set-DllTarget $dll $ip $port | Out-Null
        Start-Sleep -Seconds 3
        $miss = @(Confirm-ModFiles $cfg "after reinstall")
        if ($miss.Count) {
            Get-Process t2r-steam -ErrorAction SilentlyContinue | Stop-Process -Force
            throw ("Your antivirus removed the mod files (" + ($miss -join ", ") + ") from the game folder '$($cfg.game_dir)' right after they were copied. " +
                "Without them the game starts without the mod and closes. Open Windows Security > Virus & threat protection > Protection history, restore the item, " +
                "and add the game folder AND this client folder to the exclusions (Manage settings > Exclusions), then start Play.bat again.")
        }
    }

    $exePath = Join-Path $cfg.game_dir "tlou-ii.exe"
    $bootFile = Join-Path $cfg.game_dir "t2r-ext-boot.txt"
    Remove-Item $bootFile -ErrorAction SilentlyContinue
    Write-Host "Starting TLOU2 Multiplayer as '$playName' on $server ..." -ForegroundColor Green
    Write-Log "starting $exePath as '$playName' ($source) server $server -> $ip`:$port, steam helper name: $steamName"
    $startedAt = Get-Date
    try { $gp = Start-Process $exePath -WorkingDirectory $cfg.game_dir -PassThru }
    catch {
        Write-Log "Start-Process failed: $($_.Exception.Message) (native error $($_.Exception.NativeErrorCode))"
        if ($_.Exception.NativeErrorCode -eq 1223) {
            throw "Windows stopped tlou-ii.exe from starting (a Run as administrator / security prompt was closed). Right-click tlou-ii.exe > Properties > Compatibility and untick 'Run this program as an administrator', then start Play.bat again."
        }
        throw
    }
    $null = $gp.Handle                      # keeps the handle so ExitCode can be read
    Write-Log "game process started: pid $($gp.Id)"
    Watch-Game $cfg $gp $startedAt $bootFile
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Log ("ERROR: $($_.Exception.Message) | at " + ($_.ScriptStackTrace -replace "`r?`n", " <- "))
    Write-Host "Details were written to $LogPath - send that file to your host if you need help." -ForegroundColor Gray
    Read-Host "Press Enter to close"
    exit 3
}
