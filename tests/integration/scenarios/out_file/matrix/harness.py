#!/usr/bin/env python3
"""
out_file uuid_file 기능 검증 매트릭스.

케이스마다 설정 파일을 만들어 실제 fluent-bit 프로세스를 띄우고, 출력
디렉토리의 파일 이름과 내용을 검사한다. 결과는 report/results.json과
report/meta.json에 저장되고 build_report.py가 HTML 리포트로 만든다.

    python3 harness.py              # 전체 실행
    python3 harness.py A01 E03      # 일부 케이스만

환경 변수:
    FLUENT_BIT_BINARY  검사할 바이너리 (기본: <repo>/build/bin/fluent-bit)
    MATRIX_ROOT        케이스 작업 디렉토리 (기본: 시스템 임시 디렉토리)
"""

import json
import os
import random
import shutil
import signal
import stat
import subprocess
import platform
import sys
import tempfile
import threading
import time
import traceback
import uuid as uuidlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "../../../../.."))
BIN = os.environ.get("FLUENT_BIT_BINARY", os.path.join(REPO, "build", "bin", "fluent-bit"))
ROOT = os.environ.get("MATRIX_ROOT", os.path.join(tempfile.gettempdir(), "flb_uuid_file_matrix"))
REPORT_DIR = os.path.join(HERE, "report")
RESULTS = os.path.join(REPORT_DIR, "results.json")
META = os.path.join(REPORT_DIR, "meta.json")

NAME_RE = None
CASES = []


def case(cid, category, title, expected):
    def deco(fn):
        CASES.append({"id": cid, "category": category, "title": title,
                      "expected": expected, "fn": fn})
        return fn
    return deco


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


# ---------------------------------------------------------------- config

def q(value):
    return "'" + str(value).replace("'", "''") + "'"


def make_yaml(inputs, outputs, service=None, filters=None):
    svc = {"flush": "0.2", "grace": "2", "log_level": "info"}
    svc.update(service or {})
    lines = ["service:"]
    for k, v in svc.items():
        lines.append(f"  {k}: {q(v)}")
    lines.append("pipeline:")
    for section, items in (("inputs", inputs), ("filters", filters or []), ("outputs", outputs)):
        if not items:
            continue
        lines.append(f"  {section}:")
        for item in items:
            first = True
            for k, v in item.items():
                prefix = "    - " if first else "      "
                lines.append(f"{prefix}{k}: {q(v)}")
                first = False
    return "\n".join(lines) + "\n"


def dummy(tag="app.log", record=None, samples=1, rate=100, **extra):
    d = {"name": "dummy", "tag": tag,
         "dummy": json.dumps({"message": "hello"} if record is None else record, ensure_ascii=False),
         "samples": samples, "rate": rate}
    d.update(extra)
    return d


def uuid_out(path, **extra):
    o = {"name": "file", "match": "*", "path": path, "uuid_file": "on", "format": "plain"}
    o.update(extra)
    return o


# ---------------------------------------------------------------- process

class Flb:
    def __init__(self, workdir, yaml_text, name="flb"):
        os.makedirs(workdir, exist_ok=True)
        self.cfg = os.path.join(workdir, f"{name}.yaml")
        self.log = os.path.join(workdir, f"{name}.log")
        with open(self.cfg, "w") as f:
            f.write(yaml_text)
        self.proc = None

    def start(self):
        self.out = open(self.log, "a")
        self.proc = subprocess.Popen([BIN, "-c", self.cfg], stdout=self.out,
                                     stderr=subprocess.STDOUT)
        return self

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self, timeout=30):
        if self.proc is None:
            return None
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.out.close()
        return self.proc.returncode

    def kill9(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()
        self.out.close()

    def text(self):
        with open(self.log, errors="replace") as f:
            return f.read()

    def errors(self):
        return [l for l in self.text().splitlines() if "[error]" in l]


def wait_for(pred, timeout=20, interval=0.1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return pred()


def expect_start_fail(wd, yaml_text, needle):
    flb = Flb(wd, yaml_text).start()
    exited = wait_for(lambda: not flb.alive(), timeout=8)
    rc = flb.stop()
    check(exited, "기동이 거부되지 않음 (프로세스가 계속 실행됨)")
    check(rc != 0, f"종료 코드가 0 (rc={rc})")
    log = flb.text()
    check(needle in log, f"예상 오류 메시지 없음: {needle!r}")
    msg = next((l.split("] ", 2)[-1] for l in log.splitlines() if needle in l), needle)
    return f"기동 거부 (rc={rc}): {msg.strip()}"


def expect_start_ok(wd, yaml_text, settle=2.5):
    flb = Flb(wd, yaml_text).start()
    time.sleep(settle)
    ok = flb.alive()
    flb.stop()
    check(ok, "기동 실패: " + "; ".join(flb.errors()[:2]))
    return flb


# ---------------------------------------------------------------- inspection

def finals(d, ext=".log"):
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d) if not n.endswith(".tmp") and
                  (ext == "" or n.endswith(ext)) and os.path.isfile(os.path.join(d, n)))


def tmps(d):
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d) if n.endswith(".tmp"))


def is_uuid_name(name, prefix="", ext=".log"):
    if not name.startswith(prefix) or not name.endswith(ext):
        return False
    core = name[len(prefix):len(name) - len(ext)] if ext else name[len(prefix):]
    try:
        u = uuidlib.UUID(core)
    except ValueError:
        return False
    return str(u) == core and u.version == 7 and u.variant == uuidlib.RFC_4122


