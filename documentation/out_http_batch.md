# out_http 배치 전송 사용 가이드

fluent-bit `http` output 에 추가한 기능을 **어떻게 설정하고, 어떻게 동작하는지** 한 곳에 정리한 문서입니다.

| 기능 | 하는 일 | 핵심 옵션 |
|------|---------|-----------|
| JSON 봉투 | 요청 본문을 `{"count":N,"events":[...]}` 형태로 | `json_events_key`, `json_count_key` |
| 주기 배치 | 로그를 모았다가 **주기마다 요청 1건**으로 전송 | `batch_interval`, `batch_max_size` |
| 유실 방지 | 전송이 성공해야만 버퍼(청크 파일)를 삭제 | `batch_hold_chunks` + `storage.type: filesystem` |

```
tail ─▶ 청크 ─▶ out_http ──(5분 동안 모음)──▶ POST 1건 {"count":1234,"events":[...]}
```

---

## 1. 빠른 시작

```yaml
service:
  grace: 30                      # 종료할 때 남은 로그를 보낼 시간(초)

pipeline:
  inputs:
    - name: tail
      path: /var/log/app/*.log
      tag: app

  outputs:
    - name: http
      match: app
      host: collector.example.com
      port: 8080
      uri: /ingest
      format: json
      json_events_key: events    # {"count":N,"events":[...]} 로 감싸기
      batch_interval: 5m         # 5분마다 1번 전송
      batch_max_size: 50M        # 모아 둘 최대 크기
      workers: 1
```

```bash
fluent-bit -c fluent-bit.yaml
```

수신 서버가 5분마다 받는 요청:

```http
POST /ingest HTTP/1.1
Content-Type: application/json

{"count":3,"events":[
  {"date":1759132801.12,"log":"user login ok"},
  {"date":1759132950.40,"log":"order created"},
  {"date":1759133100.77,"log":"payment done"}]}
```

---

## 2. 로그가 전송되기까지

```
 ① 수집          ② 엔진 전달       ③ 모으기             ④ 변환·전송          ⑤ 결과 처리
 tail 이 읽음 ─▶ flush(1초)마다 ─▶ out_http 가 보내지 ─▶ 주기가 되면 JSON ─▶ 성공: 끝
 → 청크에 저장    out_http 로 넘김   않고 버퍼에 쌓음     1건으로 만들어 POST   실패: 즉시 재시도
                                                                          → 다음 주기에 합쳐서
```

### ① 수집
`tail` 이 한 줄을 읽어 **레코드**(시각 + 내용)로 만들고 **청크**(fluent-bit 내부 데이터 묶음)에 쌓습니다.
`storage.type: filesystem` 이면 청크는 디스크 파일로도 저장됩니다.

### ② 엔진 전달 (`flush`, 기본 1초)
엔진이 1초마다 청크를 output 에 넘깁니다. 원래는 여기서 바로 요청이 나가지만 배치 모드에서는 ③으로 갑니다.

### ③ 모으기
| 모드 | 하는 일 | 엔진의 청크 |
|------|---------|-------------|
| 기본 | 로그를 메모리 버퍼에 **복사**하고 "받았음" 응답 | 곧바로 삭제 |
| 유실 방지 (`batch_hold_chunks`) | 복사하지 않고 **"받았음" 응답을 미룸** | 응답할 때까지 (디스크에) 남아 있음 |

Tag 나 청크가 여러 개여도 모두 **한 배치**로 모입니다.

### ④ 변환·전송
out_http 안의 타이머가 **1초마다 "주기가 됐나?"를 확인**합니다 (서버 시계 변경에 영향받지 않는 단조 시계 사용).

```
쌓인 레코드  [t1,{"log":"a"}] [t2,{"log":"b"}] [t3,{"log":"c"}]
   │ (1) JSON 배열 변환        [{"date":t1,"log":"a"}, ...]      (시각은 json_date_key 필드로)
   │ (2) 개수 세고 봉투로 감쌈  {"count":3,"events":[...]}
   │ (3) 필요하면 압축          compress: gzip
   ▼ (4) POST 1건
```

- `count` 는 배열에 실제로 들어간 레코드 수와 항상 같고, 순서는 들어온 순서 그대로입니다.
- 보낼 로그가 없는 주기에는 요청을 보내지 않습니다.

