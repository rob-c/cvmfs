#!/bin/bash
# 25% loss, but with connection death forced up to a level that actually
# exercises teardown and retry: one connection in ten is severed mid-transfer.
HERE=$(cd "$(dirname "$0")" && pwd)
LAB=${LAB:-/var/tmp/cvmfs-losslab}
L=$LAB; REPO=grid.cern.ch; LOSS=${1:-0.25}; LABEL=${2:-loss${LOSS}harsh}
OUT=$L/log/$LABEL; mkdir -p $OUT
fusermount -u $L/mnt 2>/dev/null; sleep 1
kill $(cat $L/relay.pid 2>/dev/null) 2>/dev/null; sleep 1
rm -rf $L/cache; mkdir -p $L/cache
: > $L/log/cvmfs.log            # proxy transitions land here, not in ss
python3 $HERE/lossrelay.py --loss $LOSS --reset-prob 0.35 --stall-prob 0.03 --report 10 \
   --listen 3129 --target 194.81.255.225:3128 > $OUT/relay.log 2>&1 &
echo $! > $L/relay.pid; sleep 1
cvmfs2 -o config=$L/cvmfs.conf,allow_other $REPO $L/mnt > $OUT/mount.log 2>&1
echo "mount_rc=$?" | tee $OUT/summary
S=$(date +%s.%N); WP=""
for i in 1 2 3 4 5 6 7 8; do
 ( while read -r f; do cat "$f" 2>/dev/null | wc -c; done < <(awk "NR%8==$((i-1))" $L/log/baseline/files) > $OUT/w$i.out 2>&1 ) &
 WP="$WP $!"
done
wait $WP
E=$(date +%s.%N)
B=$(cat $OUT/w*.out | grep -E '^[0-9]+$' | paste -sd+ | python3 -c 'import sys;print(eval(sys.stdin.read().strip() or "0"))')
{ echo "bytes=$B secs=$(awk "BEGIN{printf \"%.2f\", $E-$S}")"
  echo "--- counters ---"
  cvmfs_talk -p $L/cache/$REPO/cvmfs_io.$REPO internal affairs 2>/dev/null | grep -Ei 'n_requests|n_retries|failover|n_proxy'
  cvmfs_talk -p $L/cache/$REPO/cvmfs_io.$REPO proxy info 2>/dev/null | head -8
} | tee -a $OUT/summary
echo "--- proxy/host transitions (client log) ---"
grep -Ei "switch|proxy|host|fail|timeout" $L/log/cvmfs.log | tail -20 | tee $OUT/transitions
cp $L/log/cvmfs.log $OUT/cvmfs.log 2>/dev/null
echo "--- proxy/host transitions (client log) ---"
grep -Ei "switch|proxy|host|fail|timeout" $L/log/cvmfs.log | tail -20 | tee -a $OUT/summary
cp $L/log/cvmfs.log $OUT/cvmfs.log 2>/dev/null
tail -3 $OUT/relay.log
fusermount -u $L/mnt 2>/dev/null; kill $(cat $L/relay.pid) 2>/dev/null
echo HARSH_DONE