def uuid_ms(name, prefix="", ext=".log"):
    core = name[len(prefix):len(name) - len(ext)]
    return int(core.replace("-", "")[:12], 16)


def read(d, n, mode="r"):
    with open(os.path.join(d, n), mode, **({} if "b" in mode else {"encoding": "utf-8"})) as f:
        return f.read()


def json_records(d, ext=".log"):
    """모든 완성 파일을 JSON Lines로 파싱. 부분 쓰기/깨진 줄은 예외."""
    files = {}
    for n in finals(d, ext):
        data = read(d, n)
        if not data.endswith("\n"):
            raise Fail(f"{n} 가 줄바꿈으로 끝나지 않음 (불완전)")
        files[n] = [json.loads(l) for l in data.splitlines()]
    return files


def count_json(d, ext=".log"):
    try:
        return sum(len(v) for v in json_records(d, ext).values())
    except (Fail, ValueError):
        return -1


# ================================================================= A. 설정 검증

@case("A01", "설정 검증", "path 미지정", "기동 거부")
def _(wd):
    o = uuid_out(None)
    del o["path"]
    return expect_start_fail(wd, make_yaml([dummy()], [o]), "'path' is required")


@case("A02", "설정 검증", "path가 일반 파일", "기동 거부")
def _(wd):
    os.makedirs(wd, exist_ok=True)
    f = os.path.join(wd, "afile")
    open(f, "w").close()
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(f)]), "is not a directory")


@case("A03", "설정 검증", "없는 디렉토리 + mkdir off", "기동 거부")
def _(wd):
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(os.path.join(wd, "nope"))]),
                             "is not a directory")


@case("A04", "설정 검증", "없는 중첩 디렉토리 + mkdir on", "디렉토리 생성 후 정상 기록")
def _(wd):
    d = os.path.join(wd, "a", "b", "c")
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d, mkdir="on")])).start()
    ok = wait_for(lambda: count_json(d) == 1, 10)
    flb.stop()
    check(ok, "파일이 생성되지 않음")
    return f"{d[len(wd):]} 생성, 파일 {len(finals(d))}개"


@case("A05", "설정 검증", "file 옵션과 함께 사용", "기동 거부")
def _(wd):
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, file="x.log")]),
                             "'file' cannot be used with uuid_file")


@case("A06", "설정 검증", "rotate on과 함께 사용", "기동 거부")
def _(wd):
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, rotate="on")]),
                             "'rotate' cannot be used with uuid_file")


@case("A07", "설정 검증", "path에 레코드 접근자($field)", "기동 거부")
def _(wd):
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd + "/$stream",
                             fallback_file="fb.log")]), "record accessors in 'path'")


for _cid, _val in (("A08", "a/b"), ("A09", "a\\b")):
    @case(_cid, "설정 검증", f"uuid_file_prefix에 경로 구분자 ({_val})", "기동 거부")
    def _(wd, _val=_val):
        return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, uuid_file_prefix=_val)]),
                                 "must not contain path separators")

for _cid, _val in (("A10", ".tmp"), ("A11", "tmp"), ("A12", ".log.tmp")):
    @case(_cid, "설정 검증", f"uuid_file_extension = '{_val}'", "기동 거부 (.tmp로 끝남)")
    def _(wd, _val=_val):
        return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, uuid_file_extension=_val)]),
                                 "must not end with '.tmp'")


@case("A13", "설정 검증", "uuid_file_extension에 '/' 포함", "기동 거부")
def _(wd):
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, uuid_file_extension="x/y")]),
                             "must not contain path separators")


for _cid, _val in (("A14", "off"), ("A15", "abc"), ("A16", "-1")):
    @case(_cid, "설정 검증", f"uuid_file_tmp_max_age = '{_val}'", "기동 거부 (숫자로 시작해야 함)")
    def _(wd, _val=_val):
        return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, uuid_file_tmp_max_age=_val)]),
                                 "invalid uuid_file_tmp_max_age")


@case("A17", "설정 검증", "uuid_file_tmp_max_age = ' 10m' (앞 공백)", "공백 제거 후 10m로 기동")
def _(wd):
    expect_start_ok(wd, make_yaml([dummy(samples=0, rate=1)], [uuid_out(wd, uuid_file_tmp_max_age=" 10m")]))
    return "설정 파서가 앞뒤 공백을 제거해 10m로 처리"


@case("A18", "설정 검증", "uuid_file_tmp_max_age 정상값 0/30/30s/10m/1h/1d", "모두 기동 성공")
def _(wd):
    for v in ("0", "30", "30s", "10m", "1h", "1d"):
        expect_start_ok(os.path.join(wd, v), make_yaml([dummy(samples=0, rate=1)],
                        [uuid_out(os.path.join(wd, v), mkdir="on", uuid_file_tmp_max_age=v)]), 1.5)
    return "6개 값 모두 기동 성공"


@case("A19", "설정 검증", "uuid_file 값이 잘못된 bool ('maybe')", "기동 거부")
def _(wd):
    o = uuid_out(wd)
    o["uuid_file"] = "maybe"
    flb = Flb(wd, make_yaml([dummy()], [o])).start()
    exited = wait_for(lambda: not flb.alive(), 8)
    rc = flb.stop()
    check(exited and rc != 0, f"잘못된 bool이 허용됨 (rc={rc})")
    return f"기동 거부 (rc={rc})"


