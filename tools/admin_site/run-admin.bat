@echo off
rem Host data folder; the one from before the product rename is used while the new one does not exist (same rule as T2R-Host.ps1)
set "T2RDATA=%LOCALAPPDATA%\T2R-TwelveWeeks"
if not exist "%T2RDATA%" if exist "%LOCALAPPDATA%\T2R-Factions2" set "T2RDATA=%LOCALAPPDATA%\T2R-Factions2"
rem OLD local mode on a stale PC copy of the DB - the real site is on the Pi (run-admin-pi.bat). Kept for offline tests only.
rem Local admin site for the live server (read-only DB, binds 127.0.0.1 only). Open http://127.0.0.1:7010/
rem Update-server data (manifests, bug reports) lives next to the DB: %T2RDATA%\update-data
cd /d "%~dp0..\.."
if not exist "%T2RDATA%\update-data\reports" mkdir "%T2RDATA%\update-data\reports"
python tools\admin_site\admin.py --db "%T2RDATA%\t2r.db" --daemon-log auto --reports "%T2RDATA%\update-data\reports" --update-data "%T2RDATA%\update-data" --port 7010