### ⑤ 결과 처리
| 결과 | 기본 모드 | 유실 방지 모드 |
|------|-----------|----------------|
| 성공 (2xx) | 버퍼 비움 | "받았음" 응답 → 엔진이 청크(파일) 삭제 |
| 네트워크 오류·5xx·408·429 | **새 연결로 즉시 1회 재시도** → 그래도 실패하면 다음 주기 로그와 합쳐 재전송 | 같음 (청크는 계속 보관) |
| 413 (요청이 너무 큼) | **폐기하지 않고** 다음 주기에 재전송 + 에러 로그로 조치 안내 | 같음 |
| 그 외 4xx | 폐기 (`batch_retry_4xx: on` 이면 보관 후 다음 주기 재전송) | 같음 |

- 서버가 오래 쉬던 keep-alive 연결을 끊어도, 실패한 연결은 재사용하지 않으므로 즉시 재시도가 새 연결로 성공합니다.
- 종료 중에는 즉시 재시도를 하지 않습니다 (응답하지 않는 서버 때문에 종료가 길어지지 않도록).

### ⑥ 종료할 때
| 상황 | 동작 |
|------|------|
| 정상 종료 (SIGTERM) | 주기를 기다리지 않고 1초 안에 즉시 전송 |
| 강제 종료 (kill -9, crash) | 기본 모드: 버퍼의 로그 유실 (최대 한 주기) / 유실 방지 모드: 재시작 후 다시 전송 |

### 시간 흐름 예 (5분 주기)
```
10:00:00  시작 (다음 전송 10:05:00)
10:00:01  로그 A → 버퍼        10:02:30  로그 B, C → 버퍼
10:05:00  ★ {"count":3,"events":[A,B,C]} POST 1건
10:10:00  ★ 보낼 것 없음 → 전송 안 함
10:12:00  로그 D → 버퍼
10:15:00  ★ {"count":1,"events":[D]} POST 1건
```
주기는 fluent-bit **시작 시점 기준**입니다 (정각 정렬 아님).

---

## 3. `flush` 와 `batch_interval`

| 설정 | 위치 | 역할 | 권장 |
|------|------|------|------|
| `flush` | `service` | 엔진이 로그를 out_http 버퍼로 **넘기는** 주기 | 기본 모드: 설정 안 함(1초) / 유실 방지: 5~10초 (6.3) |
| `batch_interval` | `outputs` | 실제로 서버에 **보내는** 주기 | 원하는 주기 (예: `5m`) |

`flush` 를 `batch_interval` 만큼 크게 잡으면 로그가 버퍼에 늦게 들어와 전송이 한 주기씩 밀릴 수 있습니다.

---

## 4. 옵션

### 4.1 이 기능으로 추가된 옵션 (`outputs` → `name: http`)

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `json_events_key` | 없음 | `format: json` 일 때 배열을 이 키로 감쌈 → `{"count":N,"<키>":[...]}`. 없으면 `[...]` 배열 그대로 |
| `json_count_key` | `count` | 개수 필드 이름. `false` 면 개수 필드 생략 |
| `batch_interval` | `0` (끔) | 전송 주기. `30s`, `1m`, `5m`, `10m`, `300`(초) |
| `batch_max_size` | `0` (무제한) | 모아 둘 최대 크기. `10M`, `50M`. **설정 권장** |
| `batch_hold_chunks` | `off` | `on` 이면 전송 성공 후에만 청크 삭제 (6장) |
| `batch_hold_max_chunks` | `512` | 유실 방지 모드에서 한 번에 붙잡는 청크 수 상한. 넘는 청크는 디스크에서 대기 후 이후 주기에 전송. `0` 은 무제한 (6.5) |
| `batch_retry_4xx` | `off` | `on` 이면 4xx 로 거부된 배치도 폐기하지 않고 다음 주기에 재전송 (413 은 설정과 무관하게 항상 보관) |

### 4.2 함께 쓰면 좋은 옵션

