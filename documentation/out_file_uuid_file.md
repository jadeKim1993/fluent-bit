# out_file `uuid_file` 설정 가이드

`out_file`의 `uuid_file` 옵션을 켜면, Fluent Bit이 flush하는 청크 하나가
**UUIDv7 이름의 완성된 파일 하나**가 됩니다. 다른 프로세스가 디렉토리를 보고
완성된 파일만 가져가 처리하는 중계 구조(A → 파일 → B → 목적지)에 맞춘 기능입니다.

```
/var/spool/relay/
├── relay-01a11629-f424-722e-b612-ab3823d46919.log        ← 완성 파일 (가져갈 대상)
└── relay-01a11629-f6a5-7854-b6d5-fd41442c06da.log.tmp    ← 작성 중 (무시)
```

## 1. 빠른 시작

### YAML

```yaml
service:
  flush: 1                        # 파일이 생기기까지 최대 지연(초)
  storage.path: /var/lib/fluent-bit/storage
  json.escape_unicode: off        # 한글을 \uXXXX가 아닌 원문 그대로 기록 (선택)

pipeline:
  inputs:
    - name: tail
      path: /var/log/myapp/*.log
      tag: app.log
      storage.type: filesystem    # 장애·종료 시 미전송분을 디스크에 보관

  outputs:
    - name: file
      match: 'app.*'
      path: /var/spool/relay
      mkdir: on
      format: plain               # JSON Lines (레코드당 한 줄)
      uuid_file: on
      uuid_file_prefix: relay-
      uuid_file_extension: .log
      uuid_file_fsync: on
      uuid_file_tmp_max_age: 10m
      retry_limit: no_limits      # 반드시 설정 (5절 참고)
```

`tail`에 파서를 지정하지 않으면 각 줄이 `{"log": "<원문>"}`으로 기록됩니다.
로그 한 줄이 JSON이라면 입력에 `parser: json`을 주면 필드가 그대로 풀려서 기록됩니다.

### 클래식 설정 (`.conf`)

```ini
[SERVICE]
    flush                1
    storage.path         /var/lib/fluent-bit/storage
    json.escape_unicode  off

[INPUT]
    name          tail
    path          /var/log/myapp/*.log
    tag           app.log
    storage.type  filesystem

[OUTPUT]
    name                   file
    match                  app.*
    path                   /var/spool/relay
    mkdir                  on
    format                 plain
    uuid_file              on
    uuid_file_prefix       relay-
    uuid_file_extension    .log
    uuid_file_fsync        on
    uuid_file_tmp_max_age  10m
    retry_limit            no_limits
```

### 커맨드라인으로 확인

```bash
fluent-bit -i dummy -t app.log \
  -o file -m '*' -p path=/tmp/relay -p mkdir=on -p format=plain -p uuid_file=on
```