@case("A20", "설정 검증", "경로+접두사가 PATH_MAX 초과", "기동 거부")
def _(wd):
    return expect_start_fail(wd, make_yaml([dummy()], [uuid_out(wd, uuid_file_prefix="p" * 1100)]),
                             "too long")


@case("A21", "설정 검증", "path 끝에 '/' 여러 개", "정상 기록, 이름에 '//' 없음")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d + "///")])).start()
    ok = wait_for(lambda: count_json(d) == 1, 10)
    flb.stop()
    check(ok, "파일 미생성")
    check("//" not in flb.text().split("uuid_file: path=")[1].split()[0], "경로 정규화 안 됨")
    return "path=" + flb.text().split("uuid_file: path=")[1].split()[0]


@case("A22", "설정 검증", "uuid_file off (기본값)", "기존 out_file 동작 유지 (태그 이름 파일)")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy(tag="app.log")], [{"name": "file", "match": "*", "path": d,
                                                       "format": "plain"}])).start()
    ok = wait_for(lambda: os.path.exists(os.path.join(d, "app.log")), 10)
    flb.stop()
    check(ok, "app.log 미생성")
    names = os.listdir(d)
    check(names == ["app.log"], f"예상 밖 파일: {names}")
    return "app.log 1개만 생성 (UUID 파일 없음)"


# ================================================================= B. 파일 이름/생성

@case("B01", "파일 생성·이름", "레코드 1건", "파일 1개, UUIDv7 이름, tmp 없음")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d)])).start()
    ok = wait_for(lambda: count_json(d) == 1, 10)
    flb.stop()
    check(ok, "파일 미생성")
    f = finals(d)
    check(len(f) == 1 and is_uuid_name(f[0]), f"이름 이상: {f}")
    check(tmps(d) == [], "tmp 잔존")
    return f[0]


@case("B02", "파일 생성·이름", "파일 권한", "0640 (umask 022 기준), 기타 사용자 접근 불가")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d)])).start()
    wait_for(lambda: count_json(d) == 1, 10)
    flb.stop()
    mode = stat.S_IMODE(os.stat(os.path.join(d, finals(d)[0])).st_mode)
    check(mode == 0o640, f"권한 {oct(mode)}")
    return f"권한 {oct(mode)}"


@case("B03", "파일 생성·이름", "접두사 + 점 없는 확장자 (relay-, jsonl)", "relay-<uuid>.jsonl")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d, uuid_file_prefix="relay-",
                                                 uuid_file_extension="jsonl")])).start()
    ok = wait_for(lambda: len(finals(d, ".jsonl")) == 1, 10)
    flb.stop()
    check(ok, "파일 미생성")
    n = finals(d, ".jsonl")[0]
    check(is_uuid_name(n, "relay-", ".jsonl"), n)
    return n


@case("B04", "파일 생성·이름", "빈 확장자", "<uuid> (확장자 없음)")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d, uuid_file_extension="")])).start()
    ok = wait_for(lambda: len(finals(d, "")) == 1, 10)
    flb.stop()
    check(ok, "파일 미생성")
    n = finals(d, "")[0]
    check(is_uuid_name(n, "", ""), n)
    return n


@case("B05", "파일 생성·이름", "이름 정렬 = 생성 순서 (20개 파일)", "정렬 순서와 기록 순서 일치")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "src.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log",
                              "refresh_interval": "1"}],
                            [uuid_out(d)], service={"flush": "0.1"})).start()
    time.sleep(1.5)
    for i in range(20):
        with open(src, "a") as f:
            f.write(f"seq-{i:03d}\n")
        wait_for(lambda: count_json(d) == i + 1, 5, 0.05)
    flb.stop()
    files = json_records(d)
    seqs = [r["log"] for n in sorted(files) for r in files[n]]
    check(len(seqs) == 20, f"레코드 {len(seqs)}개")
    check(seqs == [f"seq-{i:03d}" for i in range(20)], f"순서 불일치: {seqs[:5]}...")
    return f"파일 {len(files)}개, 정렬 순서 = seq-000..seq-019"


@case("B06", "파일 생성·이름", "UUID 안의 시각", "파일 생성 시각과 ±2초 이내")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy()], [uuid_out(d)])).start()
    wait_for(lambda: count_json(d) == 1, 10)
    flb.stop()
    n = finals(d)[0]
    ms = uuid_ms(n)
    mtime = os.stat(os.path.join(d, n)).st_mtime * 1000
    diff = abs(ms - mtime)
    check(diff < 2000, f"차이 {diff:.0f}ms")
    return f"UUID 시각과 mtime 차이 {diff:.0f}ms"


@case("B07", "파일 생성·이름", "uuid_file_fsync off", "정상 기록")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy(samples=50)], [uuid_out(d, uuid_file_fsync="off")])).start()
    ok = wait_for(lambda: count_json(d) == 50, 10)
    flb.stop()
    check(ok, f"레코드 {count_json(d)}개")
    return f"레코드 50개, 파일 {len(finals(d))}개"


# ================================================================= C. 포맷

def run_format(wd, fmt_opts, record=None, samples=2, ext=".log", wait_files=1, inputs=None,
               service=None):
    d = os.path.join(wd, "out")
    os.makedirs(d, exist_ok=True)
    o = uuid_out(d, **fmt_opts)
    if "format" in fmt_opts and fmt_opts["format"] is None:
        del o["format"]
    flb = Flb(wd, make_yaml(inputs or [dummy(record=record, samples=samples, rate=samples)],
                            [o], service=dict({"flush": "1"}, **(service or {})))).start()
    ok = wait_for(lambda: len(finals(d, ext)) >= wait_files, 10)
    flb.stop()
    check(ok, "파일 미생성")
    check(tmps(d) == [], "tmp 잔존")
    return d


