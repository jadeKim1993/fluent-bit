# out_file uuid_file 검증 매트릭스

`out_file`의 `uuid_file` 옵션(flush된 청크 하나 = UUIDv7 이름의 완성 파일 하나)을
실제 `fluent-bit` 바이너리로 검증하는 하네스와 그 결과 리포트입니다.

- `harness.py`: 케이스마다 설정 파일을 만들어 fluent-bit 프로세스를 띄우고,
  출력 디렉토리의 파일 이름·내용·권한을 검사합니다. 61개 케이스, 약 2분 30초.
- `collect_suites.sh`: 저장소의 관련 런타임·통합 테스트와 메모리 검사를 실행해
  요약을 `report/suites.txt`에 남깁니다.
- `build_report.py`: `report/` 의 결과로 `report/uuid_file_report.html`을 만듭니다.
- `report/`: 마지막 실행 결과와 HTML 리포트.

pytest가 수집하지 않는 독립 스크립트이며, 표준 라이브러리만 사용합니다.

## 실행

```bash
cmake -S . -B build -DFLB_TESTS_RUNTIME=On -DFLB_TESTS_INTERNAL=On
cmake --build build -j8
cd tests/integration/scenarios/out_file/matrix
python3 harness.py               # 전체 케이스
python3 harness.py A01 E03       # 일부 케이스만
./collect_suites.sh              # 저장소 테스트 스위트 (tests/integration/.venv 필요)
python3 build_report.py          # report/uuid_file_report.html 생성
```

환경 변수:

| 변수 | 기본값 | 설명 |
|---|---|---|
| `FLUENT_BIT_BINARY` | `<repo>/build/bin/fluent-bit` | 검사할 바이너리 |
| `MATRIX_ROOT` | 시스템 임시 디렉토리의 `flb_uuid_file_matrix` | 케이스 작업 디렉토리 (실행마다 지움) |
| `BUILD_DIR` | `build` | `collect_suites.sh`가 사용할 빌드 디렉토리 |

## 케이스 구성

| 분야 | ID | 내용 |
|---|---|---|
| 설정 검증 | A01–A22 | 잘못된 설정의 기동 거부, 허용 값, 경로 정규화, uuid_file off 호환 |
| 파일 생성·이름 | B01–B07 | UUIDv7 형식, 권한 0640, 정렬 = 생성 순서, 접두사·확장자 |
| 출력 포맷 | C01–C11 | 모든 format, 메트릭, 한글·이모지, 1MB 레코드 |
| 동작 | D01–D14 | 빈 입력, 필터 제거, 재시도, 종료·재시작, tmp 정리, 기존 파일 보존 |
| 다중 프로세스 | E01–E07 | 16개 프로세스 동시 기록·기동, kill -9 반복, 소비자 동시 실행 |

## 마지막 결과

`report/uuid_file_report.html`을 브라우저로 열어 보세요. 커밋 `baeb4ede7`
(macOS arm64) 기준으로 61/61 통과, 런타임·통합 테스트와 macOS Leaks 검사 통과입니다.

Linux에서 실행하면 `B02`(권한)는 umask 022 기준이고, `collect_suites.sh`는
Leaks 대신 Valgrind로 메모리 검사를 합니다.
