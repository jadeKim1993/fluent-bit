# Playbook

Run the ladder top to bottom. Skip a rung only if `flbt log` already answers it. Record each conclusion
in `env/STATE.md`.

## Verdict flags

| flag | meaning | next step |
|---|---|---|
| `sat:eng` | engine thread is the limit | Rung 2, then 3 |
| `sat:wrk` | parsing is the limit | raise `in_workers` or `conns` |
| `sat:inp` | threaded input is the limit | raise `in_inst` |
| `sat:out` | output encode is the limit | raise `out_workers`; check `compress` |
| `backlog` | output slower than input | Rung 5 |
| `backpressure` | TCP senders were held back | find the `sat:*`; if none, `backlog` applies |
| `udp-kernel-drop` | socket buffer overflow | Rung 6 |
| `ingress-drop` | worker queue overflowed, data lost | owner too slow: Rung 2 or 4 |
| `mem-buf-pause` | `mem_buf_limit` hit | Rung 7 |
| `sender-disconnected` | Fluent Bit closed senders | usually a pause; `flbt show <id> log` |
| `output-retry` | backend rejected or timed out | `flbt show <id> log` |
| `lua-error` | script failed | fix before measuring anything else |
| `fields-missing` | enrichment absent at the backend | invalid config; `flbt show <id> sample` |
| `fields-unverified` | sample unreadable, as with gzip | verify once with `compress=off` |
| `loss-unexplained` | no known cause | `flbt show <id> log`, then suspect the harness |
| `DUPLICATES` | sink got more than was sent | retries after partial success, or a sink bug |
| `gen-limited` | generator could not offer the rate | invalid run: lower `rate` or raise `cpus` |
| `gen-no-report` | generator hung | invalid run |
| `HOSTSAT` | all CPUs busy | lower bound only; do not compare |
| `CRASH-*` | process died or hit the RSS limit | Rung 7 |

## Ladder

**Rung 1: baseline.** `flbt run`, `flbt max`. If the ceiling is under `rate` x 1.5, continue.

**Rung 2: what does enrichment cost?** Separates the script from the conversion overhead:

```
flbt max lua=off
flbt max lua=filter lua_script=harness/lua/noop.lua lua_call=noop
flbt max lua=filter
```

off to noop is the msgpack and Lua conversion cost. noop to real is the script's own cost. If the script
cost dominates, optimize the script: hoist `os.getenv`, patterns and tables out of the function, avoid
per-record string building and I/O. Also compare `protected=off`, but a script error then crashes Fluent
Bit: confirm with the user before keeping it.

**Rung 3: can enrichment go native?** Needs `ip_source`, and the fields at the backend must stay equal
(compare `flbt show <id> sample`).

- collector IP or static fields: `lua=native_filter`, `lua=native_proc`, with `meta=` and `expect=`.
- sender IP: `src_key=<field>`. The value is `tcp://ip:port`, so check whether the backend can accept
  that or a cheap native step can strip it. Measure the cost.

**Rung 4: where should Lua run, and how many copies?**

```
flbt sweep lua=filter,in_proc,out_proc in_workers=1,2
flbt sweep lua=in_proc threaded=on in_inst=1,2,3
flbt sweep lua=filter procs=1,2,3
```

Compare multi-instance against multi-process: instances share one engine and buffer accounting,
processes isolate failures but need `net.share_port` and multiply the backend connections. Use
`conns` from the profile: too few connections skew any of these.

**Rung 5: forward and backend.** Use the profile's real `ack` and `compress` first.

```
flbt sweep ack=off,on out_workers=1,2,4
flbt sweep compress=off,on
flbt sweep flush=0.5,1,2 ack=on
flbt sweep ack=on delay_ms=5,20,100 out_workers=2,4
```

Note what `delay_ms` models: time per forward message. From `backend_max_rate` work out how much delay
the backend really adds, then find the `out_workers` that absorbs it. Then, with approval, confirm
against the real backend using `backend=host:port`.

**Rung 6: UDP.** `flbt sweep proto=udp in_workers=1,2,4 conns=<real>`. Loss here is expected in Docker.
Tune `net.core.rmem_default` on a real Linux host (`target=local`): in_udp has no buffer option, so
`rmem_max` alone does nothing. If `udp_loss_ok` is 0, tell the user that UDP cannot guarantee it and
propose TCP. A code option is adding a receive buffer option to in_udp, as in_syslog has.

**Rung 7: overload and durability.** Do this before calling a config production-ready. Steady-state
numbers hide these failures.

```
flbt run rate=<2x ceiling> mem_buf_limit=512M
flbt run rate=<2x ceiling> storage=filesystem mem_buf_limit=512M
flbt run ack=on delay_ms=2000 mem_buf_limit=512M
```

Decide with the user which failure is acceptable: backpressure, disconnect, drop or disk. Docker disk
speed does not represent production, so re-measure filesystem storage on the real host, and check
`outage_tolerance` against disk space.

## Sizing

Per-process ceiling `C` is the `flbt max` result on the real host with real Lua and backend options.

```
processes = ceil(peak_rate x 1.5 / C)
```

Keep the hottest thread under about 70% at peak. Spread needs many more connections than processes:
reserve one core per busy thread, plus the OS and NIC interrupts. Confirm with `flbt run procs=<N>`
and check that the spread in `flbt show <id> threads` is even.

## Deploying N processes

Depends on `deploy`. Each process needs its own `http_port`, `storage.path` and `net.share_port: true`.
Do not edit production configs from this skill: hand the user the change and the evidence.
