#!/bin/bash
# usage: bench.sh <cvmfs2-binary> <config-file> <loss> <label>
HERE=$(cd "$(dirname "$0")" && pwd)
LAB=${LAB:-/var/tmp/cvmfs-losslab}
L=$LAB; REPO=grid.cern.ch
TREE=$1; CONF=$2; LOSS=$3; LABEL=$4; EXTRA=${5:-}
BIN=$TREE/build/cvmfs/cvmfs2
export CVMFS_LIBRARY_PATH=$TREE/build/cvmfs
OUT=$L/log/$LABEL; mkdir -p $OUT

fusermount -u $L/mnt 2>/dev/null; sleep 1
kill $(cat $L/relay.pid 2>/dev/null) 2>/dev/null; sleep 1
rm -rf $L/cache; mkdir -p $L/cache; : > $L/log/cvmfs.log

python3 $HERE/lossrelay.py --loss $LOSS --listen 3129 --report 5 $EXTRA \
    --target 194.81.255.225:3128 > $OUT/relay.log 2>&1 &
echo $! > $L/relay.pid; sleep 1

MS=$(date +%s.%N)
$BIN -o config=$CONF,allow_other $REPO $L/mnt > $OUT/mount.log 2>&1
MRC=$?; ME=$(date +%s.%N)
echo "tree=$TREE" > $OUT/summary
# The loader's --version is its own; only cvmfs_talk reports the library
# that actually got dlopen'd, which is the thing under test.
echo "loader_version=$($BIN --version 2>&1 | head -1 | awk '{print $NF}')" >> $OUT/summary
echo "config=$(grep -o 'CVMFS_HTTP_PROXY=.*' $CONF)" >> $OUT/summary
echo "loss=$LOSS mount_rc=$MRC mount_secs=$(awk "BEGIN{printf \"%.2f\", $ME-$MS}")" >> $OUT/summary
if [ $MRC -ne 0 ]; then echo "MOUNT FAILED" >> $OUT/summary; cat $OUT/summary; exit 1; fi

WS=$(date +%s.%N); WP=""
for i in 1 2 3 4 5 6 7 8; do
  ( while read -r f; do cat "$f" 2>/dev/null | wc -c; done \
      < <(awk "NR%8==$((i-1))" $L/log/baseline/files) > $OUT/w$i.out 2>&1 ) &
  WP="$WP $!"
done
wait $WP
WE=$(date +%s.%N)
CTR=$(cvmfs_talk -p $L/cache/$REPO/cvmfs_io.$REPO internal affairs 2>/dev/null)
VER=$(cvmfs_talk -p $L/cache/$REPO/cvmfs_io.$REPO version 2>/dev/null)
fusermount -u $L/mnt 2>/dev/null; sleep 1
kill $(cat $L/relay.pid) 2>/dev/null; sleep 1
B=$(cat $OUT/w*.out | grep -E '^[0-9]+$' | paste -sd+ | python3 -c 'import sys;print(eval(sys.stdin.read().strip() or "0"))')
Z=$(cat $OUT/w*.out | grep -cE '^0$')
{
 echo "bytes=$B zero_reads=$Z secs=$(awk "BEGIN{printf \"%.2f\", $WE-$WS}")"
 echo "$VER" | sed 's/^/ LOADED: /'
 echo "$CTR" | grep -E 'download\.(n_requests|n_retries|n_proxy_failover|n_host_failover|sz_transferred_bytes)\|'
 echo "direct_switches=$(grep -c 'switching proxy.*to DIRECT' $L/log/cvmfs.log 2>/dev/null)"
 echo "all_switches=$(grep -c 'switching proxy' $L/log/cvmfs.log 2>/dev/null)"
 echo "relay_bytes=$(grep '\[relay\]' $OUT/relay.log | tail -1 | grep -o 'bytes=[0-9]*' | cut -d= -f2)"
 grep -E 'switching proxy' $L/log/cvmfs.log 2>/dev/null | sed 's/^/  LOG: /'
} >> $OUT/summary
cp $L/log/cvmfs.log $OUT/cvmfs.log 2>/dev/null
cat $OUT/summary