## 2. 옵션

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `uuid_file` | `off` | 켜면 청크마다 `<path>/<prefix><uuidv7><extension>` 파일 1개를 만듭니다. |
| `uuid_file_prefix` | 빈 값 | 파일명 앞에 붙는 문자열. `/`, `\`는 쓸 수 없습니다. |
| `uuid_file_extension` | `.log` | 완성 파일의 확장자. 점이 없으면 붙여 줍니다(`jsonl` → `.jsonl`). 빈 값이면 확장자 없이 만듭니다. `.tmp`로 끝나거나 `/`, `\`가 들어가면 안 됩니다. |
| `uuid_file_fsync` | `on` | 공개 전에 파일을 디스크에 동기화하고, 공개 후 디렉토리도 동기화합니다. 끄면 빠르지만 정전 시 빈 파일이나 깨진 파일이 남을 수 있습니다. |
| `uuid_file_tmp_max_age` | `10m` | 시작할 때 이 시간보다 오래된 `.tmp` 파일을 지웁니다. 숫자로 시작해야 합니다(`600`, `30s`, `10m`, `1h`, `1d`). |

함께 쓰는 기존 `out_file` 옵션:

| 옵션 | 설명 |
|---|---|
| `path` | 출력 디렉토리. **필수**입니다. |
| `mkdir` | `on`이면 디렉토리가 없을 때 만듭니다(권한 0755). 여러 프로세스가 동시에 만들어도 됩니다. |
| `format` | 모든 포맷을 쓸 수 있습니다. JSON Lines가 필요하면 `plain`을 씁니다. 기본값(`json`)은 `태그: [시각, {레코드}]` 형식입니다. |
| `csv_column_names` | `format csv`와 함께 쓰면 **모든 파일**의 첫 줄에 헤더가 들어갑니다. |
| `workers` | 늘려도 됩니다. 파일 이름은 겹치지 않습니다. |

### 함께 쓸 수 없는 설정

다음 조합은 기동할 때 오류로 거부합니다.

- `file` 옵션
- `rotate: on`
- `path`에 레코드 접근자(`$field`) 사용
- Windows (`link()`를 쓰지 못해 지원하지 않습니다)

## 3. 동작 방식

1. 청크를 `<prefix><uuidv7><extension>.tmp`에 기록합니다. 이 파일은 `O_EXCL`로 만들어 다른 프로세스와 겹치지 않습니다.
2. `uuid_file_fsync`가 켜져 있으면 디스크에 동기화한 뒤 닫습니다.
3. `link()`로 `.tmp`를 제외한 최종 이름을 만들고 `.tmp` 이름을 지웁니다. 이 단계는 원자적이며, 같은 이름의 파일이 이미 있으면 **절대 덮어쓰지 않고** 새 이름으로 다시 시도합니다.
4. 디렉토리를 동기화합니다.

그 밖의 규칙:

- **로그가 들어오지 않으면 파일을 만들지 않습니다.** 필터가 레코드를 모두 걸러내거나 출력할 내용이 없어도 파일을 만들지 않습니다.
- **파일 하나에는 청크 하나만 들어갑니다.** 입력이나 태그가 여러 개면 한 번의 flush에 파일이 여러 개 생길 수 있습니다.
- **공개 전에 실패하면 재시도합니다**(`FLB_RETRY`). 공개 후의 실패(디렉토리 동기화)는 경고만 남깁니다. 다시 쓰면 같은 데이터가 두 파일에 들어가기 때문입니다.
- 파일 권한은 `0640`(umask 적용)입니다.
- UUIDv7의 앞부분은 생성 시각(밀리초)이라서 **이름을 정렬하면 생성 순서**가 됩니다.

## 4. 여러 프로세스가 같은 디렉토리에 쓸 때

설정을 따로 맞출 필요 없이 그대로 쓰면 됩니다. 이름의 랜덤 부분, `O_EXCL`, `link()`가
함께 충돌을 막습니다. 검증에서 16개 프로세스가 같은 디렉토리에 기록할 때
같은 밀리초에 만들어진 파일 224개를 포함해 이름 충돌이나 레코드 손실·중복이 없었습니다.

- 프로세스마다 `uuid_file_prefix`를 다르게 주면 어느 프로세스가 만든 파일인지 이름으로 구분할 수 있습니다.
- 시작할 때 지우는 tmp는 **자기와 같은 접두사·확장자이면서 `uuid_file_tmp_max_age`보다 오래된 것**뿐입니다. 다른 프로세스가 쓰는 중인 파일은 건드리지 않습니다.
- 여러 프로세스가 동시에 기동하며 `mkdir: on`으로 같은 디렉토리를 만들어도 됩니다.

## 5. 주의할 설정

| 항목 | 이유 | 권장 |
|---|---|---|
| `retry_limit` | 기본값이 `1`이라서 재시도가 한 번 더 실패하면 청크를 버립니다. | `no_limits` |
| `storage.type` | 메모리 버퍼에서는 flush 전에 종료하면 아직 쓰지 않은 데이터가 사라집니다. 기존 out_file과 같은 엔진 동작입니다. | 입력에 `filesystem`, `service`에 `storage.path` |
| `json.escape_unicode` | 기본값(`on`)에서는 한글이 `\uXXXX`로 기록됩니다. JSON으로 읽으면 원문과 같습니다. | 파일 원문에 한글이 필요하면 `off` |
| `uuid_file_tmp_max_age` | `0`이면 기동하는 프로세스가 다른 프로세스가 쓰는 중인 tmp까지 지울 수 있습니다. 데이터를 잃지는 않지만 재시도가 생깁니다. | 기본값 `10m` 유지 |
| `service.flush` | flush 주기마다 파일이 생기므로 트래픽이 많으면 파일 수가 빨리 늘어납니다. | 소비 속도에 맞춰 늘림 |
| `format: plain` | 이벤트 시각과 태그가 들어가지 않습니다. | 시각이 필요하면 파서나 필터로 레코드에 넣음 |

## 6. 파일을 가져가는 쪽(소비자) 규칙

1. **`*<extension>` 파일만 가져가고 `*.tmp`는 무시합니다.** 완성 이름으로 보이는 파일은 항상 내용이 완전합니다.
2. **같은 파일시스템의 작업 디렉토리로 `rename`(mv)한 뒤 처리합니다.** 소비자가 여러 개여도 한 곳만 성공하고, 실패(`ENOENT`)하면 다른 소비자가 가져간 것이니 건너뜁니다. 다른 파일시스템으로 옮기면 원자적이지 않습니다.
3. **스풀 디렉토리 안에서 파일을 고치거나 같은 이름 패턴의 파일을 만들지 않습니다.**
4. **중복에 안전하게 처리합니다.** 파일을 공개한 직후 Fluent Bit이 죽으면, 재시작 후 같은 청크가 다른 파일로 한 번 더 올 수 있습니다.
5. **처리한 파일은 소비자가 지웁니다.** Fluent Bit은 완성 파일을 지우지 않습니다.
6. **시작할 때 작업 디렉토리에 남은 파일부터 처리합니다.** 소비자가 `mv` 직후 죽은 경우입니다.
7. **순서:** 같은 프로세스가 만든 파일은 이름 순서가 생성 순서입니다. 같은 밀리초에 다른 프로세스가 만든 파일끼리는 순서가 보장되지 않으니, 엄밀한 순서가 필요하면 레코드 안의 시각을 씁니다.
8. **권한:** 소비 프로세스는 Fluent Bit과 같은 사용자이거나 같은 그룹이어야 합니다. Linux에서는 디렉토리에 setgid(`chmod g+s`)를 걸면 새 파일이 디렉토리 그룹을 물려받습니다.
9. **감지:** inotify를 쓰면 `IN_CREATE`(link)와 `IN_MOVED_TO`(rename 대체 경로)를 모두 보고, 이벤트 유실에 대비해 주기적으로 전체 스캔도 합니다.
10. NFS 같은 네트워크 파일시스템보다 **로컬 파일시스템**을 권장합니다.

소비자 예시 (Python):

```python
import os

