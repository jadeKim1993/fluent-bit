import json
import os
import re
import signal
import time

import pytest

from utils.fluent_bit_manager import FluentBitManager
from utils.memory_check import memory_check_enabled


CONFIG_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../config"))

UUIDV7_NAME = re.compile(
    r"^relay-[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\.log$"
)

ENV_KEYS = ("UUID_FILE_DIR", "UUID_FILE_PROC", "UUID_FILE_SAMPLES",
            "UUID_FILE_RATE", "UUID_FILE_STORAGE")


class UuidFileWriters:
    """Several Fluent Bit processes writing into the same directory."""

    def __init__(self, out_dir, procs, samples, config="out_file_uuid.yaml",
                 rate=100, storage_dir=None):
        self.out_dir = str(out_dir)
        self.procs = procs
        self.samples = samples
        self.rate = rate
        self.storage_dir = storage_dir
        self.config_file = os.path.join(CONFIG_DIR, config)
        self.managers = {}

    def start_proc(self, proc):
        previous = {key: os.environ.get(key) for key in ENV_KEYS}
        try:
            os.environ["UUID_FILE_DIR"] = self.out_dir
            os.environ["UUID_FILE_PROC"] = f"p{proc}"
            os.environ["UUID_FILE_SAMPLES"] = str(self.samples)
            os.environ["UUID_FILE_RATE"] = str(self.rate)
            if self.storage_dir is not None:
                os.environ["UUID_FILE_STORAGE"] = os.path.join(str(self.storage_dir), f"p{proc}")
            manager = FluentBitManager(self.config_file)
            self.managers[proc] = manager
            manager.start()
        finally:
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

    def start(self):
        for proc in range(self.procs):
            self.start_proc(proc)

    def kill(self, proc):
        manager = self.managers[proc]
        os.kill(manager.target_pid, signal.SIGKILL)
        manager.process.wait(timeout=10)

    def stop(self):
        for manager in self.managers.values():
            manager.stop()

    def wait_for_records(self, expected, timeout=None):
        if timeout is None:
            timeout = 120 if memory_check_enabled() else 60
        deadline = time.time() + timeout
        while time.time() < deadline:
            if count_records(self.out_dir) >= expected:
                return
            time.sleep(0.5)
        raise AssertionError(
            f"timed out waiting for {expected} records in {self.out_dir}, "
            f"got {count_records(self.out_dir)}"
        )


def read_published(out_dir):
    """Parse every published file; any partial or broken line fails here."""
    files = {}
    for name in sorted(os.listdir(out_dir)):
        if not name.endswith(".log"):
            continue
        with open(os.path.join(out_dir, name), "r", encoding="utf-8") as handle:
            data = handle.read()
        assert data.endswith("\n"), f"{name} is not complete"
        files[name] = [json.loads(line) for line in data.splitlines()]
    return files


def count_records(out_dir):
    return sum(len(records) for records in read_published(out_dir).values())


def tmp_files(out_dir):
    return [n for n in os.listdir(out_dir) if n.endswith(".tmp")]


def assert_published(out_dir):
    """Check the file invariants and return the record count per stream."""
    assert tmp_files(out_dir) == []

    files = read_published(out_dir)
    assert files
    per_stream = {}
    for name, records in files.items():
        assert UUIDV7_NAME.match(name), name
        assert records, f"empty file {name}"
        streams = {(r["proc"], r.get("input", "-")) for r in records}
        # one chunk per file: records of different processes/inputs never mix
        assert len(streams) == 1, f"{name} mixes {streams}"
        for record in records:
            assert record["message"] == "hello uuid_file"
            key = (record["proc"], record.get("input", "-"))
            per_stream[key] = per_stream.get(key, 0) + 1
    return files, per_stream


def test_out_file_uuid_file_writes_complete_files(tmp_path):
    out_dir = tmp_path / "relay"
    writers = UuidFileWriters(out_dir, procs=1, samples=150)
    writers.start()
    try:
        writers.wait_for_records(150)
    finally:
        writers.stop()

    files, per_stream = assert_published(str(out_dir))
    assert per_stream == {("p0", "-"): 150}
    # flush every 0.5s while the input runs for ~1.5s
    assert len(files) >= 2


def test_out_file_uuid_file_multiple_processes_share_directory(tmp_path):
    out_dir = tmp_path / "relay"
    writers = UuidFileWriters(out_dir, procs=4, samples=200)
    writers.start()
    try:
        writers.wait_for_records(4 * 200)
    finally:
        writers.stop()

    _, per_stream = assert_published(str(out_dir))
    # nothing lost, overwritten or duplicated
    assert per_stream == {(f"p{i}", "-"): 200 for i in range(4)}


def test_out_file_uuid_file_stress_many_processes(tmp_path):
    """8 processes x 2 inputs x 4 workers, flushing every 100ms."""
    procs = 8
    samples = 400
    out_dir = tmp_path / "relay"
    writers = UuidFileWriters(out_dir, procs=procs, samples=samples,
                              config="out_file_uuid_stress.yaml", rate=400)
    writers.start()
    try:
        writers.wait_for_records(procs * 2 * samples)
    finally:
        writers.stop()

    files, per_stream = assert_published(str(out_dir))
    assert per_stream == {
        (f"p{i}", inp): samples for i in range(procs) for inp in ("a", "b")
    }
    assert len(files) >= procs * 2


def test_out_file_uuid_file_removes_only_stale_tmp(tmp_path):
    out_dir = tmp_path / "relay"
    out_dir.mkdir()
    stale = out_dir / "relay-01890a5d-ac96-774b-bcce-b302099a8057.log.tmp"
    fresh = out_dir / "relay-01890a5d-ac96-774b-bcce-b302099a8058.log.tmp"
    foreign = out_dir / "other.log.tmp"
    for path in (stale, fresh, foreign):
        path.write_text("partial")
    old = time.time() - 3600
    os.utime(stale, (old, old))
    os.utime(foreign, (old, old))

    writers = UuidFileWriters(out_dir, procs=1, samples=1)
    writers.start()
    try:
        writers.wait_for_records(1)
    finally:
        writers.stop()

    assert not stale.exists()
    assert fresh.exists()
    assert foreign.exists()


@pytest.mark.skipif(memory_check_enabled(),
                    reason="SIGKILL is reported as a failure by the memory checker")
def test_out_file_uuid_file_crash_keeps_published_files_complete(tmp_path):
    """
    Kill one writer with SIGKILL while every process keeps writing, then
    restart it on its filesystem buffer: published files stay complete, the
    survivors are unaffected and the restarted writer cleans up and resumes.
    """
    procs = 3
    out_dir = tmp_path / "relay"
    writers = UuidFileWriters(out_dir, procs=procs, samples=0, rate=500,
                              config="out_file_uuid_crash.yaml",
                              storage_dir=tmp_path / "storage")
    writers.start()
    try:
        writers.wait_for_records(1000)
        writers.kill(0)

        # survivors keep publishing complete files next to the crashed writer
        before = count_records(str(out_dir))
        writers.wait_for_records(before + 500)

        # leftovers of the crashed writer become stale (tmp_max_age is 1s)
        time.sleep(2.5)
        writers.start_proc(0)
        restarted_at = count_records(str(out_dir))
        writers.wait_for_records(restarted_at + 1000)
    finally:
        writers.stop()

    # after a clean shutdown no temporary file survives
    _, per_stream = assert_published(str(out_dir))
    assert set(per_stream) == {(f"p{i}", "a") for i in range(procs)}