| 옵션 | 위치 | 설명 |
|------|------|------|
| `workers` | `outputs` | 배치 모드는 **1로 고정**됩니다 (2 이상이면 경고 후 1). `0` 도 동작 |
| `retry_limit` | `outputs` | `batch_max_size` 나 `batch_hold_chunks` 를 쓰면 **`no_limits` 권장** (8.3) |
| `grace` | `service` | 종료 시 대기 시간 (기본 5초). `30` 정도 권장 |
| `compress` | `outputs` | `gzip` — 한 주기 분량이 클 때 |
| `json_date_key` / `json_date_format` | `outputs` | 각 이벤트의 시간 필드 이름(`false` 면 생략) / `double`, `epoch`, `iso8601`, `java_sql_timestamp` |
| `header`, `tls`, `http_user` 등 | `outputs` | 기존 out_http 옵션 그대로 사용 가능 |

`body_key` 모드(레코드별 개별 요청)와는 함께 쓸 수 없습니다 (경고 후 배치 해제).

---

## 5. 설정 예제

### 5.1 1분마다, 개수·시간 필드 없이
```yaml
  outputs:
    - name: http
      match: '*'
      host: collector.example.com
      port: 8080
      format: json
      json_events_key: events
      json_count_key: false
      json_date_key: false
      batch_interval: 1m
      batch_max_size: 20M
      workers: 1
```
→ `{"events":[{"log":"..."}, ...]}`

### 5.2 10분마다, HTTPS + gzip + 인증 헤더 + 필드 이름 변경
```yaml
service:
  grace: 60

pipeline:
  inputs:
    - name: tail
      path: /var/log/app/*.log
      tag: app.*

  outputs:
    - name: http
      match: app.*
      host: collector.example.com
      port: 443
      uri: /api/v1/logs
      tls: on
      tls.verify: on
      compress: gzip
      header:
        - Authorization Bearer my-token
        - X-Source fluent-bit
      format: json
      json_events_key: logs
      json_count_key: total
      json_date_key: timestamp
      json_date_format: iso8601
      batch_interval: 10m
      batch_max_size: 100M
      retry_limit: no_limits
      workers: 1
```
→ `{"total":5021,"logs":[{"timestamp":"2026-09-30T13:00:00.000Z", ...}, ...]}` (gzip 압축)

### 5.3 여러 로그(Tag)를 하나의 요청으로
`match` 에 걸리는 모든 Tag 가 같은 배치에 합쳐집니다. Tag 별로 따로 보내려면 output 을 Tag 마다 정의하세요.
```yaml
  inputs:
    - name: tail
      path: /var/log/app/*.log
      tag: logs.app
    - name: tail
      path: /var/log/nginx/access.log
      tag: logs.nginx
  outputs:
    - name: http
      match: logs.*
      host: collector.example.com
      port: 8080
      format: json
      json_events_key: events
      batch_interval: 5m
      workers: 1
```

### 5.4 classic(.conf) 형식
```ini
[SERVICE]
    Grace            30

[INPUT]
    Name             tail
    Path             /var/log/app/*.log
    Tag              app

[OUTPUT]
    Name             http
    Match            app
    Host             collector.example.com
    Port             8080
    Format           json
    Json_Events_Key  events
    Batch_Interval   5m
    Batch_Max_Size   50M
    Workers          1
```

---

## 6. 유실 방지 모드 (`batch_hold_chunks` + 파일시스템 스토리지)

기본 모드는 로그를 메모리로 복사하고 청크를 바로 지웁니다. 그래서 전송 전에 프로세스가 죽으면 최대 한 주기 분량이 사라집니다.
유실 방지 모드는 **전송이 성공할 때까지 청크를 붙잡아 두어**, 죽어도 재시작 후 다시 보냅니다.

```
로그 → 청크(디스크) ──붙잡음──▶ [5분 대기] ──▶ POST 성공 ──▶ 청크 파일 삭제
                                   │
                        kill -9 ───┘ → 파일이 남음 → 재시작 → 자동으로 다시 전송
```

### 6.1 설정
```yaml
service:
  flush: 5                                   # 붙잡는 청크 수를 줄이기 위해 (6.3)
  grace: 30
  storage.path: /var/lib/fluent-bit/storage  # 청크 파일 위치
  storage.sync: normal

pipeline:
  inputs:
    - name: tail
      path: /var/log/app/*.log
      tag: app
      db: /var/lib/fluent-bit/tail.db        # 파일 읽은 위치 기록
      storage.type: filesystem               # 청크를 디스크에 저장

  outputs:
    - name: http
      match: app
      host: collector.example.com
      port: 8080
      format: json
      json_events_key: events
      batch_interval: 5m
      batch_hold_chunks: on                  # 전송 성공 후에만 삭제
      batch_max_size: 50M
      retry_limit: no_limits
      workers: 1
```
필수 조합: `storage.path` + `storage.type: filesystem` + `batch_hold_chunks: on`.
재시작하면 `storage.path` 에 남은 청크를 자동으로 읽어 다시 배치에 넣습니다 (추가 설정 없음).

