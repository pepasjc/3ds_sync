"""The download queue between runs: finished entries are history, failed
ones can be put back."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for candidate in (ROOT, ROOT / "mister"):
    if str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from gamesync import downloads as gsdownloads  # noqa: E402


def write_queue(path, items):
    path.write_text(json.dumps({"downloads": items}))


def entry(rom_id, status, **extra):
    base = {"rom_id": rom_id, "name": rom_id, "system": "SNES",
            "filename": rom_id + ".sfc", "size": 10, "directory": "/games",
            "target": "/games/" + rom_id + ".sfc", "status": status,
            "received": 0, "error": ""}
    base.update(extra)
    return base


def test_done_and_cancelled_are_dropped_on_load(tmp_path, monkeypatch):
    queue_file = tmp_path / "downloads.json"
    monkeypatch.setattr(gsdownloads, "QUEUE_PATH", str(queue_file))
    write_queue(queue_file, [
        entry("a", gsdownloads.DONE),
        entry("b", gsdownloads.QUEUED),
        entry("c", gsdownloads.FAILED, error="boom"),
        entry("d", gsdownloads.DOWNLOADING, received=5),
        entry("e", gsdownloads.CANCELLED),
    ])

    queue = gsdownloads.DownloadQueue()

    assert [(i.rom_id, i.status) for i in queue.items] == [
        ("b", gsdownloads.QUEUED),
        ("c", gsdownloads.FAILED),
        ("d", gsdownloads.QUEUED),      # mid-flight last run: resumable
    ]
    # And the file on disk no longer carries the finished ones either.
    saved = json.loads(queue_file.read_text())["downloads"]
    assert [i["rom_id"] for i in saved] == ["b", "c", "d"]


def test_retry_requeues_a_failure_and_keeps_its_progress(tmp_path, monkeypatch):
    monkeypatch.setattr(gsdownloads, "QUEUE_PATH",
                        str(tmp_path / "downloads.json"))
    queue = gsdownloads.DownloadQueue()
    failed = gsdownloads.Download("x", "X", "SNES", "x.sfc", size=100,
                                  directory="/games", target="/games/x.sfc",
                                  status=gsdownloads.FAILED, received=40,
                                  error="timed out")
    fine = gsdownloads.Download("y", "Y", "SNES", "y.sfc",
                                status=gsdownloads.QUEUED)
    queue.items = [failed, fine]

    assert queue.retry(fine) is False
    assert queue.retry(failed) is True
    assert failed.status == gsdownloads.QUEUED
    assert failed.error == ""
    assert failed.received == 40
    assert queue.pending() == [failed, fine]
