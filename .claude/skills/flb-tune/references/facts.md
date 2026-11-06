# Facts

Tags: **[code]** read in source at c998e63e6 (v5.1.0 dev). **[measured]** observed, see Environment.
**[hypothesis]** reasoned, never measured: test before relying on it. Line numbers drift; grep the
symbol if one looks wrong. Update this file when a hypothesis is settled.

## Threads and data path

- [code] `workers>1` on in_tcp/in_udp: each listener thread reads and parses, then hands records to the
  owner through `flb_input_ingress_queue_log` (`plugins/in_tcp/tcp.h:74`). The owner drains them in
  `flb_input_ingress_collector` (`src/flb_input_ingest.c:330`). `workers=1` has no queue: the same
  thread reads and appends, so TCP gets natural backpressure.
- [code] Workers force SO_REUSEPORT (`plugins/in_tcp/tcp.c:159`, `plugins/in_udp/udp.c:161`). The kernel
  hashes the 4-tuple, so spread depends on the number of connections or source sockets.
- [code] Pipeline filters, including filter_lua, run in `flb_filter_do` at `src/flb_input_chunk.c:3285`.
  With `threaded: true`, `flb_input_chunk_append_raw` first goes through the ring buffer
  (`src/flb_input_chunk.c:3631`), drained by `flb_input_chunk_ring_buffer_collector` (:3567).
- [hypothesis] So filter_lua runs on one thread per process (the engine) whatever `workers` or
  `threaded` say. `selftest` check `lua-on-engine` is the first evidence to collect.
- [code] Input processors run in the caller of `flb_input_log_append` (`src/flb_input_log.c:1562`): the
  input thread when `threaded`, otherwise the owner.
- [code] Output processors run in the flush path (`include/fluent-bit/flb_output.h:819`), and each unit
  takes a mutex (`src/flb_processor.c:1403`).
- [hypothesis] So Lua as an output processor leaves the engine thread but stays serialized across
  output workers.
- [hypothesis] Parallel Lua inside one process needs N input instances sharing the port, each
  `threaded` with its own Lua processor: `in_inst=N threaded=on lua=in_proc`. Does `threaded` combine
  with `workers>1`? Unknown; the planned run was lost to the crash.

## Overload and loss

- [code] Ingress queue limits: 8192 events, 256 MiB (`include/fluent-bit/http_server/flb_http_server.h:69`),
  byte limit lowered to `mem_buf_limit` when smaller. A full queue waits 100 ms
  (`src/flb_input_ingest.c:248`), then returns `FLB_INPUT_INGRESS_BUSY` = -2.
- [code] BUSY makes the parser return -1 and the caller resets the buffer (`plugins/in_tcp/tcp_conn.c:149`,
  :399, :519), so TCP can lose data while the sender believes it was delivered. Log: `could not append
  TCP logs ... ret=-2`.
- [measured] That log appeared in real runs (`format none`, `workers=2`, unthrottled).
- [code] On `mem_buf_limit`, in_tcp closes client connections (`in_tcp_connections_pause`,
  `plugins/in_tcp/tcp.c:47`). Senders must reconnect.
- [code] Payloads rejected on the owner side log `could not ingest deferred worker payload`
  (`src/flb_input_ingest.c:442`) and are destroyed.
- [measured] One pause at `mem_buf_limit: 256M`: 351,666 records ingested, 6,902 payloads dropped
  (about 46 records each by size, so roughly 320k records), connections closed, resume logged 5.4 s later.
- [measured] Without `mem_buf_limit`, same load: RSS grew about 0.8 GB/s and the process was OOM-killed
  (cgroup `oom_kill` +1 at a 3 GB cap; 5.7 GB anon RSS uncapped). The same config at a steady 250k/s
  held RSS flat at 130 MB.
- [measured] During that overload `/api/v1/metrics` read `records=0` and `out=0` for seconds.
- [hypothesis] The owner collector starves engine timers (flush, metrics), which would explain both
  the frozen counters and the unbounded growth.