### 6.2 기본 모드와 비교
| 항목 | 기본 | 유실 방지 |
|------|------|-----------|
| 로그 보관 위치 | out_http 메모리 (복사본) | fluent-bit 청크 (디스크) |
| 청크 삭제 시점 | 버퍼에 넣는 즉시 | **전송 성공 후** |
| 강제 종료 | 최대 한 주기 유실 | 재시작 후 재전송 |
| 전송 실패 | 다음 주기에 합쳐 재전송 | 청크를 계속 붙잡고 다음 주기에 재전송 |
| 추가 자원 | 버퍼 메모리 | 붙잡은 청크당 대기 코루틴 64KB + 청크 메모리 |
| 다른 output 에 영향 | 없음 | 7장 참고 |

### 6.3 `flush` 값 정하기
```
붙잡는 청크 수 ≈ batch_interval(초) ÷ flush(초) × Tag 수
```
| batch_interval | 권장 flush | 붙잡는 청크 (Tag 1개) |
|----------------|-----------|------------------------|
| 1m | 1~2 | 30~60 |
| 5m | 5 | 60 |
| 10m | 10 | 60 |

**100개 이하**를 권장합니다 (`storage.max_chunks_up` 기본 128).

### 6.4 알아둘 점
- **중복 가능 (at-least-once):** 전송 성공 직후, 청크가 지워지기 전에 죽으면 재시작 후 한 번 더 전송됩니다.
- **`retry_limit: no_limits` 권장:** 배치가 가득 찼을 때와 종료 중 전송이 실패했을 때는 청크를 엔진에 돌려보냅니다. 기본값(1)이면 두 번째 실패에서 엔진이 청크를 **삭제**합니다 (시작 시 경고 출력).
- **메모리 스토리지와 함께 쓰지 마세요:** `storage.type: memory` + `mem_buf_limit` 이면 붙잡힌 청크가 한도를 채워 입력이 멈출 수 있습니다 (7.3).
- **macOS 에서 실행하는 바이너리:** 종료 시 엔진 스레드를 강제로 멈추는 코어 동작 때문에 SIGTERM 후 청크 파일이 남아, 재시작 시 한 번 더 전송됩니다. Linux 는 정상 (삭제까지 처리).

### 6.5 긴 주기 (최대 60분) 운영

60분처럼 긴 주기는 **유실 방지 모드 + 파일시스템 스토리지**로 운영하세요. 기본 모드는 한 시간 분량을 메모리에만 들고 있다가, 강제 종료되면 전부 잃습니다.

#### 권장 설정 (60분 주기)

```yaml
service:
  flush: 10                          # 붙잡는 청크 수 ≈ 3600 ÷ 10 = 360 (tag 당)
  grace: 60                          # 종료 시 전송 대기 (http.response_timeout 보다 크게)
  scheduler.cap: 300                 # 대기 청크 재시도 간격 상한 (기본 2000초 ≈ 33분)
  storage.path: /var/lib/fluent-bit/storage
  storage.sync: normal
  storage.backlog.mem_limit: 512M    # 한 주기 분량보다 크게 (아래 '재시작 후 backlog')

pipeline:
  inputs:
    - name: tail
      path: /var/log/app/*.log
      tag: app
      db: /var/lib/fluent-bit/tail.db
      storage.type: filesystem

  outputs:
    - name: http
      match: app
      host: collector.example.com
      port: 8080
      uri: /ingest
      format: json
      json_events_key: events
      compress: gzip                 # 한 시간 분량 요청을 줄임
      batch_interval: 60m
      batch_hold_chunks: on
      batch_max_size: 200M           # 수신 서버 본문 한도보다 작게 (JSON 은 msgpack 의 약 1.5~2배)
      batch_hold_max_chunks: 512
      http.response_timeout: 120s    # 큰 요청을 받는 서버의 처리 시간 고려
      retry_limit: no_limits
      workers: 1
```

