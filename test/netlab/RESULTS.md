# Results: t3-mw2.ph.ed.ac.uk, 2026-09-18

Client: 2.15.0~dev1.0.20260918201327.git56031386e9 (this branch).
Path under test: private mount -> relay on 127.0.0.1:3129 -> 194.81.255.225:3128
(gridpp-filer) -> cvmfs-egi.gridpp.rl.ac.uk:8000.  Measured RTT to the proxy:
0.726 / 0.770 / 0.827 ms.  Workload: cold cache, mount, then 400 files from
grid.cern.ch read by 8 parallel workers.  Loss swept 0 / 5 / 10 / 25 / 35 / 50%.

## Real packet loss is not possible on this host

`./losslab.sh check`:

    netem:        UNAVAILABLE (sch_netem will not load)
    xt_statistic: UNAVAILABLE
    netns:        UNAVAILABLE (user.max_net_namespaces=0)

`modprobe` has been replaced with a stub:

    install /bin/sh -c '/usr/bin/logger -t modulejail "blocked: sch_netem"; exit 0'

It returns 0 and loads nothing.  Only `pfifo`, `fq`, `fq_codel` and `blackhole`
qdiscs exist.  So the numbers below come from `lossrelay.py`, which emulates the
client-visible *effects* of loss (rate collapse, stalls, resets) rather than
dropping packets.  **This is emulation, not literal loss** — see README.md.

## Throughput vs emulated loss

All runs moved the identical payload with no read errors:

| loss | mount  | read 400 files | throughput | slowdown | bytes       | errors |
|------|--------|----------------|------------|----------|-------------|--------|
| 0%   |  0.21s |   5.97s        | 15.01 MB/s | x1.0     | 89,627,462  | 0      |
| 5%   |  1.08s |  10.75s        |  8.34 MB/s | x1.8     | 89,627,462  | 0      |
| 10%  |  2.34s |  16.13s        |  5.56 MB/s | x2.7     | 89,627,462  | 0      |
| 25%  |  6.73s |  66.18s        |  1.35 MB/s | x11.1    | 89,627,462  | 0      |
| 35%  | 12.82s | 159.32s        |  0.56 MB/s | x26.7    | 89,627,462  | 0      |
| 50%  | 47.14s | 446.68s        |  0.20 MB/s | x74.8    | 89,627,462  | 0      |

**At 50% loss the client is 75x slower and still completely correct.**  Every
level moved the identical payload; nothing was dropped, truncated or corrupted.
Degradation is continuous, with no cliff: the client gets slow long before it
gets wrong, which is the behaviour you want from a cache client behind a
hostile middlebox.

## The property that actually matters

| loss | n_requests | n_retries | n_proxy_failover | n_host_failover | active proxy |
|------|-----------:|----------:|-----------------:|----------------:|--------------|
| 0%   | 437        | 0         | 0                | 0               | 127.0.0.1:3129 |
| 10%  | 437        | 0         | 0                | 0               | 127.0.0.1:3129 |
| 25%  | 437        | 0         | 0                | 0               | 127.0.0.1:3129 |
| 35%  | 437        | 0         | 0                | 0               | 127.0.0.1:3129 |
| 50%  | 448        | 3         | 0                | 0               | 127.0.0.1:3129 |

Zero failovers at every loss level up to and including 50%.  The client's own
log (`CVMFS_USYSLOG`) records only two proxy transitions in any run, both at
startup:

    (manager 'standard') switching proxy from (none) to http://127.0.0.1:3129
      Reason: set random start proxy from the first proxy group
    (manager 'external') switching proxy from (none) to http://127.0.0.1:3129
      Reason: cloned

No `switching proxy` line ever appears again, at any loss level.  The second
line is worth noting on its own: the *external* download manager inherits the
proxy ("cloned") instead of defaulting to DIRECT, which is the `mountpoint.cc`
change in this branch showing up in the client's own log.

This is the payoff from gating escalation on connectivity rather than
throughput.  A 25%-loss path is slow, but the TCP connection still establishes,
so `peer_unresponsive()` stays false and `SwitchProxy()` does not fire.  Under
the previous throughput-based logic this is exactly the case that would have
promoted DIRECT and leaked traffic straight past gridpp-filer to RAL — or, as
measured earlier in this work, to China, the US and Canada.

## Forced connection death

Loss plus mid-transfer RSTs (`SO_LINGER 0`), `--reset-prob 0.35`.

### 25% loss

    bytes=89,627,462   secs=22.39   errors=0
    n_requests=392  n_retries=1  n_proxy_failover=0  n_host_failover=0
    relay: conns=23  stalls=80  resets=5
    Active proxy: [0] http://127.0.0.1:3129

Five connections were killed mid-transfer.  The client reconnected (23
connections for a workload that needs 5-6 when nothing is being severed),
retried, delivered the full payload, and **never left the proxy**.

### 50% loss

    bytes=89,627,462   secs=38.10   errors=0
    n_requests=396  n_retries=2  n_proxy_failover=0  n_host_failover=0
    relay: conns=125  stalls=83  resets=36
    Active proxy: [0] http://127.0.0.1:3129

**Thirty-six connections killed mid-transfer** and the payload still came back
byte-identical, with two retries and no failover.  125 connections were opened
for a workload that needs 5-6, so the client absorbed each RST by reconnecting
to the same proxy rather than concluding the proxy was dead.

This is the teardown path under maximum stress, and it is the case the earlier
CLOSE-WAIT/LAST-ACK work was aimed at.  No sockets were stranded: the run
completed and unmounted cleanly.

## Isolation

The host's 26 production mounts were checked before, during and after every
run: `NOIOERR=0`, `PROXY=http://194.81.255.225:3128`, `ONLINE=1` throughout.
Afterwards `lo` is back to `qdisc noqueue`, no iptables rules remain, port 3129
is free and no relay or test mount is left behind.

## Caveats

* Emulation, not packet loss.  It does not reorder, duplicate or corrupt; TCP
  would have repaired those below the client anyway.
* The Mathis rate cap is a steady-state model.  It understates the pain of
  loss during slow-start, which is why the mount time (6.73s at 25%) degrades
  faster than the bulk-read throughput.
* At 50% the relay opened 162 connections during the read sweep and 125 in the
  reset run, against 5-7 at low loss.  That is connection churn from stalls and
  RSTs, not an error, but it is the one number that grows non-linearly and is
  worth watching on a real lossy link.
* Only grid.cern.ch was exercised.  It is representative of catalog+chunk
  traffic but is not a large-file repo.
* `run-loss-harsh.sh` reuses the file list from the baseline run rather than
  re-walking, hence 392 requests instead of 437.
