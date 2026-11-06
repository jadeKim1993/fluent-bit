---
name: flb-tune
description: Performance tuning workflow and harness for the Fluent Bit log collector pipeline (tcp/udp input, Lua enrichment, forward output, backend engine). Use when analyzing or changing throughput, loss, CPU, memory, process or worker sizing of this pipeline, or when the user mentions 튜닝, 성능, 처리량, 유실, 프로세스 수, lua, forward, 뒷단 엔진, or flbt.
---

# flb-tune

Reply to the user in Korean. `H=.claude/skills/flb-tune/harness`.

## Confirmed topology (nothing else is confirmed)

```
senders --tcp,udp--> [Fluent Bit collector: in_tcp/in_udp -> Lua (adds IP + simple metadata) -> out_forward] --forward--> backend engine
```

Target: 700-byte logs, 200k+/s. Lua content, backend implementation, formats, hardware, versions and
the deployed config are UNKNOWN until `profile.yaml` or the user says so. Do not assume them.

## Token rules

1. Run experiments only through `$H/flbt`. One line per run. Never cat logs or `result.json`; use `flbt show`.
2. Resume from `env/STATE.md` and `flbt log`. Never repeat a recorded run.
3. Do not re-derive `references/facts.md`. Re-read a cited line only when about to change that code.
4. Load `references/playbook.md` when choosing the next experiment, `facts.md` for a number or location.
5. Change one variable per run. Use `sweep` for grids. Report results as a table, not narration.
6. End each session by updating `env/STATE.md` (40 lines max): best config, rejected options with run
   ids, open questions.

## Safety rules

- Docker runs share a VM with the user's containers. Uncapped overload runs OOM-killed the user's
  `elasticsearch` container four times on 2026-11-06. Keep `mem_cap`; never bypass `guard()`.
- `ALERT other containers restarted` means stop and tell the user.
- A background task's "exit code 0" proves nothing: the wrapper reported 0 for failed builds and a
  crashed benchmark. Read the final line (`BUILD OK`, `PASS`, `FAIL`, `SELFTEST`).
- `backend=host:port` sends test data to a real system: get explicit user approval per target.
- `env/` holds real configs and scripts. It is gitignored; never commit it.
- `target=local` has no cgroup cap, only the RSS watchdog: use it on a dedicated test host only.

## Workflow

0. **Intake.** `flbt doctor`. Ask only the `intake_pending` items the next step needs, in one batch.
   Have the user put the real Lua script, sample logs and config in `env/`.
1. **Trust the harness.** `flbt build`, then `flbt selftest`. It has never been run end to end: the
   code was only compiled and read. `flbt build` itself worked once, before the last harness edits.
   Fix any failure here first, because every later number depends on it.
2. **Baseline.** `flbt run` (profile values), then `flbt max` for the ceiling.
3. **Locate the bottleneck** with the verdict flags and the ladder in `playbook.md`.
4. **Tune** one knob at a time. Confirm a win with `flbt cmp` and a repeat run.
5. **Validate on the real host** with `target=local`. Docker numbers are only relative.
6. **Change code** only if config cannot reach the target. See "Code changes".

## Commands

```
flbt doctor                      environment, memory budget, pending intake
flbt build [label]               build the working tree (incremental) into a labelled binary
flbt selftest                    known-answer checks of the harness
flbt run k=v ...                 one experiment
flbt sweep k=a,b k2=x,y          grid
flbt max k=v [lo= hi=]           highest rate that passes
flbt log [n]                     ledger, the cross-session memory
flbt show <id> conf|log|threads|sample
flbt cmp <idA> <idB>
```

Keys. Workload: `proto`(tcp|udp|both) `fmt` `size` `rate`(0=max) `conns` `sample`. Input: `in_workers`
`in_inst` `threaded` `procs` `src_key`. Enrichment: `lua`(off|filter|in_proc|out_proc|native_filter|
native_proc) `lua_script` `lua_call` `protected` `meta` `expect`. Output: `out_workers` `ack` `compress`
`send_options` `delay_ms` `backend`. Buffer: `flush` `mem_buf_limit` `storage`. Run: `conf` `warm`
`measure` `drain` `tol` `bin` `cpus` `mem_cap`.

`conf=env/real.yaml` runs the real config unchanged except for these env vars, which it must use:
`${BENCH_IN_PORT}` `${BENCH_OUT_HOST}` `${BENCH_OUT_PORT}` `${BENCH_HTTP_PORT}`. Keep `http_server: on`.
The `lua*`, `in_*`, `out_workers`, `ack` and buffer keys are ignored in this mode.

## Reading the result line

```
r0012 [lua=filter] PASS req=250k sink=250k/s lost=0(0.00%) eng=88 wrk=22/18 out=9/8 rss=130M host=60% -> ok
```

`sink` counts what the fake backend decoded; `lost = sent - sink` is the end-to-end truth. Fluent Bit's
own counters can freeze while the engine is starved. CPU is % of one core per thread: `eng` engine,
`wrk` listener workers, `inp` threaded input, `out` output workers. `PASS` means no loss, no error, no
backlog, and the rate was sustained. `sat:<role>` and `HOSTSAT` are advisory: with `HOSTSAT` the
result is only a lower bound. Flag meanings and next steps are in `playbook.md`.

## Code changes

Follow `AGENTS.md`. Skill-specific additions:

- A/B: `flbt build base` before editing, `flbt build fix` after, then the same run with `bin=base` and
  `bin=fix`, then `flbt cmp`. Repeat both once; treat a gain under 5% as noise.
- Focused checks for this pipeline: `tests/integration/scenarios/{in_tcp,in_udp,in_forward,out_forward}`
  and `tests/runtime/{in_tcp,in_udp,filter_lua,out_forward}.c`. No Lua integration scenario was found,
  so a filter_lua change should add one.
- Never commit, push or edit `lib/` without the user's request. `.claude/skills/flb-tune` stays out of
  code commits.
