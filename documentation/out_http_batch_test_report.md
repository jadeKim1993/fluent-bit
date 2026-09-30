# out_http 배치 기능 E2E 테스트 결과

- 테스트한 코드: master `be9bb0c20` 와 동일한 소스 (실행 당시 작업 트리, out_http 파일 내용 일치 확인)
- 기본 테스트: 2026-09-30 22:09:06 KST — **PASS 21 / FAIL 0**
- 장시간 버퍼링·장애 테스트: 2026-09-30 22:03:37 KST — **PASS 23 / FAIL 0**

## 1. 결과 요약

### 기본 테스트 (`e2e.sh`)
| 테스트 | PASS | FAIL | INFO |
|--------|------|------|------|
| T1. JSON 봉투 + 주기 배치 (batch_interval 5초) | 5 | 0 | 0 |
| T2. 유실 방지 모드 (filesystem) + 같은 입력을 쓰는 다른 output | 10 | 0 | 1 |
| T3. 결과 이벤트 → PostgreSQL (batch_interval 3초) | 6 | 0 | 0 |

### 장시간 버퍼링·장애 테스트 (`e2e_long.sh`)
60분 주기에서 생길 수 있는 상황을 5~30초 주기로 축소해 재현했습니다. 보낸 이벤트마다 일련번호를 붙이고,
수신 서버가 **200 으로 받은** 이벤트와 대조해 유실·중복을 판정합니다.

| 테스트 | PASS | FAIL | INFO |
|--------|------|------|------|
| L1. 수신 서버 장애가 여러 주기 동안 지속 (로그는 계속 유입) | 5 | 0 | 1 |
| L2. 수신 서버가 413 (요청이 너무 큼) 반환 | 4 | 0 | 0 |
| L3. 수신 서버가 401 (인증 오류) 반환: 기본값 vs batch_retry_4xx | 5 | 0 | 1 |
| L4. 대량 backlog 가 있는 상태에서 크래시 → 재시작 | 4 | 0 | 0 |
| L5. 붙잡는 청크 수 상한 (batch_hold_max_chunks) | 3 | 0 | 0 |
| L6. 연결은 받지만 응답하지 않는 수신 서버에서 종료 | 2 | 0 | 0 |

`[INFO]` 는 판정 대상이 아닌 참고 정보입니다 (설계대로 폐기되는 4xx 로그, 재시도로 섞이는 배치 간 순서 등).

## 2. 테스트 방법

| 항목 | 내용 |
|------|------|
| 실행 환경 | Linux 컨테이너 (Ubuntu 24.04, `fb-builder-pg` 이미지) 에서 소스를 빌드해 실행 |
| DB | `postgres:16` 컨테이너 (기본 테스트 T3) |
| 로그 입력 | fluent-bit `http` 입력(9880) 에 `curl -X POST ... -d '{"ev":"..."}'` |
| 수신 서버 | `recv.pl` — 요청 헤더(`X-Batch-Id`, `Content-Type`)·응답 코드·본문을 그대로 기록. `<log>.mode` 파일로 413 / 401 / 무응답(hang) 전환 |
| 판정 | 수신 기록·청크 파일 수·결과 이벤트·DB 조회를 스크립트가 자동 비교해 `[PASS]` / `[FAIL]` 출력 |
| 시간 표시 | `[+초]` 는 각 테스트에서 fluent-bit 를 시작한 시점 기준 |

다시 실행하기:

```bash
bash documentation/examples/out_http_batch_e2e/e2e.sh        # 기본 (T1~T3, 약 2분)
bash documentation/examples/out_http_batch_e2e/e2e_long.sh   # 장시간·장애 (L1~L6, 약 10분)
```

| 파일 | 역할 |
|------|------|
| `e2e.sh`, `run.sh`, `t1.yaml`~`t3.yaml` | 기본 테스트: 호스트 실행 스크립트, 컨테이너 안 시나리오, 설정 |
| `e2e_long.sh`, `run_long.sh`, `l.yaml` | 장시간·장애 테스트 (설정 값은 시나리오마다 환경 변수로 지정) |
| `recv.pl` | 수신 서버 |
| `show.pl`, `verify.pl`, `statuses.pl` | 받은 요청 출력, 유실·중복 판정, 결과 이벤트 출력 |

## 3. 표준출력 전문 — 기본 테스트