SPOOL = "/var/spool/relay"
WORK = "/var/spool/relay-work"          # SPOOL과 같은 파일시스템

def claim_files():
    for name in sorted(os.listdir(WORK)):          # 이전에 가져와 놓고 못 끝낸 파일
        yield os.path.join(WORK, name)
    for name in sorted(os.listdir(SPOOL)):         # 이름 정렬 = 생성 순서
        if not name.startswith("relay-") or not name.endswith(".log"):
            continue                               # *.tmp, 무관한 파일 제외
        dst = os.path.join(WORK, name)
        try:
            os.rename(os.path.join(SPOOL, name), dst)
        except FileNotFoundError:
            continue                               # 다른 소비자가 먼저 가져감
        yield dst

for path in claim_files():
    with open(path, encoding="utf-8") as f:
        for line in f:                             # 레코드당 한 줄 (format plain)
            ...                                    # 목적지로 전송, 중복에 안전하게
    os.remove(path)
```

## 7. 기동 오류 메시지

| 메시지 | 원인 |
|---|---|
| `'path' is required when uuid_file is enabled` | `path`가 없음 |
| `path ... is not a directory` | 디렉토리가 없거나 일반 파일임. `mkdir: on`을 쓰거나 디렉토리를 만듦 |
| `'file' cannot be used with uuid_file` | `file` 옵션과 함께 사용 |
| `'rotate' cannot be used with uuid_file` | `rotate: on`과 함께 사용 |
| `record accessors in 'path' cannot be used with uuid_file` | `path`에 `$field` 사용 |
| `'uuid_file_prefix' must not contain path separators` | 접두사에 `/` 또는 `\` |
| `'uuid_file_extension' must not end with '.tmp'` | 확장자가 `.tmp`로 끝남 |
| `invalid uuid_file_tmp_max_age '...'` | 숫자로 시작하지 않는 값(`off` 등) |
| `path, uuid_file_prefix and uuid_file_extension are too long` | 전체 경로가 시스템 최대 길이를 넘음 |
| `uuid_file is not supported on Windows` | Windows에서 사용 |

## 8. 검증

- 런타임 테스트: `tests/runtime/out_file_uuid.c`
  ```bash
  ctest --test-dir build -R flb-rt-out_file_uuid --output-on-failure
  ```
- 통합 테스트: `tests/integration/scenarios/out_file` (여러 프로세스, 크래시 후 재시작 포함)
  ```bash
  tests/integration/.venv/bin/python -m pytest tests/integration/scenarios/out_file -q
  ```