#### 크기와 자원 계산

| 항목 | 계산 | 60분·flush 10초 예 |
|------|------|--------------------|
| 붙잡는 청크 수 | `batch_interval ÷ flush × tag 수` (청크는 2MB 를 넘으면 나뉨) | tag 1개: 약 360개 |
| 붙잡는 청크당 자원 | 파일 핸들 1개 + 메모리 매핑 + 엔진 task 1개 + 대기 코루틴 64KB | 360개: fd 360, 코루틴 약 23MB |
| 전송 순간 메모리 | 약 `batch_max_size × 3` (합치기 + JSON 변환 + 압축) | 200M → 최대 약 600MB |
| 요청 본문 | 약 `batch_max_size × 1.5~2` (압축 전) | 200M → 300~400MB |

- **`batch_hold_max_chunks` (기본 512):** 붙잡는 청크가 많아지면 파일 핸들 한도(`ulimit -n`)와 **모든 파이프라인이 공유하는 엔진 task 표**를 채워 다른 output 까지 멈출 수 있습니다. 상한을 넘는 청크는 붙잡지 않고 디스크에 둔 채 엔진이 재시도하며, 이후 주기에 **유실 없이** 전송됩니다. 한 주기 분량이 상한을 넘으면 시작 시 경고가 나옵니다 → `flush` 를 늘리세요.
- **`batch_max_size` 를 꼭 설정하세요:** 설정하지 않으면 한 시간 분량이 한 요청이 됩니다 (600초 이상 주기에서 미설정 시 시작 경고). 넘치는 청크는 디스크에서 다음 주기를 기다립니다.
- **파일 핸들:** 서비스의 `LimitNOFILE` / `ulimit -n` 이 `batch_hold_max_chunks` + 여유분보다 커야 합니다.
- **이벤트 순서:** 한 배치 안에서는 들어온 순서를 지킵니다. 다만 배치가 가득 차 **엔진 재시도로 넘어간 청크는 재시도 순서대로** 나중 배치에 들어가므로, 배치 사이의 순서가 섞일 수 있습니다. 순서가 중요하면 수신 측에서 이벤트 시각(`json_date_key`)으로 정렬하세요.
- **재시도 간격:** 가득 차서 넘어간 청크의 재시도 간격은 `scheduler.cap`(기본 2000초 ≈ 33분)까지 늘어납니다. 장애 복구 뒤 빨리 비우려면 `scheduler.cap` 을 주기보다 작게 (예: 300) 두세요.

#### 장애 유형별 동작 (60분 기준)

| 상황 | 동작 | 데이터 |
|------|------|--------|
| 수신 서버 장애가 여러 시간 지속 | 주기마다 1회(+즉시 재시도) 시도, 실패하면 보관. 배치가 `batch_max_size`/`batch_hold_max_chunks` 에 차면 나머지는 디스크에서 대기 | 유실 없음. 복구 후 첫 주기에 한도만큼, 나머지는 이후 주기와 엔진 재시도로 전송 |
| 413 Payload Too Large | 보관, 매 주기 에러 로그 (`set 'batch_max_size' below the receiver limit`) | 유실 없음. `batch_max_size` 를 줄이고 재시작하면 나눠서 전송 |
| 401/403 등 4xx | 기본: **그 배치 폐기** (에러 로그 `... are lost`) | `batch_retry_4xx: on` 이면 보관 후 복구되면 전송 |
| kill -9 / crash | 디스크의 청크를 재시작 후 다시 보냄 | 유실 없음 (at-least-once) |
| 재시작 후 backlog | 디스크의 청크를 몇 초 안에 다시 읽어 붙잡음 (`storage.backlog.mem_limit` 은 1회 로딩량만 제한, 16K 로 줄여도 첫 주기에 전량 전송됨을 확인). **첫 전송은 재시작 후 한 주기 뒤** | 유실 없음. 한 번에 보내는 양은 `batch_max_size`/`batch_hold_max_chunks` 까지, 나머지는 이후 주기 |
| 응답하지 않는 수신 서버에서 종료 | 종료 중 1회만 시도, `http.response_timeout` 뒤 포기 | 유실 없음 (재시작 후 전송). 종료 시간 ≈ grace + response_timeout |