@case("C01", "출력 포맷", "기본 포맷 (format 미지정)", "'태그: [시각, {레코드}]' 형식")
def _(wd):
    d = run_format(wd, {"format": None}, {"message": "hi"}, samples=1)
    line = read(d, finals(d)[0]).splitlines()[0]
    check(line.startswith("app.log: [") and line.endswith('{"message":"hi"}]'), line)
    return line


@case("C02", "출력 포맷", "plain", "JSON Lines")
def _(wd):
    d = run_format(wd, {"format": "plain"}, {"message": "hi", "n": 1, "nested": {"k": [1, 2]}})
    recs = [r for v in json_records(d).values() for r in v]
    check(recs and all(r == {"message": "hi", "n": 1, "nested": {"k": [1, 2]}} for r in recs),
          str(recs[:1]))
    return json.dumps(recs[0], ensure_ascii=False)


@case("C03", "출력 포맷", "csv", "시각,값,값")
def _(wd):
    d = run_format(wd, {"format": "csv"}, {"a": 1, "b": "x"}, samples=1)
    line = read(d, finals(d)[0]).splitlines()[0]
    check(line.endswith(',1,"x"'), line)
    return line


@case("C04", "출력 포맷", "csv + csv_column_names (파일 2개)", "모든 파일 첫 줄이 헤더")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "src.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log", "refresh_interval": "1"}],
                            [uuid_out(d, format="csv", csv_column_names="on")])).start()
    time.sleep(1.5)
    for i in range(2):
        with open(src, "a") as f:
            f.write(f"line{i}\n")
        wait_for(lambda: len(finals(d)) == i + 1, 5)
    flb.stop()
    fs = finals(d)
    check(len(fs) == 2, f"파일 {len(fs)}개")
    heads = [read(d, n).splitlines()[0] for n in fs]
    check(all(h == 'timestamp,"log"' for h in heads), str(heads))
    return f"파일 2개 모두 헤더: {heads[0]}"


@case("C05", "출력 포맷", "ltsv", '"time":... 탭 구분 레이블')
def _(wd):
    d = run_format(wd, {"format": "ltsv"}, {"a": 1}, samples=1)
    line = read(d, finals(d)[0]).splitlines()[0]
    check(line.startswith('"time":') and '"a":1' in line, line)
    return line.replace("\t", "\\t")


@case("C06", "출력 포맷", "template ({message} by {user})", "템플릿 치환")
def _(wd):
    d = run_format(wd, {"format": "template", "template": "{message} by {user}"},
                   {"message": "hi", "user": "kim"}, samples=1)
    line = read(d, finals(d)[0]).splitlines()[0]
    check(line == "hi by kim", line)
    return line


@case("C07", "출력 포맷", "msgpack", "원본 청크 그대로 (배열로 시작)")
def _(wd):
    d = run_format(wd, {"format": "msgpack"}, {"a": 1}, samples=1)
    data = read(d, finals(d)[0], "rb")
    check(len(data) > 0 and data[0] in (0x92, 0xdc, 0xdd), f"첫 바이트 {data[:1].hex()}")
    return f"{len(data)} bytes, 첫 바이트 0x{data[0]:02x}"


@case("C08", "출력 포맷", "메트릭 입력 (fluentbit_metrics)", "메트릭 텍스트 파일 생성")
def _(wd):
    d = run_format(wd, {"format": None},
                   inputs=[{"name": "fluentbit_metrics", "tag": "m", "scrape_interval": "1"}])
    text = read(d, finals(d)[0])
    check("fluentbit_" in text, text[:80])
    return f"메트릭 {len(text.splitlines())}줄"


UTF8_REC = {"msg": "안녕하세요 🚀 \"quoted\" back\\slash\nnew\tline", "키": "값"}


@case("C09", "출력 포맷", "한글·이모지·특수문자 (기본 설정)",
      "유효한 JSON, 파싱 결과 원문과 동일 (한글은 \\uXXXX로 이스케이프)")
def _(wd):
    d = run_format(wd, {"format": "plain"}, UTF8_REC, samples=1)
    recs = [r for v in json_records(d).values() for r in v]
    check(recs == [UTF8_REC], str(recs))
    raw = read(d, finals(d)[0])
    return "파싱 결과 동일, 파일 원문: " + raw.strip()[:46] + "..."


@case("C10", "출력 포맷", "한글·이모지·특수문자 (json.escape_unicode: off)",
      "UTF-8 원문 그대로 기록, 파싱 결과 동일")
def _(wd):
    d = run_format(wd, {"format": "plain"}, UTF8_REC, samples=1,
                   service={"json.escape_unicode": "off"})
    recs = [r for v in json_records(d).values() for r in v]
    check(recs == [UTF8_REC], str(recs))
    raw = read(d, finals(d)[0])
    check("안녕하세요 🚀" in raw, "이스케이프됨")
    return "파일 원문: " + raw.strip()[:40] + "..."


@case("C11", "출력 포맷", "1MB 크기 레코드", "내용 손상 없음")
def _(wd):
    big = "".join(random.choice("abcdefghij") for _ in range(1024 * 1024))
    rec = {"big": big}
    d = run_format(wd, {"format": "plain"}, rec, samples=1)
    recs = [r for v in json_records(d).values() for r in v]
    check(len(recs) == 1 and recs[0]["big"] == big, "내용 불일치")
    return f"{os.path.getsize(os.path.join(d, finals(d)[0])):,} bytes 일치"