- [code] in_udp has no `cb_pause` and no socket receive buffer option; in_syslog has
  `receive_buffer_size` (`plugins/in_syslog/syslog_server.c:178`). in_udp depends on `net.core.rmem_default`.
- [code] in_udp handles one datagram per event, each with its own encode and append
  (`plugins/in_udp/udp_conn.c:478`). With workers, that is one queue event per datagram.

## Plugin details

- [code] in_tcp/in_udp defaults: `format json`, `chunk_size` = `buffer_size` = 32 KiB; both options are
  in KiB. `format none` needs a trailing separator (default `\n`).
- [code] `source_address_key` writes `tcp://ip:port` or `udp://ip:port`, not a bare IP
  (`src/flb_connection.c:95`, :112). Stripping it needs another step.
- [code] filter_lua converts every record to a Lua table (`plugins/filter_lua/lua.c:572`). Options:
  `protected_mode` default true (:801), `time_as_table` default false (:807).
- [code] out_forward: 2 workers by default (`plugins/out_forward/forward.c:1992`),
  `require_ack_response` default false, `compress gzip` implies `send_options` (:1045), ack is read
  after each send (`forward_read_ack`, :778), failed ack means retry.
- [code] The harness sink does not implement the `shared_key` handshake. Test that with a real backend.
- [code] FLB_RELEASE sets the build type through a normal variable (`CMakeLists.txt:551`), so
  `CMakeCache.txt` still shows `FLB_DEBUG=ON`. `flags.make` showed `-O2 -g -DNDEBUG`.
- [code] Listener workers were added in 670b74c10 (2026-07-14). This clone has no tags, so the first
  release containing them is unknown: check `prod_version` before recommending `workers`.

## Measurements

**Environment.** Docker Desktop VM, 4 vCPU, Apple M1, loopback, generator sharing the same CPUs, 700-byte
records, 12-key JSON or plain text, -O2, jemalloc, SIMD, default socket buffers, memory storage,
`flush 1`, **no Lua, no ack, no gzip**. Use the numbers to compare options, not to size production.

TCP, CPU per thread at 250k/s, output `null`, no errors:

| fmt | workers=1 | workers=2 | workers=3 |
|---|---|---|---|
| json | 56 | 43/28/15 | 35/28/15/7 |
| none | 25 | 21/12/11 | 17/13/6/4 |

TCP ceiling, unthrottled, output `null`:

| config | rate/s | note |
|---|---|---|
| json, workers=1 | 506k | one thread at 100% |
| json, workers=2 | 1.15M | |
| json, workers=3 | 1.68M | all 4 vCPU saturated |
| none, workers=1 | 1.39M | |
| none, workers=2 | crashed | owner outrun, OOM |
| json, 2 processes | 985k | workers=1 each |
| json, 3 processes | 1.37M | workers=1 each |

UDP at 250k/s offered, kernel `RcvbufErrors` as % of offered:

| config | json | none |
|---|---|---|
| workers=1 | 8.8 | 9.2 |
| workers=2 | 2.4 | 0.2 |
| workers=3 | 0.9 | 0.8 |
| 2 processes | 8.7 | - |
| 3 processes | 0.7 | - |

UDP never reached zero loss here, even at 100k/s (0.3%, workers=2). Raising `rmem_default` is unmeasured
because the sysctl is not visible inside the container.

Skew: `workers=3` with one UDP source lost 4.3% with one thread at 97%. One TCP connection used one
worker only. Eight connections over 3 processes split 62k/31k/156k.

Forward to a discarding sink, no ack, `tcp json workers=2`: out worker CPU 8% at 250k (1 worker), 4+4%
at 250k (2 workers), 9+9% at 500k.

## Not measured yet

Lua cost in any placement; `threaded`; multiple instances on one port; ack; gzip; TLS; filesystem
storage; slow or failing backend; retries; `rmem_default`; a real NIC; x86; `proto=both`.
