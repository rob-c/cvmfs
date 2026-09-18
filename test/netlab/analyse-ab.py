#!/usr/bin/env python3
"""Tabulate the A/B benchmark summaries."""
import glob, os, re, sys

base = sys.argv[1] if len(sys.argv) > 1 else \
    os.environ.get("LAB", "/var/tmp/cvmfs-losslab") + "/log"
rows = []
for d in sorted(glob.glob(os.path.join(base, "ab-*"))):
    f = os.path.join(d, "summary")
    if not os.path.exists(f):
        continue
    t = open(f).read()
    def g(pat, cast=str, default=None):
        m = re.search(pat, t, re.M)
        return cast(m.group(1)) if m else default
    name = os.path.basename(d)
    _, arm, cfg, loss = name.split("-", 3)
    rows.append(dict(failed="MOUNT FAILED" in t,
        arm=arm, cfg=cfg, loss=float(loss),
        mount=g(r"mount_secs=([\d.]+)", float),
        read=g(r"^bytes=\d+ zero_reads=\d+ secs=([\d.]+)", float),
        bytes=g(r"^bytes=(\d+)", int),
        zero=g(r"zero_reads=(\d+)", int),
        reqs=g(r"n_requests\|(\d+)", int),
        retries=g(r"n_retries\|(\d+)", int),
        pfail=g(r"n_proxy_failover\|(\d+)", int),
        hfail=g(r"n_host_failover\|(\d+)", int),
        xfer=g(r"sz_transferred_bytes\|(\d+)", int),
        relay=g(r"relay_bytes=(\d+)", int),
        dsw=g(r"direct_switches=(\d+)", int),
        asw=g(r"all_switches=(\d+)", int),
        lib=g(r"LOADED: (\S+) \(CernVM-FS Fuse Module\)"),
    ))

REF = 89627462
hdr = (f"{'cfg':<10}{'loss':>6}  {'arm':<8}{'mount':>8}{'read':>8}{'MB/s':>7}"
       f"{'bytes ok':>9}{'retr':>6}{'pfail':>6}{'DIRECT sw':>10}{'off-proxy':>11}")
for cfg in ("proxyonly", "direct"):
    print(f"\n=== CVMFS_HTTP_PROXY = {'127.0.0.1:3129' if cfg=='proxyonly' else '127.0.0.1:3129;DIRECT'} ===")
    print(hdr); print("-" * len(hdr))
    for loss in (0.0, 0.25, 0.50):
        for arm in ("stock", "branch"):
            r = next((x for x in rows if x["cfg"]==cfg and x["arm"]==arm and x["loss"]==loss), None)
            if not r:
                continue
            if r["read"] is None:
                # Distinguish a real failure from a run still in flight; the
                # explicit marker is the only trustworthy signal.
                why = "MOUNT FAILED" if r["failed"] else "(running)"
                print(f"{cfg:<10}{str(int(loss*100))+'%':>6}  {arm:<8}"
                      f"{r['mount']:>7.2f}s{'--':>8}{'--':>7}{why:>14}")
                continue
            mbs = (r["bytes"]/r["read"]/1e6) if r["read"] else 0
            ok = "yes" if r["bytes"] == REF and r["zero"] == 0 else "NO"
            # Bytes the client moved that did not pass through the relay.
            off = ""
            if r["xfer"] and r["relay"]:
                d = r["xfer"] - r["relay"]
                off = f"{d:+,}" if d > 0 else "0"
            print(f"{cfg:<10}{str(int(loss*100))+'%':>6}  {arm:<8}{r['mount']:>7.2f}s{r['read']:>7.2f}s"
                  f"{mbs:>7.2f}{ok:>9}{r['retries']:>6}{r['pfail']:>6}{r['dsw']:>10}{off:>11}")
print("\nlibraries actually loaded:")
for arm in ("stock", "branch"):
    v = {x["lib"] for x in rows if x["arm"] == arm and x["lib"]}
    print(f"  {arm:<8} {v}")