# ================================================================= D. 동작

@case("D01", "동작", "로그가 들어오지 않음 (8초 대기)", "파일 0개")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "empty.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log"}],
                            [uuid_out(d)])).start()
    time.sleep(8)
    flb.stop()
    check(os.listdir(d) == [], f"파일 생성됨: {os.listdir(d)}")
    return "8초간 파일 0개, tmp 0개"


@case("D02", "동작", "필터가 모든 레코드 제거 (grep exclude)", "파일 0개, tmp 0개")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy(samples=0, rate=20)], [uuid_out(d)],
                            filters=[{"name": "grep", "match": "*", "exclude": "message .*"}])).start()
    time.sleep(4)
    flb.stop()
    check(os.listdir(d) == [], f"{os.listdir(d)}")
    return "4초간 레코드 ~80건 유입, 파일 0개"


@case("D03", "동작", "빈 출력 청크 (csv에 빈 레코드)", "파일 만들지 않음")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy(record={}, samples=10, rate=10)], [uuid_out(d, format="csv")])).start()
    time.sleep(3)
    flb.stop()
    check(os.listdir(d) == [], f"{os.listdir(d)}")
    return "레코드 10건(내용 없음) → 파일 0개"


@case("D04", "동작", "태그 3개 동시 입력", "태그별 파일, 한 파일에 섞이지 않음")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    ins = [dummy(tag=f"t{i}", record={"t": i}, samples=100, rate=100) for i in range(3)]
    flb = Flb(wd, make_yaml(ins, [uuid_out(d, match="t*")])).start()
    ok = wait_for(lambda: count_json(d) == 300, 15)
    flb.stop()
    check(ok, f"레코드 {count_json(d)}")
    files = json_records(d)
    mixed = [n for n, rs in files.items() if len({r["t"] for r in rs}) != 1]
    check(not mixed, f"섞인 파일 {len(mixed)}개")
    return f"파일 {len(files)}개, 레코드 300건, 섞인 파일 0개"


@case("D05", "동작", "output 2개가 같은 디렉토리", "각자 사본, 덮어쓰기 없음")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy(samples=100, rate=100)], [uuid_out(d), uuid_out(d)])).start()
    ok = wait_for(lambda: count_json(d) == 200, 15)
    flb.stop()
    check(ok, f"레코드 {count_json(d)} (기대 200)")
    return f"파일 {len(finals(d))}개, 레코드 200건 (100 × 2)"


@case("D06", "동작", "workers 8 + 입력 16개", "레코드 정확히 1번씩")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    ins = [dummy(tag=f"t{i}", record={"t": i}, samples=200, rate=200) for i in range(16)]
    flb = Flb(wd, make_yaml(ins, [uuid_out(d, workers="8")], service={"flush": "0.1"})).start()
    ok = wait_for(lambda: count_json(d) == 3200, 30)
    flb.stop()
    check(ok, f"레코드 {count_json(d)} (기대 3200)")
    per = {}
    for rs in json_records(d).values():
        for r in rs:
            per[r["t"]] = per.get(r["t"], 0) + 1
    check(all(per.get(i) == 200 for i in range(16)), str(per))
    return f"파일 {len(finals(d))}개, 입력별 200건 × 16"


@case("D07", "동작", "시작 시 tmp 정리 (7가지 파일)", "자기 패턴의 오래된 tmp만 삭제")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    u = "01890a5d-ac96-774b-bcce-b30209900000"
    old = time.time() - 3600
    files = {
        f"relay-{u}.log.tmp": (True, "삭제"),          # 자기 패턴, 오래됨
        f"relay-{u[:-1]}1.log.tmp": (False, "유지"),   # 자기 패턴, 최근
        f"other-{u}.log.tmp": (True, "유지"),          # 다른 접두사
        f"relay-{u}.json.tmp": (True, "유지"),         # 다른 확장자
        "junk.tmp": (True, "유지"),                    # 무관
        f"relay-{u[:-1]}2.log": (True, "유지"),        # 완성 파일
    }
    for n, (is_old, _) in files.items():
        p = os.path.join(d, n)
        with open(p, "w") as f:
            f.write("x")
        if is_old:
            os.utime(p, (old, old))
    os.mkdir(os.path.join(d, f"relay-{u[:-1]}3.log.tmp"))  # 디렉토리
    os.utime(os.path.join(d, f"relay-{u[:-1]}3.log.tmp"), (old, old))
    expect_start_ok(wd, make_yaml([dummy(samples=0, rate=1)], [uuid_out(d, uuid_file_prefix="relay-")]))
    bad = []
    for n, (_, want) in files.items():
        exists = os.path.exists(os.path.join(d, n))
        if exists != (want == "유지"):
            bad.append(n)
    check(os.path.isdir(os.path.join(d, f"relay-{u[:-1]}3.log.tmp")), "디렉토리 삭제됨")
    check(not bad, f"잘못 처리: {bad}")
    return "삭제 1 (자기 패턴·1시간 전) / 유지 6 (최근·다른 접두사·다른 확장자·무관·완성·디렉토리)"


