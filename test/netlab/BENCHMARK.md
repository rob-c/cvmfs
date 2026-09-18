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

| arm    | runs | switched to DIRECT | never switched |
|--------|-----:|-------------------:|---------------:|
| stock  |   19 |             **19** |              0 |
| branch |   19 |              **0** |             19 |

Deterministic, at every loss level including 0%, and **in both configs** --
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
    LAB=/var/tmp/cvmfs-losslab ./analyse-ab.py

where `<tree>` is a source tree containing `build/cvmfs/`.  Labels beginning
`ab-<arm>-<cfg>-<loss>` are what `analyse-ab.py` tabulates.