#### 모니터링
- 주기마다 info 로그 1줄: `batch <id> sent: N records (M chunks), B bytes, HTTP 200, 1 attempt(s), T ms`
- 실패: warn `batch <id> failed: <원인>; N records (M chunks) kept in storage for the next interval in 3600s`
- 가득 참: warn `batch is full (...)` — 주기당 1번

---

## 7. 다른 output 과 함께 쓸 때

### 7.1 청크는 output 별로 따로 있나요?
**아니요.** 청크는 입력 단위로 1개이고, 같은 태그를 match 하는 output 들이 **공유**합니다.
대신 **output 별 완료 여부를 따로 기록**합니다. 성공한 output 은 자기 표시만 지우고, 모두 끝나야 청크가 삭제됩니다.

```
input 청크 1개 ──┬─▶ batch_out  : 배치 대기 중 (표시 유지)
                ├─▶ other_http : 전송 성공   (표시 해제)
                └─▶ stdout     : 전송 성공   (표시 해제)   → 모두 해제되면 삭제
```

### 7.2 배치가 청크를 붙잡는 동안 다른 output 이 재전송하나요?
**아니요.** 이미 보낸 output 은 다시 보내지 않고, 재시도도 output 별입니다.
다른 output 이 실패해 재시도해도 배치 output 은 주기당 1건만 보냅니다.

### 7.3 유실 방지 모드에서 주의할 경우
| 상황 | 영향 | 대응 |
|------|------|------|
| 청크를 붙잡은 채 크래시 → 재시작 | 재시작 시 남은 청크는 **태그 기준으로 모든 output 에 다시 전송** → 이미 받은 다른 output 도 **중복 수신** | 7.4 격리 구성 |
| `storage.type: memory` + `mem_buf_limit` 입력 | 입력 pause → 그 입력의 **모든 output 이 새 로그를 못 받음** | `filesystem` 사용 |
| `storage.pause_on_chunks_overlimit: on` | 붙잡힌 청크가 `max_chunks_up` 을 채우면 입력 pause | 기본값(off) 유지, flush 늘리기 |
| 배치 output 에 `storage.total_limit_size` | 한 주기 분량이 계속 잡혀 한도 초과 시 오래된 청크 삭제 | 한 주기 분량보다 넉넉히 |
| 디스크 | 청크는 가장 늦게 끝나는 output 기준으로 삭제 | 한 주기 분량의 여유 |

### 7.4 권장: 배치 전용 사본으로 격리 (`rewrite_tag`)
레코드를 배치 전용 태그로 **복사**하면 배치 output 은 자기만 쓰는 청크를 붙잡습니다.
원본 청크는 다른 output 이 보내는 즉시 삭제되고, 크래시 후에도 사본은 배치 output 으로만 다시 갑니다.

```yaml
  filters:
    - name: rewrite_tag
      match: app
      rule: $log .* batch.app true           # 모든 레코드를 batch.app 으로 복사, 원본 유지(true)
      emitter_name: batch_emitter
      emitter_storage.type: filesystem

  outputs:
    - name: http                             # 배치 output: 사본만
      match: batch.app
      batch_interval: 5m
      batch_hold_chunks: on
      retry_limit: no_limits
      workers: 1
      # host, port, format, json_events_key ...

    - name: http                             # 다른 output: 원본만
      match: app
      host: other.example.com
```
`rule` 의 키(`$log`)는 모든 레코드에 **항상 있는 키**여야 합니다.

---

## 8. 운영 가이드

### 8.1 상황별 동작
| 상황 | 기본 배치 | 유실 방지 |
|------|-----------|-----------|
| 주기 도래 | 요청 1건 | 요청 1건 → 성공 시 청크 삭제 |
| 빈 주기 | 전송 안 함 | 전송 안 함 |
| 전송 실패 (네트워크/5xx/408/429) | 즉시 1회 재시도 → 다음 주기에 합쳐 재전송 | 즉시 1회 재시도 → 청크 보관, 다음 주기 재전송 |
| 413 | 보관 후 다음 주기 재전송 | 같음 |
| 그 외 4xx | 폐기 (`batch_retry_4xx: on` 이면 보관) | 청크 폐기 (`batch_retry_4xx: on` 이면 보관) |
| 배치가 `batch_max_size` 초과 | 새 청크는 엔진이 보관 후 재시도 (조기 전송 안 함) | 같음 |
| 붙잡은 청크가 `batch_hold_max_chunks` 도달 | — | 새 청크는 디스크에서 대기, 엔진이 재시도 |
| SIGTERM | 1초 내 즉시 전송 | 1초 내 즉시 전송, 성공 시 청크 삭제 |
| kill -9 / crash | 최대 한 주기 유실 | 재시작 후 재전송 |

