set -u
H=$(cd "$(dirname "$0")" && pwd)
B=${ADM_CMP:-/root/adm-cmp}
SRC=${ADM_SRC:?set ADM_SRC to a folder holding read-only copies t2r-live and t2r-test from fra}
BIN=${ADM_BIN:-$H/../target/x86_64-unknown-linux-musl/release/t2r-admin}
export ADM_CMP=$B
stop() { pkill -f fakedaemon.py; pkill -f statusbump.py; pkill -f "admin_site/admin.py"; pkill -f "$BIN"; sleep 0.5; }
stop
rm -rf $B && mkdir -p $B/base
cp -a $SRC/t2r-live $B/base/live
cp -a $SRC/t2r-test $B/base/test
for d in live test; do
  printf 'testsecret\n' > $B/base/$d/data/admin-secret.txt
  printf 'websecret\n' > $B/base/$d/data/web-secret
  printf '# admins\n76561190000000002\n' > $B/base/$d/data/web-admins.txt
  printf '# users\nalice tok123\nbob   tok 456  \n' > $B/base/$d/data/admin-users.txt
done
cp -a $B/base $B/py
cp -a $B/base $B/rs
args() {
  echo "--db $B/$1/live/data/t2r.db --daemon-log auto:$B/$1/live --reports $B/$1/live/update-data/reports --update-data $B/$1/live/update-data --port $2 --bind 127.0.0.1 --tunnel-host admin.twelve-weeks.com --users $B/$1/live/data/admin-users.txt --test-db $B/$1/test/data/t2r.db --test-daemon-log $B/$1/test/host/daemon.log --admin-url http://127.0.0.1:17011 --test-admin-url http://127.0.0.1:17111 --daemon 127.0.0.1:17001 --test-daemon 127.0.0.1:17101"
}
(cd $B/py/live && nohup python3 -u tools/admin_site/admin.py $(args py 17010) > $B/py.out 2>&1 &)
(cd $B/rs/live && nohup $BIN $(args rs 17020) > $B/rs.out 2>&1 &)
mode() {
  pkill -f fakedaemon.py; pkill -f statusbump.py; sleep 0.3
  for s in py rs; do for d in live test; do cp -p $B/base/$d/data/t2r-status.json $B/$s/$d/data/t2r-status.json; done; done
  case "$1" in
    live) nohup python3 -u $H/fakedaemon.py $B > $B/fake.out 2>&1 & ;;
    snapshot) nohup python3 -u $H/statusbump.py $B > /dev/null 2>&1 & ;;
  esac
  sleep 4.5
}
cd $H
for m in live snapshot log; do
  mode $m
  echo "## mode $m"
  python3 compare.py $m
done
mode live
echo "## writes"
python3 writes.py
stop
