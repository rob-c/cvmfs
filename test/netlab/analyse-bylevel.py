#!/usr/bin/env python3
"""Break the A/B runs down by loss level, reading each run's own summary."""
import glob, os, re, statistics as st

base = os.environ.get("LAB", "/var/tmp/cvmfs-losslab") + "/log/"
REF = 89627462
runs = []
for d in sorted(glob.glob(base + "*/")):
    f = os.path.join(d, "summary")
    if not os.path.exists(f):
        continue
    t = open(f).read()
    def g(p, cast=str, dflt=None):
        m = re.search(p, t, re.M)
        return cast(m.group(1)) if m else dflt
    arm = g(r"tree=/var/tmp/cvmfs-(\w+)")
    dsw = g(r"direct_switches=(\d+)", int)
    if arm is None or dsw is None:
        continue                      # mount failed before counters were taken
    runs.append(dict(
        label=os.path.basename(d.rstrip("/")), arm=arm, dsw=dsw,
        loss=g(r"^loss=([\d.]+)", float),
        cfg="direct" if ";DIRECT" in (g(r"^config=(.*)") or "") else "proxyonly",
        allsw=g(r"all_switches=(\d+)", int),
        mount=g(r"mount_secs=([\d.]+)", float),
        read=g(r"^bytes=\d+ zero_reads=\d+ secs=([\d.]+)", float),
        bytes=g(r"^bytes=(\d+)", int), zero=g(r"zero_reads=(\d+)", int),
        retries=g(r"n_retries\|(\d+)", int), pfail=g(r"n_proxy_failover\|(\d+)", int),
        hfail=g(r"n_host_failover\|(\d+)", int),
        xfer=g(r"sz_transferred_bytes\|(\d+)", int), relay=g(r"relay_bytes=(\d+)", int),
    ))

def fmt(v):
    return "--" if not v else f"{st.mean(v):.2f}"

print("=" * 118)
print("DIRECT SWITCHES BY LOSS LEVEL".center(118))
print("=" * 118)
h = (f"{'loss':>5} | {'arm':<7}| {'runs':>4} | {'switched to':>11} | {'never':>5} | "
     f"{'% of runs':>9} | {'proxy-only':>10} | {'  ;DIRECT':>9}")
print(h); print(f"{'':>5} | {'':<7}| {'':>4} | {'DIRECT':>11} | {'':>5} | {'':>9} | "
                f"{'cfg':>10} | {'cfg':>9}")
print("-" * 118)
for loss in (0.0, 0.25, 0.50):
    for arm in ("stock", "branch"):
        r = [x for x in runs if x["loss"] == loss and x["arm"] == arm]
        if not r:
            continue
        sw = [x for x in r if x["dsw"] > 0]
        po = [x for x in r if x["cfg"] == "proxyonly"]
        dr = [x for x in r if x["cfg"] == "direct"]
        print(f"{int(loss*100):>4}% | {arm:<7}| {len(r):>4} | {len(sw):>11} | "
              f"{len(r)-len(sw):>5} | {100*len(sw)/len(r):>8.0f}% | "
              f"{sum(1 for x in po if x['dsw']>0)}/{len(po):<8} | "
              f"{sum(1 for x in dr if x['dsw']>0)}/{len(dr):<7}")
    print("-" * 118)

print()
print("=" * 118)
print("SUPPORTING METRICS BY LOSS LEVEL (mean over runs)".center(118))
print("=" * 118)
h2 = (f"{'loss':>5} | {'arm':<7}| {'runs':>4} | {'mount s (mean/range)':>21} | "
      f"{'read s (mean/range)':>21} | {'payload':>8} | {'retr':>4} | {'pfail':>5} | "
      f"{'off-proxy bytes':>15}")
print(h2); print("-" * 118)
for loss in (0.0, 0.25, 0.50):
    for arm in ("stock", "branch"):
        r = [x for x in runs if x["loss"] == loss and x["arm"] == arm]
        if not r:
            continue
        rd = [x["read"] for x in r if x["read"]]
        mo = [x["mount"] for x in r if x["mount"] is not None]
        ok = all(x["bytes"] == REF and x["zero"] == 0 for x in r)
        off = [x["xfer"] - x["relay"] for x in r if x["xfer"] and x["relay"]]
        # A negative figure means every client byte went through the relay and
        # the relay additionally counted HTTP headers, so nothing bypassed it.
        leak = max(0, max(off)) if off else 0
        ms = f"{st.mean(mo):.2f} [{min(mo):.2f}-{max(mo):.2f}]" if mo else "--"
        rs = f"{st.mean(rd):.2f} [{min(rd):.2f}-{max(rd):.2f}]" if rd else "--"
        print(f"{int(loss*100):>4}% | {arm:<7}| {len(r):>4} | {ms:>21} | {rs:>21} | "
              f"{'all OK' if ok else 'MISMATCH':>8} | "
              f"{sum(x['retries'] or 0 for x in r):>4} | {sum(x['pfail'] or 0 for x in r):>5} | "
              f"{str(leak)+' (none)' if leak==0 else leak:>15} |")
    print("-" * 118)

print()
print("TOTALS")
for arm in ("stock", "branch"):
    r = [x for x in runs if x["arm"] == arm]
    sw = sum(1 for x in r if x["dsw"] > 0)
    print(f"  {arm:<7} runs={len(r):<3} switched-to-DIRECT={sw:<3} "
          f"({100*sw/len(r):.0f}%)  payload-mismatches="
          f"{sum(1 for x in r if x['bytes']!=REF or x['zero'])}")