```text
==================================================================
 out_http 배치 기능 E2E 테스트
==================================================================
  실행 시각 : 2026-09-30 22:09:06 KST
  소스      : c4a529128 out_http: add batch status events for database logging
  실행 환경 : docker 20.10.14 (linux/arm64), 이미지 fb-builder-pg:latest
  DB        : postgres:16 컨테이너 (e2e-pg)

==================================================================
 빌드
==================================================================
  Fluent Bit v5.1.0
  플러그인: out_http, in_http, in_emitter=On, out_pgsql=On
  수신 서버: recv.pl (요청 헤더·본문을 그대로 기록, 응답 후 연결 종료)
  로그 입력: curl -X POST http://127.0.0.1:9880/app -d '{"ev":"e1"}'

==================================================================
 T1. JSON 봉투 + 주기 배치 (batch_interval 5초)
==================================================================
  목적: 5초 동안 들어온 로그가 요청 1건 {"count":N,"events":[...]} 로 나가고,
        보낼 데이터가 없는 주기에는 전송하지 않는지 확인
  ▶ 설정 (t1.yaml)
    | service:
    |   flush: 1
    |   log_level: info
    | 
    | pipeline:
    |   inputs:
    |     - name: http
    |       listen: 127.0.0.1
    |       port: 9880
    |       tag: app
    | 
    |   outputs:
    |     - name: http
    |       alias: batch_out
    |       match: app
    |       host: 127.0.0.1
    |       port: 9001
    |       format: json
    |       json_events_key: events
    |       json_count_key: count
    |       json_date_key: false
    |       batch_interval: 5
    |       workers: 1
  ▶ 진행
  [+  0.0s] fluent-bit 시작 (전송 주기: +5초, +10초, +15초)
  [+  1.1s] 로그 전송: e1 e2 e3 e4 e5
  [+ 11.6s] +5초 주기 전송, +10초 주기는 보낼 데이터 없음
  [+ 11.6s] 로그 전송: e6 e7
  [+ 16.2s] +15초 주기 전송
  [+ 16.6s] fluent-bit 종료
  ▶ 수신 서버가 받은 요청
    요청 #1  (+5.6s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: {"count":5,"events":[{"ev":"e1"},{"ev":"e2"},{"ev":"e3"},{"ev":"e4"},{"ev":"e5"}]}
      → count=5, events 배열 길이=5, 순서=[e1,e2,e3,e4,e5]
    요청 #2  (+15.6s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: {"count":2,"events":[{"ev":"e6"},{"ev":"e7"}]}
      → count=2, events 배열 길이=2, 순서=[e6,e7]
  ▶ fluent-bit 로그
    [ info] [output:http:batch_out] batch mode enabled, sending every 5s (batch_max_size 0)
    [ info] [output:http:batch_out] 127.0.0.1:9001, HTTP status=200
    [ info] [output:http:batch_out] 127.0.0.1:9001, HTTP status=200
  ▶ 판정
    [PASS] 요청은 2건뿐 (주기 3번 중 보낼 데이터가 없던 +10초 주기는 전송 안 함)
    [PASS] 1번째 요청: count=5, 배열 5건, 순서 e1~e5
    [PASS] 2번째 요청: count=2, 순서 e6,e7
    [PASS] 2번째 요청은 +15초 주기에 전송 (+15.6초)
    [PASS] Content-Type: application/json

==================================================================
 T2. 유실 방지 모드 (filesystem) + 같은 입력을 쓰는 다른 output
==================================================================
  목적: 전송이 성공해야만 청크 파일이 지워지는지, 강제 종료 후 재시작하면 다시 보내는지,
        정상 종료 시 즉시 보내는지, 배치가 청크를 붙잡는 동안 다른 output 이 재전송하지 않는지
  ▶ 설정 (t2.yaml)
    | service:
    |   flush: 1
    |   grace: 10
    |   log_level: info
    |   storage.path: /work/storage2
    |   storage.sync: normal
    | 
    | pipeline:
    |   inputs:
    |     - name: http
    |       listen: 127.0.0.1
    |       port: 9880
    |       tag: app
    |       storage.type: filesystem
    | 
    |   outputs:
    |     - name: http                      # batch output, chunks held until delivered
    |       alias: batch_out
    |       match: app
    |       host: 127.0.0.1
    |       port: 9001
    |       format: json
    |       json_events_key: events
    |       json_date_key: false
    |       batch_interval: ${BATCH}
    |       batch_hold_chunks: on
    |       retry_limit: no_limits
    |       workers: 1
    | 
    |     - name: http                      # another output on the same input
    |       alias: other_out
    |       match: app
    |       host: 127.0.0.1
    |       port: 9002
    |       format: json
    |       json_date_key: false
  ▶ 진행
  -- A. 정상 주기 (batch_interval 10초)
  [+  0.0s] fluent-bit 시작 (전송 주기: +10초)
  [+  1.1s] 로그 전송: h1 h2 h3
  [+  4.1s] 배치 대기 중 디스크의 청크 파일: 1개
  [+ 11.2s] +10초 주기 전송 후 청크 파일: 0개
  -- B. 청크를 붙잡은 상태에서 kill -9 (강제 종료) → 재시작 (batch_interval 5초)
  [+ 11.2s] 로그 전송: h4 h5
  [+ 14.2s] 배치 대기 중 청크 파일: 1개
  [+ 14.3s] kill -9 후 청크 파일: 1개
  [+ 14.3s] fluent-bit 재시작
  [+ 21.3s] 재시작 후 주기 전송 뒤 청크 파일: 0개
  -- C. 청크를 붙잡은 상태에서 SIGTERM (정상 종료)
  [+ 21.3s] 로그 전송: h6
  [+ 23.4s] 배치 대기 중 청크 파일: 1개
  [+ 24.9s] SIGTERM → 종료 완료, 청크 파일: 0개
  ▶ batch_out 수신 서버가 받은 요청
    요청 #1  (+10.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: {"count":3,"events":[{"ev":"h1"},{"ev":"h2"},{"ev":"h3"}]}
      → count=3, events 배열 길이=3, 순서=[h1,h2,h3]
    요청 #2  (+19.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: {"count":2,"events":[{"ev":"h4"},{"ev":"h5"}]}
      → count=2, events 배열 길이=2, 순서=[h4,h5]
    요청 #3  (+23.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: {"count":1,"events":[{"ev":"h6"}]}
      → count=1, events 배열 길이=1, 순서=[h6]
  ▶ other_out 수신 서버가 받은 요청
    요청 #1  (+1.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: [{"ev":"h1"},{"ev":"h2"},{"ev":"h3"}]
      → (일반 전송) 배열 길이=3, 순서=[h1,h2,h3]
    요청 #2  (+11.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: [{"ev":"h4"},{"ev":"h5"}]
      → (일반 전송) 배열 길이=2, 순서=[h4,h5]
    요청 #3  (+15.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: [{"ev":"h4"},{"ev":"h5"}]
      → (일반 전송) 배열 길이=2, 순서=[h4,h5]
    요청 #4  (+21.9s)  응답 200  X-Batch-Id: -  Content-Type: application/json
      본문: [{"ev":"h6"}]
      → (일반 전송) 배열 길이=1, 순서=[h6]
  ▶ fluent-bit 로그 (재시작 후)
    [ info] [engine] flush backlog chunk '2595-1790773854.331662680.flb' succeeded: task_id=0, input=storage_backlog.1 > output=other_out (out_id=1)
    [ info] [engine] flush backlog chunk '2595-1790773854.331662680.flb' succeeded: task_id=0, input=storage_backlog.1 > output=batch_out (out_id=0)
  ▶ 판정
    [PASS] A: 배치 대기 중에는 청크 파일이 디스크에 남아 있음 (1개)
    [PASS] A: 주기 전송 성공 후 청크 파일 삭제 (0개)
    [PASS] A: batch_out 이 +10초 주기에 count=3 [h1,h2,h3] 1건 수신
    [PASS] A: other_out 은 주기와 무관하게 바로 수신 (+1.9초)
    [PASS] B: kill -9 후에도 청크 파일이 디스크에 남아 있음 (1개)
    [PASS] B: 재시작 후 batch_out 이 [h4,h5] 를 정확히 1번 수신
    [PASS] B: 재전송 성공 후 청크 파일 삭제 (0개)
    [PASS] C: SIGTERM 즉시 [h6] 전송 (SIGTERM +23.4초 → 수신 +23.9초)
    [PASS] C: 종료 후 청크 파일 삭제 (0개)
    [PASS] 배치가 청크를 붙잡는 동안 other_out 은 재전송하지 않음 ([h1,h2,h3] 1번)
    [INFO] 크래시 후 재시작 시 other_out 이 [h4,h5] 를 2번 수신 → 알려진 동작 (가이드 7.3, 피하려면 7.4 격리 구성)

==================================================================
 T3. 결과 이벤트 → PostgreSQL (batch_interval 3초)
==================================================================
  목적: 주기마다 결과(success / empty / retry_next_interval)가 PostgreSQL 에 1행씩 저장되고,
        요청의 X-Batch-Id 헤더와 DB 의 batch_id 가 같은지 확인 (DB 확인은 아래 호스트 단계)
  ▶ 설정 (t3.yaml)
    | service:
    |   flush: 1
    |   grace: 5
    |   log_level: info
    |   storage.path: /work/storage3
    | 
    | pipeline:
    |   inputs:
    |     - name: http
    |       listen: 127.0.0.1
    |       port: 9880
    |       tag: app
    | 
    |   outputs:
    |     - name: http
    |       alias: batch_out
    |       match: app
    |       host: 127.0.0.1
    |       port: 9001
    |       format: json
    |       json_events_key: events
    |       json_date_key: false
    |       batch_interval: 3
    |       batch_status_tag: batch.status
    |       batch_status_storage.type: filesystem
    |       workers: 1
    | 
    |     - name: pgsql
    |       match: batch.status
    |       host: e2e-pg
    |       port: 5432
    |       user: fluent
    |       password: fluent_test_pw
    |       database: logs
    |       table: batch_events
    |       retry_limit: no_limits
  ▶ 진행
  [+  0.0s] fluent-bit 시작 (전송 주기: 약 3초마다)
  [+  1.1s] 로그 전송: s1 s2 s3
  [+  6.6s] 수신 서버 중단 (다음 주기 전송 실패 예정)
  [+  6.6s] 로그 전송: s4 s5
  [+ 12.7s] 수신 서버 재시작
  [+ 16.9s] fluent-bit 종료
  ▶ 수신 서버가 받은 요청
    요청 #1  (+3.9s)  응답 200  X-Batch-Id: batch_out-1790773872-1-abaed6c6  Content-Type: application/json
      본문: {"count":3,"events":[{"ev":"s1"},{"ev":"s2"},{"ev":"s3"}]}
      → count=3, events 배열 길이=3, 순서=[s1,s2,s3]
    요청 #2  (+12.9s)  응답 200  X-Batch-Id: batch_out-1790773881-4-abaed6c6  Content-Type: application/json
      본문: {"count":2,"events":[{"ev":"s4"},{"ev":"s5"}]}
      → count=2, events 배열 길이=2, 순서=[s4,s5]
  ▶ fluent-bit 로그
    [ info] [input:emitter:batch_out_batch_status] initializing
    [ info] [output:http:batch_out] batch mode enabled, sending every 3s (batch_max_size 0)
    [ info] [output:pgsql:pgsql.1] host=e2e-pg port=5432 dbname=logs OK
    [ info] [output:http:batch_out] batch batch_out-1790773877-3-abaed6c6: no connection available to 127.0.0.1:9001, retrying now
  ▶ PostgreSQL 의 batch_events 테이블 (호스트에서 조회)
       time   |       status        | http_status | records | carried | attempts | ms |            batch_id             |                   error                   
    ----------+---------------------+-------------+---------+---------+----------+----+---------------------------------+-------------------------------------------
     13:11:12 | success             | 200         | 3       | 0       | 1        | 16 | batch_out-1790773872-1-abaed6c6 | 
     13:11:14 | empty               |             | 0       | 0       | 0        | 0  | batch_out-1790773874-2-abaed6c6 | 
     13:11:18 | retry_next_interval |             | 2       | 0       | 2        | 4  | batch_out-1790773877-3-abaed6c6 | no connection available to 127.0.0.1:9001
     13:11:21 | success             | 200         | 2       | 2       | 1        | 15 | batch_out-1790773881-4-abaed6c6 | 
     13:11:23 | empty               |             | 0       | 0       | 0        | 0  | batch_out-1790773883-5-abaed6c6 | 
    (5 rows)
    
  ▶ 한 행의 data (jsonb) 전체
    {
        "date": 1790773872.016534,
        "bytes": 58,
        "error": null,
        "output": "batch_out",
        "status": "success",
        "records": 3,
        "attempts": 1,
        "batch_id": "batch_out-1790773872-1-abaed6c6",
        "started_at": "2026-09-30T13:11:12.000Z",
        "duration_ms": 16,
        "http_status": 200,
        "last_record_time": "2026-09-30T13:11:09.201Z",
        "first_record_time": "2026-09-30T13:11:09.176Z",
        "carried_over_records": 0
    }
  ▶ 판정
    [PASS] 이벤트 순서: success → empty → retry_next_interval → success (success,empty,retry_next_interval,success,empty)
    [PASS] 첫 success: records=3, attempts=1, http_status=200, error=null (3|1|200|null)
    [PASS] empty: records=0, attempts=0, http_status=null (0|0|null)
    [PASS] retry_next_interval: records=2, attempts=2(즉시 재시도 포함), http_status=null, error 기록 (2|2|null|no connection available to 127.0.0.1:9001)
    [PASS] 복구 후 success: records=2, carried_over_records=2, http_status=200 (2|2|200)
    [PASS] 요청의 X-Batch-Id 헤더 = DB 의 success batch_id (batch_out-1790773872-1-abaed6c6,batch_out-1790773881-4-abaed6c6)

==================================================================
 결과: PASS 21 / FAIL 0
==================================================================
```

