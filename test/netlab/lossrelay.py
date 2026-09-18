#!/usr/bin/env python3
"""Emulate what packet loss does to a CVMFS client's path to its proxy.

This host cannot drop packets: sch_netem, xt_statistic and network namespaces
are all unavailable (see losslab.sh for the checks).  What it can do is
reproduce, faithfully and measurably, the three things a lossy link actually
presents to the application above TCP:

  throughput collapse  TCP's steady-state rate is about MSS/(RTT*sqrt(p)), the
                       Mathis model, so a given loss rate maps to a rate cap.
  stalls               a lost retransmission costs an RTO, which doubles on
                       each further loss; long stalls are what trip
                       CVMFS_TIMEOUT and CVMFS_LOW_SPEED_LIMIT.
  severed connections  sustained loss, or a middlebox reacting to it, kills
                       flows mid-transfer, which the client sees as a short
                       transfer.

It does NOT emulate reordering or duplication, and it cannot corrupt the byte
stream, because TCP would have repaired that below us anyway.
"""
import argparse, random, signal, socket, sys, threading, time

MSS = 1460

def mathis_bps(rtt_s, loss):
    if loss <= 0:
        return None                      # unlimited
    return MSS / (rtt_s * (loss ** 0.5))

class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.conns = 0; self.resets = 0; self.stalls = 0; self.bytes = 0
    def add(self, **kw):
        with self.lock:
            for k, v in kw.items():
                setattr(self, k, getattr(self, k) + v)

def pump(src, dst, rate_bps, stall_prob, stall_ms, stats, stop):
    """Copy src->dst, rate limited, with occasional stalls."""
    budget_start = time.time(); sent_in_window = 0
    try:
        while not stop.is_set():
            chunk = src.recv(16384)
            if not chunk:
                break
            if stall_prob and random.random() < stall_prob:
                stats.add(stalls=1)
                time.sleep(stall_ms / 1000.0)
            if rate_bps:
                sent_in_window += len(chunk)
                elapsed = time.time() - budget_start
                want = sent_in_window / rate_bps
                if want > elapsed:
                    time.sleep(want - elapsed)
                if elapsed > 1.0:
                    budget_start = time.time(); sent_in_window = 0
            dst.sendall(chunk)
            stats.add(bytes=len(chunk))
    except OSError:
        pass
    finally:
        stop.set()
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

def handle(client, target, rate_bps, stall_prob, stall_ms, reset_prob, stats):
    stats.add(conns=1)
    try:
        upstream = socket.create_connection(target, timeout=10)
    except OSError:
        client.close(); return
    stop = threading.Event()
    if reset_prob and random.random() < reset_prob:
        # Sever it mid-flight: an abortive close, i.e. RST, after a short delay.
        def kill():
            time.sleep(random.uniform(0.05, 0.4))
            if stop.is_set():
                return          # transfer already finished; nothing left to sever
            try:
                # SO_LINGER with a zero timeout turns close() into an RST, which
                # is what a middlebox or a dead path actually looks like.
                client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                  b'\x01\x00\x00\x00\x00\x00\x00\x00')
                stats.add(resets=1)
            except OSError:
                return          # raced with a normal close
            stop.set()
            for s in (client, upstream):
                try: s.close()
                except OSError: pass
        threading.Thread(target=kill, daemon=True).start()
    a = threading.Thread(target=pump, args=(client, upstream, None, 0, 0, stats, stop), daemon=True)
    b = threading.Thread(target=pump, args=(upstream, client, rate_bps, stall_prob, stall_ms, stats, stop), daemon=True)
    a.start(); b.start(); a.join(); b.join()
    for s in (client, upstream):
        try: s.close()
        except OSError: pass

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--listen", type=int, default=3129)
    p.add_argument("--target", default="194.81.255.225:3128")
    p.add_argument("--loss", type=float, default=0.25, help="emulated loss fraction")
    p.add_argument("--rtt-ms", type=float, default=0.77)
    p.add_argument("--stall-prob", type=float, default=None)
    p.add_argument("--stall-ms", type=float, default=600)
    p.add_argument("--reset-prob", type=float, default=None)
    p.add_argument("--report", type=float, default=30)
    a = p.parse_args()

    host, port = a.target.split(":"); target = (host, int(port))
    rate = mathis_bps(a.rtt_ms / 1000.0, a.loss)
    # A stall costs an RTO and needs several consecutive losses; scale both
    # knobs off the loss rate so one number drives the whole emulation.
    stall_prob = a.stall_prob if a.stall_prob is not None else (a.loss ** 3)
    reset_prob = a.reset_prob if a.reset_prob is not None else (a.loss ** 4)

    stats = Stats()
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.listen)); srv.listen(512)
    sys.stderr.write(
        f"loss={a.loss:.2f} -> rate={'unlimited' if not rate else f'{rate/1e6:.2f} MB/s'}"
        f" stall_p={stall_prob:.4f} reset_p={reset_prob:.4f} -> {a.target}\n")
    sys.stderr.flush()

    def reporter():
        while True:
            time.sleep(a.report)
            sys.stderr.write(f"  [relay] conns={stats.conns} bytes={stats.bytes}"
                             f" stalls={stats.stalls} resets={stats.resets}\n")
            sys.stderr.flush()
    threading.Thread(target=reporter, daemon=True).start()

    def final(_sig=None, _frm=None):
        # Short runs can end before the first periodic report, so always emit
        # the totals on the way out.
        sys.stderr.write(f"  [relay] FINAL conns={stats.conns} bytes={stats.bytes}"
                         f" stalls={stats.stalls} resets={stats.resets}\n")
        sys.stderr.flush()
        sys.exit(0)
    signal.signal(signal.SIGTERM, final)
    signal.signal(signal.SIGINT, final)

    while True:
        try:
            c, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=handle,
                         args=(c, target, rate, stall_prob, a.stall_ms, reset_prob, stats),
                         daemon=True).start()

main()
