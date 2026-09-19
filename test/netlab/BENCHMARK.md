# Stock vs this branch, under emulated loss

Run on t3-mw2.ph.ed.ac.uk, 2026-09-18/19.

Both arms were built from source with **identical cmake flags** (only the
version define differs), in separate git worktrees:

| arm    | commit   | library version reported at runtime                  |
|--------|----------|------------------------------------------------------|
| stock  | 75bbf60  | `2.15.0~dev1.0.20260907114345.git75bbf605cb`         |
| branch | 8ca2512  | `2.15.0~dev1.0.20260918224305.git8ca2512429`         |

`75bbf60` is the `devel_tip` tag, i.e. the upstream base this branch forked
from.  It genuinely lacks every change here: the stock tree has no
`chunk_readahead.h` and zero matches for `DemoteDirect`, `peer_unresponsive` or
`IsEscalatedProxyGroup`, while the branch library contains all three.

## Selecting the arm correctly

`cvmfs2` is only a loader; it `dlopen`s `libcvmfs_fuse3.so`.  Running the stock
`cvmfs2` binary is **not** enough — without `CVMFS_LIBRARY_PATH` it finds the
*installed* library and silently runs whatever is in `/usr/lib64`.  The harness
therefore exports `CVMFS_LIBRARY_PATH=<tree>/build/cvmfs`, and records what was
actually loaded with `cvmfs_talk version`, which reports the Fuse module and the
loader separately.  Note that `cvmfs2 --version` prints the *loader's*
compiled-in string and proves nothing about the library.

## Workload