### 8.2 로그로 확인하기
| 로그 | 의미 / 조치 |
|------|-------------|
| `batch mode enabled, sending every 3600s, chunks held until delivered (batch_max_size .., batch_hold_max_chunks ..)` | 배치 모드와 주요 한도 (info) |
| `batch <id> sent: N records (M chunks), B bytes, HTTP 200, 1 attempt(s), T ms` | 주기마다 성공 1줄 (info) |
| `batch <id>: <원인>, retrying now` | 첫 시도 실패 → 새 연결로 즉시 재시도 (info) |
| `batch <id> failed: <원인>; N records ... kept ... for the next interval in 3600s` | 실패 → 보관, 다음 주기 재전송 (warn) |
| `batch <id>: request of B bytes rejected with HTTP 413, set 'batch_max_size' below the receiver limit` | 요청이 수신 서버 한도 초과 → `batch_max_size` 줄이기 (error, 배치는 보관) |
| `batch <id> dropped: <원인>; N records are lost` | 4xx 로 폐기됨 (error). 막으려면 `batch_retry_4xx: on` |
| `batch <id> failed on shutdown: ...` | 종료 중 전송 실패 → 유실 방지 모드는 디스크에 남아 재시작 후 전송 |
| `batch is full (...): further chunks ...` | `batch_max_size`/`batch_hold_max_chunks` 도달, 주기당 1번 (warn) |
| `about N chunks per tag are flushed in one batch_interval ... consider a larger 'flush'` | 시작 시 점검: 한 주기 청크 수가 상한 초과 |
| `'batch_max_size' is not set: a whole Ns interval goes in a single request` | 시작 시 점검: 긴 주기인데 크기 한도 없음 |
| `'batch_interval' requires a single worker, setting 'workers' to 1` | `workers: 1` 명시하면 사라짐 |
| `chunks handed back for retry ... consider 'retry_limit no_limits'` | 8.3 참고 |
| `'batch_interval' is not supported with 'body_key'` | `body_key` 와 함께 사용 불가 |

### 8.3 `retry_limit` 을 `no_limits` 로 권장하는 이유
배치가 가득 찼을 때(`batch_max_size`)와 유실 방지 모드에서 종료 중 전송이 실패했을 때는 청크를 엔진에 돌려보내 재시도하게 합니다.
기본 `retry_limit`(1)이면 두 번째 실패에서 엔진이 그 청크를 **삭제**하므로, 해당 옵션을 쓸 때는 `no_limits` 로 두세요.

### 8.4 엔진 메트릭 주의
배치 기본 모드는 버퍼에 넣는 순간 엔진에 "성공"을 알리므로, 엔진 메트릭(`fluentbit_output_proc_records_total`)은 **실제 전송 전에** 성공으로 집계됩니다.
실제 전송 결과는 주기마다 남는 로그(`batch <id> sent: ...`)로 확인하세요.

---

## 9. 자주 묻는 질문

**Q. `flush` 를 `batch_interval` 과 같게 맞춰야 하나요?**
아니요. 기본 모드는 설정하지 않는 것이 가장 좋고, 유실 방지 모드는 5~10초면 됩니다 (3장, 6.3).

**Q. 한 주기에 정말 1건만 나가나요?**
네, 로그 양·Tag·청크 수와 관계없이 1건입니다. 예외는 실패 시 즉시 재시도 1회와 종료 시 전송뿐입니다.

**Q. `count` 는 무엇을 세나요?**
그 요청의 배열에 들어간 레코드 수입니다 (바이트 아님). 이월분이 합쳐졌으면 합친 전체 수입니다.

**Q. `workers` 를 늘릴 수 있나요?**
배치 모드에서는 1로 고정됩니다. 워커마다 따로 보내면 주기당 여러 건이 되기 때문입니다.

**Q. 한 요청이 너무 커요.**
`compress: gzip` 을 켜고 수신 서버의 본문 크기 제한을 확인하세요. 필요하면 `batch_interval` 을 줄이세요.

