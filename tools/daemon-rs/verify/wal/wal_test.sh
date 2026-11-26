#!/bin/bash
# WAL test (sfo3 only): the live DB readers (t2r-files, t2r-web, t2r-admin) on a copy of t2r.db while wal_writer.py writes and backs up.
# usage: bash verify/wal/wal_test.sh SRC_DB MODE(wal|delete) SECONDS [BACKUP_EVERY_S]
# Run in a folder with bin/{t2r-files,t2r-web,t2r-admin}; writes wal-<mode>/{writer,readers}.json + the service logs.
set -u
SRC=$1
MODE=$2
SECS=$3
BK=${4:-60}
D=$PWD/wal-$MODE
rm -rf "$D"
mkdir -p "$D/data" "$D/upd"
cp "$SRC" "$D/data/t2r.db"
echo '{"entries": []}' > "$D/upd/news.json"
if [ "$MODE" = wal ]; then
    python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); print('journal', c.execute('PRAGMA journal_mode=WAL').fetchone()[0])" "$D/data/t2r.db"
fi
FP=17402
WP=17420
AP=17410
./bin/t2r-files --listen 127.0.0.1:$FP --data "$D/upd" --db "$D/data/t2r.db" --status-file "$D/data/t2r-status.json" --daemon-log-dir "$D" > "$D/files.log" 2>&1 &
P1=$!
./bin/t2r-web --db "$D/data/t2r.db" --news "$D/upd/news.json" --host 127.0.0.1 --port $WP > "$D/web.log" 2>&1 &
P2=$!
./bin/t2r-admin --db "$D/data/t2r.db" --port $AP --bind 127.0.0.1 --update-data "$D/upd" --reports "$D/upd" --daemon 127.0.0.1:1 > "$D/admin.log" 2>&1 &
P3=$!
sleep 3
python3 "$(dirname "$0")/wal_writer.py" "$D/data/t2r.db" "$SECS" "$MODE" "$BK" "$D/writer.json" &
PW=$!
python3 "$(dirname "$0")/wal_readers.py" "$D/data/t2r.db" "$SECS" "$D/readers.json" $FP $WP $AP
wait $PW
kill $P1 $P2 $P3 2>/dev/null
wait 2>/dev/null
ls -la "$D/data" > "$D/files.txt"
echo "== writer ($MODE)"
cat "$D/writer.json"
echo "== readers ($MODE)"
cat "$D/readers.json"