@case("D08", "동작", "기존 완성 파일 300개가 있는 디렉토리", "기존 파일 내용 변화 없음")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    before = {}
    for i in range(300):
        n = f"{uuidlib.uuid4()}.log"
        with open(os.path.join(d, n), "w") as f:
            f.write(f"original {i}\n")
        before[n] = f"original {i}\n"
    flb = Flb(wd, make_yaml([dummy(samples=200, rate=200)], [uuid_out(d)])).start()
    wait_for(lambda: len(finals(d)) > 300, 10)
    time.sleep(1)
    flb.stop()
    changed = [n for n, c in before.items() if read(d, n) != c]
    check(not changed, f"변경된 파일 {len(changed)}개")
    return f"기존 300개 보존, 새 파일 {len(finals(d)) - 300}개"


@case("D09", "동작", "쓰기 중 디렉토리 삭제 → 재생성 (retry_limit no_limits)", "재시도 후 정확히 1번 기록")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "src.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log", "refresh_interval": "1"}],
                            [uuid_out(d, retry_limit="no_limits")],
                            service={"scheduler.base": "1", "scheduler.cap": "2"})).start()
    time.sleep(1.5)
    os.rmdir(d)
    with open(src, "a") as f:
        f.write("must-arrive\n")
    time.sleep(3)
    os.makedirs(d)
    ok = wait_for(lambda: count_json(d) == 1, 15)
    flb.stop()
    check(ok, f"레코드 {count_json(d)}")
    retries = flb.text().count("could not create")
    return f"실패 {retries}회 후 전달, 레코드 1건, tmp {len(tmps(d))}개"


@case("D10", "동작", "디렉토리 쓰기 권한 제거 → 복구", "재시도 후 정확히 1번 기록")
def _(wd):
    if os.geteuid() == 0:
        return "root라서 생략"
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "src.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log", "refresh_interval": "1"}],
                            [uuid_out(d, retry_limit="no_limits")],
                            service={"scheduler.base": "1", "scheduler.cap": "2"})).start()
    time.sleep(1.5)
    os.chmod(d, 0o555)
    with open(src, "a") as f:
        f.write("must-arrive\n")
    time.sleep(3)
    check(os.listdir(d) == [], "권한 없는데 파일 생성")
    os.chmod(d, 0o755)
    ok = wait_for(lambda: count_json(d) == 1, 15)
    flb.stop()
    check(ok, f"레코드 {count_json(d)}")
    return f"권한 없는 동안 파일 0개 → 복구 후 1건, 실패 로그 {flb.text().count('Permission denied')}회"


@case("D11", "동작", "retry_limit 기본값(1)에서 계속 실패", "청크 폐기 (설정 주의 사항 확인)")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "src.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log", "refresh_interval": "1"}],
                            [uuid_out(d)], service={"scheduler.base": "1", "scheduler.cap": "1"})).start()
    time.sleep(1.5)
    os.rmdir(d)
    with open(src, "a") as f:
        f.write("will-be-dropped\n")
    dropped = wait_for(lambda: "cannot be retried" in flb.text() or "retry_limit" in flb.text()
                       or "chunk cannot be retried" in flb.text(), 15)
    os.makedirs(d)
    time.sleep(3)
    flb.stop()
    check(dropped, "폐기 로그 없음")
    check(count_json(d) == 0, "폐기 후에도 전달됨")
    return "재시도 1회 후 폐기 → 중계 용도는 retry_limit: no_limits 필요"


@case("D12", "동작", "flush 전 종료 → 재시작 (storage.type: filesystem)",
      "종료 때 못 쓴 500건이 재시작 후 모두 기록")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    st = os.path.join(wd, "storage")
    fs = {"storage.type": "filesystem"}
    flb = Flb(wd, make_yaml([dummy(samples=500, rate=500, **fs)], [uuid_out(d)],
                            service={"flush": "5", "grace": "5", "storage.path": st})).start()
    time.sleep(2.5)
    flb.stop()
    at_stop = count_json(d)
    flb2 = Flb(wd, make_yaml([dummy(samples=1, rate=1, record={"message": "after"}, **fs)],
                             [uuid_out(d)], service={"flush": "1", "storage.path": st}),
               name="restart").start()
    wait_for(lambda: count_json(d) >= 501, 15)
    flb2.stop()
    recs = [r["message"] for v in json_records(d).values() for r in v]
    check(recs.count("hello") == 500, f"재시작 후 이전 데이터 {recs.count('hello')}건")
    check(tmps(d) == [], "tmp 잔존")
    return f"종료 시점 기록 {at_stop}건 → 재시작 후 500건 모두 기록 (+ 새 데이터 1건)"


@case("D14", "동작", "flush 전 종료 (메모리 버퍼, 기본값)",
      "미전송분 유실 — Fluent Bit 엔진 기존 동작 (stdout·기존 out_file 동일)")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    flb = Flb(wd, make_yaml([dummy(samples=500, rate=500)], [uuid_out(d)],
                            service={"flush": "5", "grace": "5"})).start()
    time.sleep(2.5)
    flb.stop()
    n = count_json(d)
    check(tmps(d) == [], "tmp 잔존")
    return f"종료 시점 기록 {n}/500건 → 유실 방지에는 storage.type: filesystem 필요"


@case("D13", "동작", "파일 생성 지연 (flush 1초)", "레코드 유입 후 약 1초 이내 파일 공개")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    src = os.path.join(wd, "src.log")
    open(src, "w").close()
    flb = Flb(wd, make_yaml([{"name": "tail", "path": src, "tag": "app.log", "refresh_interval": "1"}],
                            [uuid_out(d)], service={"flush": "1"})).start()
    time.sleep(2)
    delays = []
    for i in range(5):
        t0 = time.time()
        with open(src, "a") as f:
            f.write(f"x{i}\n")
        wait_for(lambda: count_json(d) == i + 1, 5, 0.02)
        delays.append(time.time() - t0)
        time.sleep(0.3)
    flb.stop()
    check(max(delays) < 2.0, f"최대 {max(delays):.2f}s")
    return "지연 " + ", ".join(f"{x:.2f}s" for x in delays)