Private mount (production mounts untouched), cold cache each run, 400 files
from grid.cern.ch read by 8 parallel workers through `lossrelay.py`.  Two
configs: proxy-only (the host's real `default.local` setting) and
`proxy;DIRECT`.

## Correctness: no difference

Across **38 runs** (19 per arm, 0/25/50% loss, both configs) every single run
returned the identical payload, 89,627,462 bytes, with no zero-length reads.
Stock and this branch are both correct at every loss level tested.

## Throughput: no difference

Replicated where it mattered, because single samples disagreed with each other:

| condition            | arm    | n | mount (mean, range)   | read (mean, range)    |
|----------------------|--------|---|-----------------------|-----------------------|
| 0% loss, proxy-only  | stock  | 5 | --                    | 1.61s [1.45-1.72]     |
| 0% loss, proxy-only  | branch | 5 | --                    | 1.57s [1.52-1.63]     |
| 50% loss, proxy-only | stock  | 4 | 39.3s [34.0-43.4]     | 35.5s [29.7-45.6]     |
| 50% loss, proxy-only | branch | 4 | 36.9s [31.3-43.2]     | 38.1s [33.1-44.9]     |
| 50% loss, ;DIRECT    | stock  | 3 | 36.3s [32.9-39.4]     | 34.7s [29.7-38.5]     |
| 50% loss, ;DIRECT    | branch | 3 | 45.5s [39.8-54.5]     | 43.6s [37.1-55.4]     |

Ranges overlap in every comparison and the sign of the difference flips between
conditions, so **no throughput difference is supportable from this data**.  The
branch costs nothing measurable when the network is healthy, and neither arm is
reliably faster under loss.  Anyone wanting to claim a difference at 50% in the
`;DIRECT` config specifically needs more than n=3.

## The one reproducible difference

Broken down by loss level (`analyse-bylevel.py`):

| loss | arm    | runs | switched to DIRECT | never | % of runs | proxy-only cfg | `;DIRECT` cfg |
|-----:|--------|-----:|-------------------:|------:|----------:|---------------:|--------------:|
|   0% | stock  |    9 |              **9** |     0 |      100% |            7/7 |           2/2 |
|   0% | branch |    8 |              **0** |     8 |        0% |            0/7 |           0/1 |
|  25% | stock  |    2 |              **2** |     0 |      100% |            1/1 |           1/1 |
|  25% | branch |    2 |              **0** |     2 |        0% |            0/1 |           0/1 |
|  50% | stock  |    8 |              **8** |     0 |      100% |            5/5 |           3/3 |
|  50% | branch |    9 |              **0** |     9 |        0% |            0/5 |           0/4 |
| **all** | **stock**  | **19** | **19 (100%)** | **0** | | **13/13** | **6/6** |
| **all** | **branch** | **19** |  **0 (0%)**   | **19**| |  **0/13** | **0/6** |

Supporting metrics for the same runs.  Ranges matter more than means here: they
overlap at every level, which is why no throughput difference is claimed.

| loss | arm    | runs | mount s (mean [range])  | read s (mean [range])   | payload | retries | proxy failovers | off-proxy bytes |
|-----:|--------|-----:|-------------------------|-------------------------|---------|--------:|----------------:|----------------:|
|   0% | stock  |    9 | 0.21 [0.20-0.22]        | 1.59 [1.45-1.73]        | all OK  |       0 |               0 |        0 (none) |
|   0% | branch |    8 | 0.21 [0.20-0.22]        | 1.66 [1.52-2.17]        | all OK  |       0 |               0 |        0 (none) |
|  25% | stock  |    2 | 5.43 [4.93-5.93]        | 8.62 [7.37-9.88]        | all OK  |       0 |               0 |        0 (none) |
|  25% | branch |    2 | 5.20 [4.37-6.03]        | 7.06 [6.81-7.31]        | all OK  |       0 |               0 |        0 (none) |
|  50% | stock  |    8 | 38.99 [32.87-46.07]     | 35.13 [29.67-45.64]     | all OK  |       4 |               0 |        0 (none) |
|  50% | branch |    9 | 41.68 [31.34-54.49]     | 38.69 [29.82-55.37]     | all OK  |       7 |               0 |        0 (none) |

"off-proxy bytes" is `sz_transferred_bytes` minus the relay's byte count, floored
at zero: the raw figure is about -339,000 in every run because the relay also
counts HTTP headers and both directions, which is the signature of *everything*
having gone through the proxy.

Run counts are uneven because the replication was targeted where single samples
disagreed (n=5 at 0% proxy-only, n=4 at 50% proxy-only, n=3 at 50% `;DIRECT`),
and 25% was only ever run once per config.  The DIRECT result needs no
replication: it is 19/19 versus 0/19 with no variance at all.

The switch is deterministic at every loss level including 0%, and **in both
configs** --
including proxy-only, where the config names no DIRECT fallback at all.  Stock
logs:

    (manager 'external') switching proxy from http://127.0.0.1:3129 to DIRECT.
      Reason: set random start proxy from the first proxy group

The branch logs instead:

    (manager 'external') switching proxy from (none) to http://127.0.0.1:3129.
      Reason: cloned

This is the `mountpoint.cc` change: the external download manager used to be
constructed with a hardcoded `"DIRECT"` proxy list rather than inheriting the
one the standard manager uses.

### How much this actually leaked here: nothing

Honest qualification.  `sz_transferred_bytes` minus the relay's byte count is
**0 for both arms in every run**, because grid.cern.ch serves no external data,
so the external manager never fetched anything over its DIRECT setting.  The
branch closes a hole that is latent in this repository rather than one that was
actively leaking.  It would bite on a repository using `CVMFS_EXTERNAL_URL`,
where external chunk fetches would go straight out, bypassing gridpp-filer and
the DPI-imposed routing entirely.

## What did NOT reproduce

One stock run (`;DIRECT`, 50% loss) exited **126** after successfully mounting
-- its `mount.log` shows "mounted cvmfs on ...".  126 is not a cvmfs loader exit
code.  Three repeats of that exact cell all succeeded (mount 32.9-39.4s), so
this was a one-off that I cannot attribute to stock, and it should not be read
as a stock failure mode.  Cause unknown.

## Neither arm escalated on slowness alone

`n_proxy_failover` and `n_host_failover` are 0 in all 38 runs, including stock.
Under *emulated* loss the connections still establish and transfers still
complete, so stock's throughput-based escalation is never triggered either.
The 23.8%-unproxied figure measured earlier in this work came from the real DPI
path with real connection failures, which this emulator does not reproduce --
see the caveats in README.md.  So this benchmark demonstrates the external
manager fix, not the `SwitchProxy` gate.

## Reproducing

    LAB=/var/tmp/cvmfs-losslab ./bench-ab.sh <tree> <config> <loss> <label> [relay-args]
    LAB=/var/tmp/cvmfs-losslab ./analyse-ab.py        # per-cell table
    LAB=/var/tmp/cvmfs-losslab ./analyse-bylevel.py   # aggregated by loss level

where `<tree>` is a source tree containing `build/cvmfs/`.  Labels beginning
`ab-<arm>-<cfg>-<loss>` are what `analyse-ab.py` tabulates.
