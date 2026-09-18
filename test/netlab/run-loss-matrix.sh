#!/bin/bash
# usage: run.sh <loss-fraction> <label>
HERE=$(cd "$(dirname "$0")" && pwd)
LAB=${LAB:-/var/tmp/cvmfs-losslab}
L=$LAB; REPO=grid.cern.ch
LOSS=$1; LABEL=$2
OUT=$L/log/$LABEL; mkdir -p $OUT

fusermount -u $L/mnt 2>/dev/null; sleep 1
pkill -x lossrelay.py 2>/dev/null
kill $(cat $L/relay.pid 2>/dev/null) 2>/dev/null; sleep 1
rm -rf $L/cache; mkdir -p $L/cache
: > $L/log/cvmfs.log            # proxy transitions land here, not in ss

python3 $HERE/lossrelay.py --loss $LOSS --listen 3129 \
    --target 194.81.255.225:3128 > $OUT/relay.log 2>&1 &
echo $! > $L/relay.pid
sleep 1

MSTART=$(date +%s.%N)
cvmfs2 -o config=$L/cvmfs.conf,allow_other $REPO $L/mnt > $OUT/mount.log 2>&1
MRC=$?
MEND=$(date +%s.%N)
echo "mount_rc=$MRC mount_secs=$(awk "BEGIN{printf \"%.2f\", $MEND-$MSTART}")" | tee $OUT/summary

if [ $MRC -ne 0 ]; then echo "MOUNT FAILED"; cat $OUT/mount.log; exit 1; fi

# Workload: parallel walk + read.  Deliberately cold cache each run.
WSTART=$(date +%s.%N)
find $L/mnt -maxdepth 4 -type f 2>$OUT/find.err | head -400 > $OUT/files
NFILES=$(wc -l < $OUT/files)
ERRS=0; BYTES=0; WPIDS=""
for i in $(seq 1 8); do
  ( while read -r f; do
      if ! sz=$(cat "$f" 2>/dev/null | wc -c); then echo "ERR $f"; fi
      echo "$sz"
    done < <(awk "NR%8==$((i-1))" $OUT/files) > $OUT/w$i.out 2>&1 ) &
  WPIDS="$WPIDS $!"
done
wait $WPIDS
WEND=$(date +%s.%N)
BYTES=$(cat $OUT/w*.out | grep -E '^[0-9]+$' | paste -sd+ | python3 -c 'import sys;print(eval(sys.stdin.read().strip() or "0"))')
ERRS=$(cat $OUT/w*.out | grep -c '^ERR' || true)
WSECS=$(awk "BEGIN{printf \"%.2f\", $WEND-$WSTART}")

{
echo "files=$NFILES bytes=${BYTES:-0} errors=$ERRS walk_read_secs=$WSECS"
echo "--- cvmfs_talk counters ---"
cvmfs_talk -p $L/cache/$REPO/cvmfs_io.$REPO internal affairs 2>/dev/null \
  | grep -Ei 'download\.(sz_transferred_bytes|n_requests|n_retries|n_proxy_failover|n_host_failover)|download\.sz_transfer_time' 
echo "--- proxy in use ---"
cvmfs_talk -p $L/cache/$REPO/cvmfs_io.$REPO proxy info 2>/dev/null | head -20
} | tee -a $OUT/summary
echo "--- proxy/host transitions (client log) ---"
grep -Ei "switch|proxy|host|fail|timeout" $L/log/cvmfs.log | tail -20 | tee $OUT/transitions
cp $L/log/cvmfs.log $OUT/cvmfs.log 2>/dev/null
tail -3 $OUT/relay.log