# ================================================================= E. 다중 프로세스

def multi_writers(wd, d, n, samples, rate=200, extra_out=None, inputs_per=1, flush="0.2",
                  start_together=False):
    procs = []
    for p in range(n):
        ins = [dummy(tag=f"in.{i}", record={"proc": p, "in": i, "message": "m"}, samples=samples,
                     rate=rate) for i in range(inputs_per)]
        o = uuid_out(d, mkdir="on", retry_limit="no_limits", **(extra_out or {}))
        procs.append(Flb(os.path.join(wd, f"p{p}"), make_yaml(ins, [o], service={"flush": flush}),
                         name=f"p{p}"))
    for f in procs:
        f.start()
    return procs


def tally(d, ext=".log"):
    per = {}
    files = json_records(d, ext)
    for n, rs in files.items():
        keys = {(r["proc"], r.get("in", 0)) for r in rs}
        if len(keys) != 1:
            raise Fail(f"{n}: 여러 스트림이 섞임 {keys}")
        for r in rs:
            k = (r["proc"], r.get("in", 0))
            per[k] = per.get(k, 0) + 1
    return files, per


def same_ms(names, prefix="", ext=".log"):
    ms = [uuid_ms(n, prefix, ext) for n in names]
    return len(ms) - len(set(ms))


for _cid, _n, _inputs, _samples in (("E01", 4, 1, 500), ("E02", 8, 2, 400), ("E03", 16, 2, 300)):
    @case(_cid, "다중 프로세스", f"프로세스 {_n}개 × 입력 {_inputs}개가 같은 디렉토리에 기록",
          "레코드 손실·중복·섞임 0, 이름 충돌 0")
    def _(wd, _n=_n, _inputs=_inputs, _samples=_samples):
        d = os.path.join(wd, "out")
        procs = multi_writers(wd, d, _n, _samples, rate=_samples, inputs_per=_inputs,
                              extra_out={"workers": "4"}, flush="0.1")
        total = _n * _inputs * _samples
        ok = wait_for(lambda: count_json(d) == total, 60, 0.3)
        for p in procs:
            p.stop()
        files, per = tally(d)
        check(ok, f"레코드 {sum(per.values())} (기대 {total})")
        bad = {k: v for k, v in per.items() if v != _samples}
        check(not bad and len(per) == _n * _inputs, f"스트림 불일치 {bad}")
        check(tmps(d) == [], "tmp 잔존")
        check(all(is_uuid_name(n) for n in files), "이름 형식 오류")
        return (f"파일 {len(files)}개, 레코드 {total:,}건 정확히 1번씩, "
                f"같은 ms에 생성된 파일 {same_ms(list(files))}개도 충돌 없음")


@case("E04", "다중 프로세스", "16개 프로세스 동시 기동 + 없는 중첩 디렉토리 mkdir", "모두 기동")
def _(wd):
    fails = 0
    for r in range(3):
        d = os.path.join(wd, f"r{r}", "a", "b", "c")
        procs = multi_writers(os.path.join(wd, f"r{r}"), d, 16, 0, rate=1)
        time.sleep(2.5)
        alive = sum(1 for p in procs if p.alive())
        fails += 16 - alive
        for p in procs:
            p.stop()
    check(fails == 0, f"{fails}/48 기동 실패")
    return "3회 × 16개 = 48개 모두 기동"


@case("E05", "다중 프로세스", "접두사가 다른 프로세스 4개가 같은 디렉토리", "접두사별 분리, 서로의 tmp 정리 안 함")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    old = time.time() - 3600
    foreign = [f"w{p}-01890a5d-ac96-774b-bcce-b30209900000.log.tmp" for p in range(4)]
    for n in foreign:
        with open(os.path.join(d, n), "w") as f:
            f.write("x")
        os.utime(os.path.join(d, n), (old, old))
    procs = []
    for p in range(4):
        o = uuid_out(d, uuid_file_prefix=f"w{p}-")
        procs.append(Flb(os.path.join(wd, f"p{p}"), make_yaml([dummy(record={"proc": p, "message": "m"},
                         samples=200, rate=200)], [o]), name=f"p{p}"))
    procs[0].start()
    time.sleep(1.5)
    after_first = sorted(n for n in tmps(d))
    for p in procs[1:]:
        p.start()
    ok = wait_for(lambda: count_json(d) == 800, 20)
    for p in procs:
        p.stop()
    check(ok, f"레코드 {count_json(d)}")
    check(after_first == sorted(foreign[1:]), f"w0 기동 후 남은 tmp: {after_first}")
    check(tmps(d) == [], f"최종 tmp: {tmps(d)}")
    per = {}
    for n in finals(d):
        per[n.split("-")[0]] = per.get(n.split("-")[0], 0) + 1
    return f"w0 기동 시 자기 tmp만 삭제, 접두사별 파일 {per}"


