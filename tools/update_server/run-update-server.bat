@echo off
rem Host data folder; the one from before the product rename is used while the new one does not exist (same rule as T2R-Host.ps1)
set "T2RDATA=%LOCALAPPDATA%\T2R-TwelveWeeks"
if not exist "%T2RDATA%" if exist "%LOCALAPPDATA%\T2R-Factions2" set "T2RDATA=%LOCALAPPDATA%\T2R-Factions2"
rem Update server (launcher updates, /clan.json, /clanfeed.json, bug reports) on port 7002 - needs a playit tunnel to local port 7002.
rem Same data dir the admin site reads: %T2RDATA%\update-data
cd /d "%~dp0..\.."
if not exist "%T2RDATA%\update-data\reports" mkdir "%T2RDATA%\update-data\reports"
python tools\update_server\server.py --data "%T2RDATA%\update-data" --db "%T2RDATA%\t2r.db" --daemon 127.0.0.1:7001 --status-file "%T2RDATA%\t2r-status.json" --port 7002
