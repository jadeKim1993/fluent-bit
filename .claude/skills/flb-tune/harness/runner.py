#!/usr/bin/env python3
"""Runs one experiment and prints a single verdict line.

Usage: runner.py <job.json>. Runs inside the harness container, or directly on
a Linux host. Everything bulky stays in the run directory. The sink's record
count is the source of truth, because Fluent Bit's own counters stop updating
while the engine thread is starved.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request

IN_PORT, SINK_PORT, HTTP_BASE = 5170, 24224, 2020
HZ = os.sysconf("SC_CLK_TCK")
ERR_PATTERNS = {
    "busy": r"could not append (?:TCP|UDP) logs",
    "deferred_drop": r"could not ingest deferred worker payload",
    "paused": r"paused \(mem buf overlimit",
    "flush_fail": r"failed to flush chunk",
    "retry_exhausted": r"cannot be retried",
    "lua_err": r"\[filter:lua|\[processor:lua|error code|lua_pcall",
    "conn_err": r"no upstream connections|connection .* timed out|Connection refused",
}


def bool_on(v):
    return str(v).lower() in ("on", "true", "1", "yes")


def protocols(job):
    return ["tcp", "udp"] if job["proto"] == "both" else [job["proto"]]


def meta_pairs(job):
    return [p.split("=", 1) for p in job["meta"].split(";") if "=" in p]


def lua_unit(job, pad):
    lines = ["- name: lua", f"  script: {job['lua_script']}", f"  call: {job['lua_call']}"]
    if not bool_on(job["protected"]):
        lines.append("  protected_mode: false")
    return [pad + x for x in lines]


def enrich_blocks(job):
    """Returns (filters, input processors, output processors) for the chosen placement."""
    mode = job["lua"]
    filters, inproc, outproc = [], [], []
    if mode == "filter":
        filters = lua_unit(job, "    ")
        filters.insert(1, "      match: '*'")
    elif mode == "in_proc":
        inproc = lua_unit(job, "          ")
    elif mode == "out_proc":
        outproc = lua_unit(job, "          ")
    elif mode == "native_filter":
        filters = ["    - name: record_modifier", "      match: '*'", "      record:"]
        filters += [f"        - {k} {v}" for k, v in meta_pairs(job)]
    elif mode == "native_proc":
        for k, v in meta_pairs(job):
            inproc += ["          - name: content_modifier", "            action: insert",
                       f"            key: {k}", f"            value: {v}"]
    elif mode != "off":
        raise SystemExit(f"unknown lua mode {mode}")
    return filters, inproc, outproc


def make_conf(job, idx):
    filters, inproc, outproc = enrich_blocks(job)
    shared = int(job["in_inst"]) > 1 or int(job["procs"]) > 1
    out = ["service:", f"  flush: {job['flush']}", "  grace: 2", "  log_level: info",
           "  http_server: on", "  http_listen: 127.0.0.1", f"  http_port: {HTTP_BASE + idx}"]
    if job["storage"] == "filesystem":
        out.append(f"  storage.path: /tmp/flbt-storage-{idx}")
    out += ["pipeline:", "  inputs:"]
    for proto in protocols(job) * int(job["in_inst"]):
        out += [f"    - name: {proto}", "      listen: 127.0.0.1", f"      port: {IN_PORT}",
                f"      format: {job['fmt']}", f"      workers: {job['in_workers']}", "      tag: app.log"]
        if bool_on(job["threaded"]):
            out.append("      threaded: true")
        if shared:
            out.append("      net.share_port: true")
        if job["mem_buf_limit"]:
            out.append(f"      mem_buf_limit: {job['mem_buf_limit']}")
        if job["storage"] == "filesystem":
            out.append("      storage.type: filesystem")
        if job["src_key"]:
            out.append(f"      source_address_key: {job['src_key']}")
        if inproc:
            out += ["      processors:", "        logs:"] + inproc
    if filters:
        out += ["  filters:"] + filters
    host, port = "127.0.0.1", SINK_PORT
    if job["backend"]:
        host, port = job["backend"].rsplit(":", 1)
    out += ["  outputs:", "    - name: forward", "      match: '*'", f"      host: {host}",
            f"      port: {port}", f"      workers: {job['out_workers']}"]
    if bool_on(job["ack"]):
        out.append("      require_ack_response: true")
    if bool_on(job["compress"]):
        out.append("      compress: gzip")
    if bool_on(job["send_options"]):
        out.append("      send_options: true")
    if outproc:
        out += ["      processors:", "        logs:"] + outproc
    return "\n".join(out) + "\n"


def metrics(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/metrics", timeout=3) as r:
        d = json.load(r)
    out = d.get("output", {}).values()
    return {"in": sum(v.get("records", 0) for v in d.get("input", {}).values()),
            "out": sum(v.get("proc_records", 0) for v in out),
            "retries": sum(v.get("retries", 0) for v in out),
            "dropped": sum(v.get("dropped_records", 0) for v in out),
            "errors": sum(v.get("errors", 0) for v in out)}


def try_metrics(ports):
    try:
        rows = [metrics(p) for p in ports]
    except Exception:
        return None
    return {k: sum(r[k] for r in rows) for k in rows[0]}


def threads(pid):
    res = {}
    try:
        for t in os.listdir(f"/proc/{pid}/task"):
            s = open(f"/proc/{pid}/task/{t}/stat").read()
            f = s[s.rindex(")") + 2:].split()
            res[int(t)] = (s[s.index("(") + 1:s.rindex(")")], int(f[11]) + int(f[12]))
    except OSError:
        pass
    return res


def role_map(snapshot):
    """The engine is the oldest flb-pipeline thread; listener workers inherit its name."""
    roles, seen = {}, False
    for tid in sorted(snapshot):
        name = snapshot[tid][0]
        if name == "flb-pipeline":
            roles[tid] = "wrk" if seen else "eng"
            seen = True
        elif name.startswith("flb-out"):
            roles[tid] = "out"
        elif name.startswith("flb-in"):
            roles[tid] = "inp"
        else:
            roles[tid] = "oth"
    return roles


def rss_mb(pid):
    try:
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmRSS"):
                return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 0


def udp_rcvbuf_errors():
    rows = [line.split() for line in open("/proc/net/snmp") if line.startswith("Udp:")]
    return int(dict(zip(rows[0], rows[1]))["RcvbufErrors"])


def host_ticks():
    f = open("/proc/stat").readline().split()[1:]
    vals = [int(x) for x in f]
    return sum(vals), vals[3] + vals[4]


def oom_kills():
    try:
        for line in open("/sys/fs/cgroup/memory.events"):
            if line.startswith("oom_kill "):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def mp_decode(b, i=0):
    """Minimal msgpack reader for the sampled record."""
    c = b[i]
    i += 1
    if c <= 0x7f:
        return c, i
    if c >= 0xe0:
        return c - 256, i
    if 0x80 <= c <= 0x8f or c in (0xde, 0xdf):
        n = c & 0x0f
        if c == 0xde:
            n, i = struct.unpack(">H", b[i:i + 2])[0], i + 2
        elif c == 0xdf:
            n, i = struct.unpack(">I", b[i:i + 4])[0], i + 4
        d = {}
        for _ in range(n):
            k, i = mp_decode(b, i)
            d[k], i = mp_decode(b, i)
        return d, i
    if 0x90 <= c <= 0x9f or c in (0xdc, 0xdd):
        n = c & 0x0f
        if c == 0xdc:
            n, i = struct.unpack(">H", b[i:i + 2])[0], i + 2
        elif c == 0xdd:
            n, i = struct.unpack(">I", b[i:i + 4])[0], i + 4
        out = []
        for _ in range(n):
            v, i = mp_decode(b, i)
            out.append(v)
        return out, i
    if 0xa0 <= c <= 0xbf or c in (0xd9, 0xda, 0xdb, 0xc4, 0xc5, 0xc6):
        if 0xa0 <= c <= 0xbf:
            n = c & 0x1f
        elif c in (0xd9, 0xc4):
            n, i = b[i], i + 1
        elif c in (0xda, 0xc5):
            n, i = struct.unpack(">H", b[i:i + 2])[0], i + 2
        else:
            n, i = struct.unpack(">I", b[i:i + 4])[0], i + 4
        return b[i:i + n].decode("utf-8", "replace"), i + n
    if c == 0xc0:
        return None, i
    if c in (0xc2, 0xc3):
        return c == 0xc3, i
    sized = {0xca: (">f", 4), 0xcb: (">d", 8), 0xcc: (">B", 1), 0xcd: (">H", 2), 0xce: (">I", 4),
             0xcf: (">Q", 8), 0xd0: (">b", 1), 0xd1: (">h", 2), 0xd2: (">i", 4), 0xd3: (">q", 8)}
    if c in sized:
        fmt, n = sized[c]
        return struct.unpack(fmt, b[i:i + n])[0], i + n
    ext = {0xd4: 2, 0xd5: 3, 0xd6: 5, 0xd7: 9, 0xd8: 17}
    if c in ext:
        return "<ext>", i + ext[c]
    if c == 0xc7:
        return "<ext>", i + 2 + b[i]
    raise ValueError(f"msgpack type {c:#x}")


def sample_record(path):
    try:
        entry = mp_decode(open(path, "rb").read())[0]
    except (OSError, ValueError, IndexError):
        return None
    if isinstance(entry, list) and len(entry) >= 2 and isinstance(entry[1], dict):
        return entry[1]
    return None


def read_sink(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return {"records": 0, "bytes": 0, "msgs": 0, "acks": 0, "unknown": 0, "conns": 0}


def log_counts(paths):
    counts = {k: 0 for k in ERR_PATTERNS}
    counts["error_lines"] = 0
    for p in paths:
        with open(p, errors="replace") as f:
            for line in f:
                if "[error]" not in line and "[ warn]" not in line:
                    continue
                counts["error_lines"] += 1
                for k, rx in ERR_PATTERNS.items():
                    if re.search(rx, line):
                        counts[k] += 1
    return counts


def stop(procs):
    codes = []
    for p in procs:
        if p.poll() is None:
            p.terminate()
    for p in procs:
        try:
            codes.append(p.wait(8))
        except subprocess.TimeoutExpired:
            p.kill()
            codes.append("hung")
    return codes


def verdict(job, r):
    rate = float(job["rate"])
    tol = float(job["tol"])
    flags = []
    hot = max([(c, role) for role, cs in r["cpu"].items() for c in cs] or [(0, "-")])
    if r["rss_killed"]:
        flags.append("CRASH-RSS-LIMIT")
    elif r["crashed"]:
        flags.append("CRASH-OOM" if r["oom_kills"] else "CRASH")
    if not r["gen_reported"]:
        flags.append("gen-no-report")
    if r["gen_failed"]:
        flags.append("sender-disconnected")
    if r["sink_unknown"]:
        flags.append("sink-count-unknown")
    if r["lost"] < 0:
        flags.append("DUPLICATES")
    if r["lost_pct"] > tol:
        causes = []
        if r["udp_drop_rate"] > 0:
            causes.append("udp-kernel-drop")
        if r["log"]["busy"] or r["log"]["deferred_drop"]:
            causes.append("ingress-drop")
        if r["log"]["paused"]:
            causes.append("mem-buf-pause")
        flags += causes or ["loss-unexplained"]
    if r["log"]["flush_fail"] or r["retries"]:
        flags.append("output-retry")
    if r["log"]["lua_err"]:
        flags.append("lua-error")
    if r["fields_ok"] is False:
        flags.append("fields-missing")
    if r["gen_stalled"] or (rate > 0 and r["offered"] < rate * 0.98):
        flags.append("backpressure" if job["proto"] != "udp" else "gen-limited")
    # The window rate jitters by one flush, so a slow output is judged by how
    # long it needed to catch up after the senders stopped.
    if r["drain_s"] > 2 * float(job["flush"]) + 1 or r["undrained"] > 0:
        flags.append("backlog")
    r["pass"] = len(flags) == 0
    if job["expect"] and r["fields_ok"] is None:
        flags.append("fields-unverified")
    if hot[0] >= 90:
        flags.append(f"sat:{hot[1]}")
    if r["host_busy"] >= 90:
        flags.append("HOSTSAT")
    return flags or ["ok"]


def short(v):
    if v >= 1e6:
        return f"{v / 1e6:.2f}M"
    return f"{v / 1e3:.0f}k" if v >= 1e3 else f"{v:.0f}"


def summary(job, r):
    cpu = " ".join(f"{k}={'/'.join(str(c) for c in v[:4])}" for k, v in r["cpu"].items() if v)
    return (f"{'PASS' if r['pass'] else 'FAIL'} req={short(float(job['rate']))} "
            f"sink={short(r['sink_rate'])}/s lost={r['lost']}({r['lost_pct']:.2f}%) "
            f"{cpu} rss={r['rss_peak']}M host={r['host_busy']}% -> {','.join(r['verdict'])}")


def main():
    job = json.load(open(sys.argv[1]))
    d = job["dir"]
    procs_n = int(job["procs"])
    warm, measure, drain = int(job["warm"]), int(job["measure"]), int(job["drain"])
    tools = job["tools"]
    oom0 = oom_kills()

    sink = None
    if not job["backend"]:
        sink = subprocess.Popen([f"{tools}/sink", str(SINK_PORT), f"{d}/sink.json", f"{d}/sample.bin",
                                 str(job["delay_ms"])])
    time.sleep(0.2)

    fbs, ports, logs = [], [], []
    out_host, out_port = "127.0.0.1", str(SINK_PORT)
    if job["backend"]:
        out_host, out_port = job["backend"].rsplit(":", 1)
    for i in range(procs_n):
        conf = f"{d}/fb{i}.yaml"
        if job["conf"]:
            shutil.copy(job["conf"], f"{d}/fb{i}.conf.txt")
            conf = job["conf"]
        else:
            open(conf, "w").write(make_conf(job, i))
        logs.append(f"{d}/fb{i}.log")
        lf = open(logs[-1], "w")
        env = dict(os.environ, BENCH_IN_PORT=str(IN_PORT), BENCH_OUT_HOST=out_host,
                   BENCH_OUT_PORT=out_port, BENCH_HTTP_PORT=str(HTTP_BASE + i))
        fbs.append(subprocess.Popen([job["fb"], "-c", conf], stdout=lf, stderr=lf, env=env))
        ports.append(HTTP_BASE + i)

    deadline = time.time() + 10
    while time.time() < deadline and try_metrics(ports) is None:
        if any(p.poll() is not None for p in fbs):
            break
        time.sleep(0.2)
    if any(p.poll() is not None for p in fbs) or try_metrics(ports) is None:
        tail = open(logs[0], errors="replace").read()[-400:].replace("\n", " | ")
        stop(fbs)
        if sink:
            sink.kill()
        print(f"FAIL START fluent-bit did not come up: {tail}")
        return 0

    gens = []
    share = float(job["rate"]) / len(protocols(job))
    for proto in protocols(job):
        args = [f"{tools}/gen", proto, "127.0.0.1", str(IN_PORT), job["fmt"], str(job["conns"]),
                str(share), str(warm + measure + 1), str(job["size"])]
        if job["sample"]:
            args.append(job["sample"])
        gens.append(subprocess.Popen(args, stdout=subprocess.PIPE, text=True))

    state = {"rss_peak": 0, "crashed": False, "rss_killed": False}

    def watch(seconds):
        """Waits while enforcing the RSS limit, which also protects target=local."""
        end = time.time() + seconds
        while time.time() < end and not state["crashed"]:
            time.sleep(0.25)
            rss = sum(rss_mb(p.pid) for p in fbs)
            state["rss_peak"] = max(state["rss_peak"], rss)
            if rss > int(job["rss_limit_mb"]):
                state["rss_killed"] = state["crashed"] = True
                for p in fbs:
                    p.kill()
            elif any(p.poll() is not None for p in fbs):
                state["crashed"] = True

    watch(warm)
    s0 = read_sink(f"{d}/sink.json")
    c0 = {p.pid: threads(p.pid) for p in fbs}
    u0, (h0, i0), t0 = udp_rcvbuf_errors(), host_ticks(), time.time()
    watch(measure)
    crashed, rss_peak = state["crashed"], state["rss_peak"]
    dt = max(time.time() - t0, 0.001)
    s1 = read_sink(f"{d}/sink.json")
    c1 = {p.pid: threads(p.pid) for p in fbs}
    u1, (h1, i1) = udp_rcvbuf_errors(), host_ticks()
    m_end = try_metrics(ports) if not crashed else None

    sent = offered = failed = stalled = reported = 0
    for gen in gens:
        if crashed:
            gen.kill()
        try:
            g = json.loads(gen.communicate(timeout=warm + measure + 8)[0].strip() or "{}")
        except (subprocess.TimeoutExpired, ValueError):
            gen.kill()
            g = {}
        reported += 1 if g else 0
        sent += int(g.get("sent", 0))
        offered += float(g.get("rate", 0))
        failed += int(g.get("failed", 0))
        stalled += int(g.get("stalled", 0))
    gen_reported = crashed or reported == len(gens)

    t_end = time.time()
    last, last_change, deadline = -1, t_end, t_end + drain
    while time.time() < deadline and not crashed:
        cur = read_sink(f"{d}/sink.json")["records"]
        if cur != last:
            last, last_change = cur, time.time()
        if cur >= sent and not job["backend"]:
            break
        if time.time() - last_change > 2.5:
            break
        time.sleep(0.2)
    drain_s = round(last_change - t_end, 1)
    s2 = read_sink(f"{d}/sink.json")
    undrained = max(0, sent - s2["records"]) if job["proto"] == "tcp" else 0
    m_fin = try_metrics(ports) if not crashed else None
    codes = stop(fbs)
    if sink:
        sink.kill()

    cpu, per_thread = {}, []
    for pid, snap in c1.items():
        roles = role_map(snap)
        for tid, (name, ticks) in snap.items():
            pct = round(100 * (ticks - c0[pid].get(tid, (name, ticks))[1]) / HZ / dt)
            per_thread.append({"pid": pid, "tid": tid, "name": name, "role": roles[tid], "cpu": pct})
            if roles[tid] != "oth" and pct >= 2:
                cpu.setdefault(roles[tid], []).append(pct)
    for v in cpu.values():
        v.sort(reverse=True)

    delta_total = max(1, h1 - h0)
    rec = sample_record(f"{d}/sample.bin")
    expect = [x for x in job["expect"].split(",") if x]
    fields_ok = None
    if expect and rec is not None:
        fields_ok = all(k in rec for k in expect)

    use_sink = not job["backend"]
    final = m_fin or m_end or {"in": 0, "out": 0, "retries": 0, "dropped": 0, "errors": 0}
    lost = (sent - s2["records"]) if use_sink else (sent - final["out"])
    r = {"sent": sent, "offered": offered, "gen_failed": failed, "gen_stalled": stalled,
         "gen_reported": gen_reported, "drain_s": drain_s, "rss_killed": state["rss_killed"],
         "sink_rate": (s1["records"] - s0["records"]) / dt if use_sink else 0,
         "sink_total": s2["records"], "sink_msgs": s2["msgs"], "sink_acks": s2["acks"],
         "sink_conns": s2["conns"], "sink_unknown": s2["unknown"],
         "fb_in": final["in"], "fb_out": final["out"], "retries": final["retries"],
         "dropped": final["dropped"], "lost": lost,
         "lost_pct": 100 * lost / sent if sent else 0.0, "undrained": undrained if use_sink else 0,
         "udp_drop_rate": round((u1 - u0) / dt), "cpu": cpu, "threads": per_thread,
         "rss_peak": rss_peak, "host_busy": round(100 * (1 - (i1 - i0) / delta_total)),
         "crashed": crashed, "exit_codes": codes, "oom_kills": oom_kills() - oom0,
         "log": log_counts(logs), "fields_ok": fields_ok, "sample": rec}
    r["verdict"] = verdict(job, r)
    json.dump(r, open(f"{d}/result.json", "w"), indent=1, default=str)
    print(summary(job, r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