@case("E06", "다중 프로세스", "SIGKILL 크래시 5회 반복 + 재시작 (3개 중 1개)", "완성 파일 항상 온전, 재시작 시 tmp 정리")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    procs = []
    for p in range(3):
        o = uuid_out(d, retry_limit="no_limits", uuid_file_tmp_max_age="1s")
        procs.append(Flb(os.path.join(wd, f"p{p}"), make_yaml(
            [dummy(record={"proc": p, "message": "m"}, samples=0, rate=1000,
                   **{"storage.type": "filesystem"})], [o],
            service={"flush": "0.1", "storage.path": os.path.join(wd, f"p{p}", "storage")}),
            name=f"p{p}"))
    for p in procs:
        p.start()
    kills = 0
    for i in range(5):
        time.sleep(random.uniform(0.8, 2.0))
        procs[0].kill9()
        kills += 1
        check(count_json(d) >= 0, "크래시 직후 불완전 파일 발견")
        time.sleep(1.5)
        procs[0].start()
    time.sleep(2)
    for p in procs:
        p.stop()
    files = json_records(d)
    n = sum(len(v) for v in files.values())
    check(tmps(d) == [], f"tmp 잔존 {tmps(d)}")
    per = {}
    for rs in files.values():
        for r in rs:
            per[r["proc"]] = per.get(r["proc"], 0) + 1
    return f"kill -9 {kills}회, 파일 {len(files)}개 / 레코드 {n:,}건 전부 파싱 성공, 프로세스별 {per}"


@case("E07", "다중 프로세스", "기록 4개 + 소비자 2개 동시 실행 (rename으로 가져가기)",
      "소비자가 불완전 파일을 본 적 0, 레코드 정확히 1번씩")
def _(wd):
    d = os.path.join(wd, "out")
    os.makedirs(d)
    work = [os.path.join(wd, f"work{c}") for c in range(2)]
    for w in work:
        os.makedirs(w)
    stop = threading.Event()
    stats = {"files": 0, "records": 0, "partial": 0, "lost_race": 0, "per": {}}
    lock = threading.Lock()

    def consumer(c):
        while True:
            names = [n for n in os.listdir(d) if n.endswith(".log")]
            if not names and stop.is_set():
                return
            for n in names:
                dst = os.path.join(work[c], n)
                try:
                    os.rename(os.path.join(d, n), dst)
                except FileNotFoundError:
                    with lock:
                        stats["lost_race"] += 1
                    continue
                with open(dst, encoding="utf-8") as f:
                    data = f.read()
                ok = data.endswith("\n")
                try:
                    recs = [json.loads(l) for l in data.splitlines()]
                except ValueError:
                    ok, recs = False, []
                with lock:
                    stats["files"] += 1
                    if not ok:
                        stats["partial"] += 1
                    for r in recs:
                        stats["records"] += 1
                        stats["per"][r["proc"]] = stats["per"].get(r["proc"], 0) + 1
                os.remove(dst)
            time.sleep(0.005)

    threads = [threading.Thread(target=consumer, args=(c,)) for c in range(2)]
    for t in threads:
        t.start()
    procs = multi_writers(wd, d, 4, 1000, rate=500, flush="0.1")
    wait_for(lambda: stats["records"] >= 4000, 40, 0.3)
    for p in procs:
        p.stop()
    stop.set()
    for t in threads:
        t.join(30)
    check(stats["partial"] == 0, f"불완전 파일 {stats['partial']}개")
    check(stats["records"] == 4000, f"레코드 {stats['records']}")
    check(all(stats["per"].get(p) == 1000 for p in range(4)), str(stats["per"]))
    return (f"소비 파일 {stats['files']}개 / 레코드 4,000건 정확히 1번씩, "
            f"불완전 파일 0, 소비자 간 경합(다른 쪽이 먼저 가져감) {stats['lost_race']}회")


# ================================================================= main

def git_commit():
    try:
        return subprocess.check_output(["git", "-C", REPO, "log", "-1", "--format=%h %s"],
                                       text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main():
    if not os.access(BIN, os.X_OK):
        print(f"fluent-bit binary not found: {BIN} (set FLUENT_BIT_BINARY)")
        return 2
    only = set(sys.argv[1:])
    shutil.rmtree(ROOT, ignore_errors=True)
    os.makedirs(ROOT)
    results = []
    for c in CASES:
        if only and c["id"] not in only:
            continue
        wd = os.path.join(ROOT, c["id"])
        t0 = time.time()
        try:
            detail = c["fn"](wd)
            status = "PASS"
        except Fail as e:
            status, detail = "FAIL", str(e)
        except Exception as e:  # noqa
            status, detail = "ERROR", f"{type(e).__name__}: {e}\n{traceback.format_exc()[-600:]}"
        dur = time.time() - t0
        detail = str(detail).replace(ROOT + os.sep, "").replace(ROOT, "")
        results.append({k: c[k] for k in ("id", "category", "title", "expected")} |
                       {"status": status, "detail": detail, "seconds": round(dur, 1)})
        print(f"{c['id']} {status:5} {dur:5.1f}s  {c['title']}  ->  {detail}", flush=True)
    os.makedirs(REPORT_DIR, exist_ok=True)
    with open(RESULTS, "w") as f:
        json.dump(results, f, ensure_ascii=False, indent=1)
    with open(META, "w") as f:
        json.dump({"commit": git_commit(), "platform": f"{platform.system()} {platform.machine()}",
                   "date": time.strftime("%Y-%m-%d")}, f, ensure_ascii=False, indent=1)
    bad = [r for r in results if r["status"] != "PASS"]
    print(f"\n{len(results) - len(bad)}/{len(results)} PASS")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
