# netlab: validating the client against a lossy path to its proxy

The site proxy at `gridpp-filer.ecdf.ed.ac.uk:3128` sits behind a DPI firewall
that mangles and drops traffic unpredictably.  These scripts answer one
question: **does the client stay correct, and stay on the proxy, when the path
to that proxy is losing packets?**

Two implementations, because not every host will let you drop packets.

## `losslab.sh` — real loss, when the kernel allows it

Applies `netem` loss on loopback to a `socat` relay that forwards to the real
proxy, filtered to the relay's port so nothing else on the host is touched:

    cvmfs2 (test) --lo:3129 [netem loss N%]--> socat --> gridpp-filer:3128

    ./losslab.sh check          # are netem / xt_statistic / netns usable?
    ./losslab.sh setup 25       # 25% loss on the relay port only
    ./losslab.sh status
    ./losslab.sh teardown

Run `check` first.  On t3-mw2 it reports all three mechanisms denied:
`sch_netem` will not load (`modprobe` is replaced by a stub that logs and
exits 0), `xt_statistic` is absent, and `user.max_net_namespaces` is 0.  There
is no way to drop an IP packet on that host, which is why the second tool
exists.

## `lossrelay.py` — the client-visible effects of loss, anywhere

A userspace TCP relay that reproduces what loss presents to the application
above TCP, without needing to drop anything:

* **throughput collapse** — TCP's steady state is `MSS/(RTT*sqrt(p))` (Mathis),
  so a loss fraction maps to a per-connection rate cap.  Measured RTT to
  gridpp-filer is 0.77 ms, so 25% loss caps a stream at ~3.79 MB/s.
* **stalls** — a lost retransmission costs an RTO, and RTOs are what trip
  `CVMFS_TIMEOUT` and `CVMFS_LOW_SPEED_LIMIT`.
* **severed connections** — `SO_LINGER 0` close, i.e. a real RST mid-transfer.

        ./lossrelay.py --loss 0.25 --rtt-ms 0.77 --target 194.81.255.225:3128

One knob (`--loss`) drives all three; `--stall-prob` and `--reset-prob` override
individually.  This is **emulation, not literal packet loss**: it does not
reorder, duplicate or corrupt, because TCP would have repaired those below us
anyway.

## Drivers

    ./run-loss-matrix.sh <fraction> <label>   # one cold-cache mount + 400-file read
    ./run-loss-harsh.sh                       # 25% loss + forced mid-transfer RSTs

Both use a **private** mount (`CVMFS_CACHE_BASE` under `$LAB`, default
`/var/tmp/cvmfs-losslab`) pointed at the relay, so the host's production mounts
keep talking to the proxy directly and are unaffected.

## What to look for

The pass conditions are not "it was fast".  They are:

1. byte counts identical to the zero-loss baseline (correctness), and
2. `download.n_proxy_failover` and `download.n_host_failover` both **0**.

(2) is the important one.  A lossy proxy is slow but *reachable*, and the
escalation gate in `download.cc` keys on connectivity rather than throughput
precisely so that this case does not promote DIRECT and leak traffic past the
site proxy.