**Q. 서버 시계가 바뀌면 전송 주기가 흔들리나요?**
아니요. 전송 시점은 시스템 시계 변경에 영향받지 않는 단조 시계로 계산합니다.

---

## 10. 제약

- `workers` 는 1 고정, `body_key` 모드와 함께 쓸 수 없음
- 주기는 시작 시점 기준 (정각 정렬 아님)
- 한 배치에 여러 Tag 가 섞이면 `header_tag` 에는 첫 청크의 Tag 가 들어감
- Windows 는 빌드·실행을 검증하지 못함 (Linux·macOS 에서 검증)

---

## 11. 빌드 메모

```bash
mkdir -p build && cd build
cmake .. -DFLB_CONFIG_YAML=On -DFLB_TESTS_RUNTIME=On
make -j8 fluent-bit-bin flb-rt-out_http
./bin/flb-rt-out_http          # 런타임 테스트
```

| 상황 | 옵션 |
|------|------|
| macOS 최신 SDK 에서 c-ares `pipe2` 오류 | `-DHAVE_PIPE2=0` |
| `rdkafka.h` 없음 | `-DFLB_KAFKA=Off` |
| macOS 에 pkg-config 가 없어 libyaml 미검출 | `-DFLB_LIBYAML_DIR=/opt/homebrew` |

---

## 12. 검증 요약

### 런타임 테스트 (`tests/runtime/out_http.c`, 22개 통과)
| 테스트 | 내용 |
|--------|------|
| `json_events_key`, `_count_key`, `_disable_count` | 봉투 형식, 개수 키 변경/생략 |
| `batch_interval`, `batch_hold_chunks` | 여러 flush 에 걸친 3건 → 수신측 요청 **정확히 1건**, `"count":3` (기본 / 유실 방지) |
| `..._no_workers` | 위 두 테스트를 `workers: 0` 으로 |

배치 기능을 끄면 요청 3건으로 테스트가 실패하는 것도 확인했습니다 (테스트가 실제 동작을 검증함).

### 실제 바이너리 E2E
| 시나리오 | 환경 | 결과 |
|----------|------|------|
| 5건 → 빈 주기 → 2건 (기본/유실 방지) | macOS | 주기당 1건, `count == 배열 길이`, 순서 보존, 빈 주기 전송 없음 |
| 서버가 매 응답 후 연결을 끊음 | macOS | 즉시 재시도가 새 연결로 성공, 주기 지연 없음 |
| 유실 방지: kill -9 → 재시작 | Linux | 크래시 동안 0건 → 재시작 후 1건, 청크 파일 삭제 |
| 유실 방지: 대기 중 SIGTERM | Linux | 즉시 전송, 종료 후 청크 파일 0개 |
| output 3개 (배치 + http + stdout) | Linux | 다른 output 즉시 수신·재전송 없음, 배치 1건 |
| 크래시 후 재시작 (격리 없음 / `rewrite_tag` 격리) | Linux | 다른 output 중복 수신 / **중복 0건** |
| memory 스토리지 + `mem_buf_limit` + 유실 방지 | Linux | 입력 pause, 다른 output 도 수신 누락 (7.3 근거) |
| 수신 서버 장애 3주기 지속, 로그 계속 유입 (`batch_max_size` 4K) | Linux | 매 주기 보관, 가득 차면 나머지는 디스크 대기 → 복구 후 **유실 0 · 중복 0** (149건) |
| 수신 서버가 413 반환 | Linux | 폐기 없이 보관, 정상화 후 전송 (유실 0 · 중복 0) |
| 수신 서버가 401 반환 (기본 / `batch_retry_4xx: on`) | Linux | 폐기 + 에러 로그 `... are lost` / 보관 후 인증 복구 뒤 전송 |
| 크래시 후 재시작 backlog (`storage.backlog.mem_limit` 16K / 100M) | Linux | 둘 다 재시작 후 첫 주기에 전량 전송, 유실 0 |
| `batch_hold_max_chunks` 5 초과 (주기당 청크 약 20개) | Linux | 배치당 5청크 이하, 나머지는 이후 주기에 유실 0 |
| 연결만 받고 응답하지 않는 서버에서 SIGTERM | Linux | 13초에 종료 (grace 10 + response_timeout 5 이내), 재시작 후 전송 |

