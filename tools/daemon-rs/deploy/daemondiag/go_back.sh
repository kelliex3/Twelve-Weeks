#!/bin/bash
# Rollback of go.sh: the previous Rust binary again (bin/t2r-daemon-tcp.bak-diag), restart. run-daemon.sh was not changed by go.sh.
set -e
cd $HOME/t2r-live
S=staging/daemonrsdiag
[ -f bin/t2r-daemon-tcp.bak-diag ] || { echo "no bin/t2r-daemon-tcp.bak-diag, stop"; exit 1; }
[ "$(md5sum bin/t2r-daemon-tcp.bak-diag | cut -d' ' -f1)" = "$(cat $S/live-binary.md5)" ] || { echo "bin/t2r-daemon-tcp.bak-diag is not the binary go.sh replaced, stop"; exit 1; }
cp -p bin/t2r-daemon-tcp.bak-diag bin/t2r-daemon-tcp.new
mv -f bin/t2r-daemon-tcp.new bin/t2r-daemon-tcp
sudo systemctl restart t2r-daemon
sleep 25
D=$(cat host-dir)
for p in $(pgrep -u kellie -f "t2r-daemon-tcp serve"); do [ "$(readlink /proc/$p/cwd)" = "$HOME/t2r-live/$D" ] && ps -o pid,etime,pcpu,rss,cmd -p $p; done
echo "binary $(md5sum bin/t2r-daemon-tcp | cut -c1-8)"
systemctl is-active t2r-daemon t2r-relay
tail -n 400 $D/daemon.log | grep -aciE "panic|HANDLER ERR|CONN THREAD ERR" || true