## 4. 표준출력 전문 — 장시간 버퍼링·장애 테스트

```text
==================================================================
 out_http 배치 기능: 장시간 버퍼링 · 장애 시나리오 테스트
==================================================================
  실행 시각 : 2026-09-30 22:03:37 KST
  소스      : c4a529128 out_http: add batch status events for database logging (+ 작업 트리 변경분)
  실행 환경 : docker 20.10.14 (linux/arm64), 이미지 fb-builder-pg:latest
  주기      : 60분 주기의 동작을 5~30초 주기로 축소해 재현
  빌드      : Fluent Bit v5.1.0

==================================================================
 공통 설정 (l.yaml, ${...} 는 시나리오마다 환경 변수로 지정)
==================================================================
    | service:
    |   flush: ${FLUSH}
    |   grace: 10
    |   log_level: info
    |   scheduler.cap: 10
    |   storage.path: /work/lstore
    |   storage.sync: normal
    |   storage.backlog.mem_limit: ${BL_MEM}
    | 
    | pipeline:
    |   inputs:
    |     - name: http
    |       listen: 127.0.0.1
    |       port: 9880
    |       tag: app
    |       storage.type: filesystem
    | 
    |   outputs:
    |     - name: http
    |       alias: batch_out
    |       match: app
    |       host: 127.0.0.1
    |       port: 9001
    |       format: json
    |       json_events_key: events
    |       json_date_key: false
    |       batch_interval: ${INTERVAL}
    |       batch_hold_chunks: on
    |       batch_max_size: ${MAX_SIZE}
    |       batch_hold_max_chunks: ${MAX_CHUNKS}
    |       batch_retry_4xx: ${RETRY_4XX}
    |       batch_status_tag: batch.status
    |       retry_limit: no_limits
    |       http.response_timeout: ${RESP_TIMEOUT}
    |       workers: 1
    | 
    |     - name: stdout
    |       match: batch.status
    |       format: json_lines
    |       json_date_key: false

==================================================================
 L1. 수신 서버 장애가 여러 주기 동안 지속 (로그는 계속 유입)
==================================================================
  목적: 장애 동안 배치가 보관되고 가득 차면 나머지는 디스크에서 대기, 복구 후 유실·중복 없이 전부 전송되는지
  ▶ 설정 값: batch_interval=10s flush=1s batch_max_size=4K batch_hold_max_chunks=512 batch_retry_4xx=off http.response_timeout=10s storage.backlog.mem_limit=100M
  [+  0.0s] fluent-bit 시작 (주기 10초)
  [+  0.1s] 로그 유입 시작 (초당 약 5건, 150건)
  [+  4.2s] 수신 서버 다운 (+10, +20, +30초 주기 실패 예정)
  [+ 32.2s] 수신 서버 복구
  [+ 36.9s] 로그 유입 종료 (150건 입력)
  [+ 50.6s] 모든 이벤트 도착
  [+ 51.3s] fluent-bit 종료
  ▶ 수신 서버가 받은 요청 (본문 생략)
        → count=124, events 배열 길이=124, 순서=[L1-1,L1-2,L1-3,L1-4,L1-5,L1-6,L1-7,L1-8,L1-9,L1-10,L1-11,L1-12,L1-13,L1-14,L1-15,L1-16,L1-17,L1-18,L1-19,L1-20,L1-21,L1-22,L1-23,L1-24,L1-25,L1-26,L1-27,L1-28,L1-29,L1-30,L1-31,L1-32,L1-33,L1-34,L1-35,L1-36,L1-37,L1-38,L1-39,L1-40,L1-41,L1-42,L1-43,L1-44,L1-45,L1-46,L1-47,L1-48,L1-49,L1-50,L1-51,L1-52,L1-53,L1-54,L1-55,L1-56,L1-57,L1-58,L1-59,L1-60,L1-61,L1-62,L1-63,L1-64,L1-65,L1-66,L1-67,L1-68,L1-69,L1-70,L1-71,L1-72,L1-73,L1-74,L1-75,L1-76,L1-77,L1-78,L1-79,L1-80,L1-81,L1-82,L1-83,L1-84,L1-85,L1-86,L1-87,L1-88,L1-89,L1-90,L1-91,L1-92,L1-93,L1-94,L1-95,L1-96,L1-97,L1-98,L1-99,L1-100,L1-101,L1-102,L1-103,L1-104,L1-105,L1-106,L1-107,L1-108,L1-109,L1-110,L1-111,L1-112,L1-113,L1-114,L1-115,L1-116,L1-117,L1-118,L1-119,L1-120,L1-121,L1-122,L1-123,L1-124]
        → count=26, events 배열 길이=26, 순서=[L1-133,L1-134,L1-135,L1-136,L1-129,L1-130,L1-131,L1-132,L1-137,L1-138,L1-139,L1-140,L1-145,L1-146,L1-147,L1-148,L1-141,L1-142,L1-143,L1-144,L1-149,L1-150,L1-125,L1-126,L1-127,L1-128]
  ▶ 결과 이벤트
    retry_next_interval  records=40   carried=0    attempts=2 http=null error=no connection available to 127.0.0.1:9001
    retry_next_interval  records=81   carried=40   attempts=2 http=null error=no connection available to 127.0.0.1:9001
    retry_next_interval  records=120  carried=81   attempts=2 http=null error=no connection available to 127.0.0.1:9001
    success              records=124  carried=120  attempts=1 http=200  error=null
    success              records=26   carried=0    attempts=1 http=200  error=null
  ▶ fluent-bit 로그 (배치 결과)
    [ info] [output:http:batch_out] batch batch_out-1790773502-1-9d90c70a: no connection available to 127.0.0.1:9001, retrying now
    [ warn] [output:http:batch_out] batch batch_out-1790773502-1-9d90c70a failed: no connection available to 127.0.0.1:9001; 40 records (10 chunks) kept in storage for the next interval in 10s
    [ info] [output:http:batch_out] batch batch_out-1790773512-2-9d90c70a: no connection available to 127.0.0.1:9001, retrying now
    [ warn] [output:http:batch_out] batch batch_out-1790773512-2-9d90c70a failed: no connection available to 127.0.0.1:9001; 81 records (20 chunks) kept in storage for the next interval in 10s
    [ info] [output:http:batch_out] batch batch_out-1790773522-3-9d90c70a: no connection available to 127.0.0.1:9001, retrying now
    [ warn] [output:http:batch_out] batch batch_out-1790773522-3-9d90c70a failed: no connection available to 127.0.0.1:9001; 120 records (30 chunks) kept in storage for the next interval in 10s
    [ warn] [output:http:batch_out] batch is full (31 chunks, 3984 bytes; batch_hold_max_chunks 512, batch_max_size 4000): further chunks stay in storage and are retried by the engine
    [ info] [output:http:batch_out] batch batch_out-1790773532-4-9d90c70a sent: 124 records (31 chunks), 3016 bytes, HTTP 200, 1 attempt(s), 21 ms
    [ info] [output:http:batch_out] batch batch_out-1790773542-5-9d90c70a sent: 26 records (7 chunks), 673 bytes, HTTP 200, 1 attempt(s), 10 ms
  ▶ 판정
    [PASS] 유실 0 · 중복 0 (보낸 이벤트 150건, 받은 이벤트 150건, 유실 0건, 중복 0건)
    [PASS] 장애 동안 retry_next_interval 로 보관
    [PASS] 폐기(dropped) 없음
    [PASS] 'batch is full' 경고는 주기당 1번 이하 (경고 1번 / 주기 5번)
    [PASS] 모든 요청에서 count == events 배열 길이
    [INFO] 요청 간 이벤트 순서: 섞임 (가득 차서 엔진 재시도로 넘어간 청크는 재시도 순서대로 도착, 청크 안의 순서는 유지)

==================================================================
 L2. 수신 서버가 413 (요청이 너무 큼) 반환
==================================================================
  목적: 413 을 받아도 배치를 버리지 않고 보관하는지 (원래 4xx 는 폐기 대상)
  ▶ 설정 값: batch_interval=5s flush=1s batch_max_size=0 batch_hold_max_chunks=512 batch_retry_4xx=off http.response_timeout=10s storage.backlog.mem_limit=100M
  [+  0.0s] fluent-bit 시작 (주기 5초), 수신 서버는 413 응답
  [+  1.2s] 로그 20건 입력
  [+ 12.3s] 수신 서버 정상화 (200 응답)
  [+ 16.5s] 모든 이벤트 도착
  ▶ 결과 이벤트
    retry_next_interval  records=20   carried=0    attempts=1 http=413  error=HTTP status 413
    retry_next_interval  records=20   carried=20   attempts=1 http=413  error=HTTP status 413
    success              records=20   carried=20   attempts=1 http=200  error=null
  ▶ fluent-bit 로그
    [error] [output:http:batch_out] 127.0.0.1:9001, HTTP status=413
    [error] [output:http:batch_out] batch batch_out-1790773549-1-ab5df0c2: request of 494 bytes rejected with HTTP 413, set 'batch_max_size' below the receiver limit
    [ warn] [output:http:batch_out] batch batch_out-1790773549-1-ab5df0c2 failed: HTTP status 413; 20 records (2 chunks) kept in storage for the next interval in 5s
{"batch_id":"batch_out-1790773549-1-ab5df0c2","output":"batch_out","status":"retry_next_interval","http_status":413,"records":20,"carried_over_records":0,"bytes":494,"attempts":1,"started_at":"2026-09-30T13:05:49.031Z","duration_ms":29,"first_record_time":"2026-09-30T13:05:43.230Z","last_record_time":"2026-09-30T13:05:44.367Z","error":"HTTP status 413"}
    [error] [output:http:batch_out] 127.0.0.1:9001, HTTP status=413
    [error] [output:http:batch_out] batch batch_out-1790773554-2-ab5df0c2: request of 494 bytes rejected with HTTP 413, set 'batch_max_size' below the receiver limit
    [ warn] [output:http:batch_out] batch batch_out-1790773554-2-ab5df0c2 failed: HTTP status 413; 20 records (2 chunks) kept in storage for the next interval in 5s
{"batch_id":"batch_out-1790773554-2-ab5df0c2","output":"batch_out","status":"retry_next_interval","http_status":413,"records":20,"carried_over_records":20,"bytes":494,"attempts":1,"started_at":"2026-09-30T13:05:54.029Z","duration_ms":22,"first_record_time":"2026-09-30T13:05:43.230Z","last_record_time":"2026-09-30T13:05:44.367Z","error":"HTTP status 413"}
  ▶ 판정
    [PASS] 유실 0 · 중복 0 (보낸 이벤트 20건, 받은 이벤트 20건, 유실 0건, 중복 0건)
    [PASS] 413 을 받은 주기는 retry_next_interval (http=413)
    [PASS] 폐기(dropped) 없음
    [PASS] 원인과 조치가 에러 로그에 남음 (set 'batch_max_size' below the receiver limit)

==================================================================
 L3. 수신 서버가 401 (인증 오류) 반환: 기본값 vs batch_retry_4xx
==================================================================
  목적: 기본값은 기존 out_http 처럼 4xx 배치를 폐기하고 그 사실을 로그·이벤트로 남기는지,
        batch_retry_4xx: on 이면 보관했다가 인증이 복구된 뒤 보내는지
  ▶ 설정 값: batch_interval=5s flush=1s batch_max_size=0 batch_hold_max_chunks=512 batch_retry_4xx=off http.response_timeout=10s storage.backlog.mem_limit=100M
  -- a. 기본값 (batch_retry_4xx: off)
  [+  0.0s] fluent-bit 시작, 수신 서버는 401 응답
  [+  0.7s] 로그 10건 입력
  [+  6.8s] 인증 복구 (200 응답)
  [+  7.1s] 로그 5건 추가 입력
  ▶ 결과 이벤트
    dropped              records=10   carried=0    attempts=1 http=401  error=HTTP status 401
    success              records=5    carried=0    attempts=1 http=200  error=null
  ▶ fluent-bit 로그
    [error] [output:http:batch_out] batch batch_out-1790773566-1-41a73efb dropped: HTTP status 401; 10 records (1 chunks) are lost
    [ info] [output:http:batch_out] batch batch_out-1790773571-2-41a73efb sent: 5 records (2 chunks), 142 bytes, HTTP 200, 1 attempt(s), 25 ms
  -- b. batch_retry_4xx: on
  ▶ 설정 값: batch_interval=5s flush=1s batch_max_size=0 batch_hold_max_chunks=512 batch_retry_4xx=on http.response_timeout=10s storage.backlog.mem_limit=100M
  [+  0.0s] fluent-bit 시작, 수신 서버는 401 응답
  [+  0.7s] 로그 10건 입력
  [+  6.8s] 인증 복구 (200 응답)
  [+ 11.0s] 모든 이벤트 도착
  ▶ 결과 이벤트
    retry_next_interval  records=10   carried=0    attempts=1 http=401  error=HTTP status 401
    success              records=10   carried=10   attempts=1 http=200  error=null
  ▶ fluent-bit 로그
    [ warn] [output:http:batch_out] batch batch_out-1790773578-1-918f38f2 failed: HTTP status 401; 10 records (1 chunks) kept in storage for the next interval in 5s
    [ info] [output:http:batch_out] batch batch_out-1790773583-2-918f38f2 sent: 10 records (1 chunks), 264 bytes, HTTP 200, 1 attempt(s), 24 ms
  ▶ 판정
    [PASS] a: 401 배치는 dropped 로 기록 (http=401)
    [PASS] a: 폐기 사실이 에러 로그에 남음 (... are lost)
    [PASS] a: 인증 복구 뒤 새 로그는 정상 전송 (보낸 이벤트 5건, 받은 이벤트 5건, 유실 0건, 중복 0건)
    [INFO] a: 401 을 받은 로그는 기본값에서 폐기됨 (설계대로) → 보낸 이벤트 10건, 받은 이벤트 0건, 유실 10건, 중복 0건 (유실 예: L3a-1,L3a-10,L3a-2,L3a-3,L3a-4)
    [PASS] b: batch_retry_4xx 면 retry_next_interval 로 보관 (http=401)
    [PASS] b: 인증 복구 뒤 전부 전송, 유실 0 · 중복 0 (보낸 이벤트 10건, 받은 이벤트 10건, 유실 0건, 중복 0건)

==================================================================
 L4. 대량 backlog 가 있는 상태에서 크래시 → 재시작
==================================================================
  목적: 재시작 후 디스크의 backlog 가 첫 주기에 전송되는지, storage.backlog.mem_limit 을 아주 작게(16K) 해도 같은지
  ▶ 설정 값: batch_interval=30s flush=1s batch_max_size=0 batch_hold_max_chunks=512 batch_retry_4xx=off http.response_timeout=10s storage.backlog.mem_limit=16K
  [+  0.0s] fluent-bit 시작 (주기 30초)
  [+  3.6s] 로그 200건 입력 (건당 약 400바이트)
  [+  5.6s] 청크 파일 4개 보관 중 → kill -9
  [+  0.0s] 재시작 (주기 5초, storage.backlog.mem_limit=16K)
  [+  5.3s] 모든 이벤트 도착
  ▶ 재시작 후 결과 이벤트
    success              records=200  carried=0    attempts=1 http=200  error=null
  ▶ 설정 값: batch_interval=30s flush=1s batch_max_size=0 batch_hold_max_chunks=512 batch_retry_4xx=off http.response_timeout=10s storage.backlog.mem_limit=100M
  [+  0.0s] fluent-bit 시작 (주기 30초)
  [+  3.5s] 로그 200건 입력 (건당 약 400바이트)
  [+  5.6s] 청크 파일 4개 보관 중 → kill -9
  [+  0.0s] 재시작 (주기 5초, storage.backlog.mem_limit=100M)
  [+  6.4s] 모든 이벤트 도착
  ▶ 재시작 후 결과 이벤트
    success              records=200  carried=0    attempts=1 http=200  error=null
  ▶ 판정
    [PASS] mem_limit 16K: 유실 0 · 중복 0 (보낸 이벤트 200건, 받은 이벤트 200건, 유실 0건, 중복 0건)
    [PASS] mem_limit 100M: 유실 0 · 중복 0 (보낸 이벤트 200건, 받은 이벤트 200건, 유실 0건, 중복 0건)
    [PASS] mem_limit 100M: 재시작 후 첫 주기에 backlog 전부 전송 (성공 1번)
    [PASS] mem_limit 16K: 한도가 작아도 첫 주기에 backlog 전부 전송 (성공 1번)

==================================================================
 L5. 붙잡는 청크 수 상한 (batch_hold_max_chunks)
==================================================================
  목적: 상한을 넘는 청크는 디스크에서 대기했다가 이후 주기에 유실 없이 전송되는지
  ▶ 설정 값: batch_interval=10s flush=0.5s batch_max_size=0 batch_hold_max_chunks=5 batch_retry_4xx=off http.response_timeout=10s storage.backlog.mem_limit=100M
  [+  0.0s] fluent-bit 시작 (주기 10초, flush 0.5초 → 주기당 청크 약 20개, 상한 5)
  [+ 24.1s] 로그 100건 입력
  [+111.2s] 모든 이벤트 도착
  ▶ fluent-bit 로그
    [ warn] [output:http:batch_out] about 20 chunks per tag are flushed in one batch_interval (10s / flush 0.5s), more than batch_hold_max_chunks (5): the rest waits in storage for a later interval, consider a larger 'flush'
    [ warn] [output:http:batch_out] batch is full (5 chunks, 279 bytes; batch_hold_max_chunks 5, batch_max_size 0): further chunks stay in storage and are retried by the engine
    [ info] [output:http:batch_out] batch batch_out-1790773622-1-0c1bc3cb sent: 9 records (5 chunks), 229 bytes, HTTP 200, 1 attempt(s), 18 ms
    [ warn] [output:http:batch_out] batch is full (5 chunks, 352 bytes; batch_hold_max_chunks 5, batch_max_size 0): further chunks stay in storage and are retried by the engine
    [ info] [output:http:batch_out] batch batch_out-1790773632-2-0c1bc3cb sent: 11 records (5 chunks), 287 bytes, HTTP 200, 1 attempt(s), 14 ms
    [ warn] [output:http:batch_out] batch is full (5 chunks, 352 bytes; batch_hold_max_chunks 5, batch_max_size 0): further chunks stay in storage and are retried by the engine
    [ info] [output:http:batch_out] batch batch_out-1790773642-3-0c1bc3cb sent: 11 records (5 chunks), 287 bytes, HTTP 200, 1 attempt(s), 20 ms
    [ warn] [output:http:batch_out] batch is full (5 chunks, 289 bytes; batch_hold_max_chunks 5, batch_max_size 0): further chunks stay in storage and are retried by the engine
    [ info] [output:http:batch_out] batch batch_out-1790773652-4-0c1bc3cb sent: 9 records (5 chunks), 239 bytes, HTTP 200, 1 attempt(s), 16 ms
    [ warn] [output:http:batch_out] batch is full (5 chunks, 320 bytes; batch_hold_max_chunks 5, batch_max_size 0): further chunks stay in storage and are retried by the engine
  ▶ 판정
    [PASS] 시작 시 설정 점검 경고 (chunks per tag ... more than batch_hold_max_chunks)
    [PASS] 한 배치의 청크 수는 상한 이하 (최대 5개)
    [PASS] 유실 0 · 중복 0 (보낸 이벤트 100건, 받은 이벤트 100건, 유실 0건, 중복 0건)

==================================================================
 L6. 연결은 받지만 응답하지 않는 수신 서버에서 종료
==================================================================
  목적: 종료가 응답 제한 시간 때문에 얼마나 늦어지는지, 보내지 못한 로그가 재시작 후 전송되는지
  ▶ 설정 값: batch_interval=30s flush=1s batch_max_size=0 batch_hold_max_chunks=512 batch_retry_4xx=off http.response_timeout=5s storage.backlog.mem_limit=100M
  [+  0.0s] fluent-bit 시작, 수신 서버는 응답하지 않음
  [+  0.7s] 로그 10건 입력
  [+ 16.0s] SIGTERM → 13.2초 만에 종료, 청크 파일 1개 남음
  [+ 16.0s] 수신 서버 정상화 후 재시작
  [+ 22.3s] 모든 이벤트 도착
  ▶ fluent-bit 로그 (종료 중)
    [ warn] [output:http:batch_out] batch batch_out-1790773726-1-c9baea21 failed on shutdown: request to 127.0.0.1:9001 failed (connection error or timeout); 10 records (1 chunks) stay in storage and are sent again after a restart
    [ info] [engine] service has stopped (1 pending tasks)
    [ info] [output:http:batch_out] thread worker #0 stopping...
    [ warn] [output:http:batch_out] batch batch_out-1790773733-2-c9baea21 failed on shutdown: request to 127.0.0.1:9001 failed (connection error or timeout); 10 records (1 chunks) stay in storage and are sent again after a restart
    [ info] [output:http:batch_out] thread worker #0 stopped
    [ info] [output:stdout:stdout.1] thread worker #0 stopping...
  ▶ 판정
    [PASS] 종료 시간이 grace(10초) + 응답 제한(5초) 이내 (13.2초)
    [PASS] 보내지 못한 로그는 재시작 후 전송, 유실 0 · 중복 0 (보낸 이벤트 10건, 받은 이벤트 10건, 유실 0건, 중복 0건)

==================================================================
 결과: PASS 23 / FAIL 0
==================================================================
```
