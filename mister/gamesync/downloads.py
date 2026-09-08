"""Resumable ROM download queue for the MiSTer client.

Mirrors the behaviour of ``steamdeck/download_manager.py`` and
``android/.../sync/DownloadManager.kt`` - ``.part`` files, HTTP ``Range``
resume, atomic rename on completion - with two differences forced by the
hardware:

* MiSTer has **no ``sqlite3`` module**, so the queue persists to JSON.
* One download at a time. The device has 492 MB of RAM, the SD card is exfat
  mounted ``sync``, and the NIC is 100 Mb; running three at once would only
  make each slower.

Progress is persisted as it goes, so a multi-gigabyte download survives the app
being closed or the console being switched off.
"""

from __future__ import annotations

import json
import os
import posixpath
import time
import urllib.error
import urllib.parse
import urllib.request

from shared.mister import MISTER_CONFIG_DIR
from shared.mister_install import (
    bios_seed_sources,
    install_target,
    safe_file_name,
)

QUEUE_PATH = posixpath.join(MISTER_CONFIG_DIR, "downloads.json")

CHUNK = 256 * 1024
#: How often progress is written back to disk while a download runs.
PERSIST_INTERVAL = 3.0

QUEUED = "queued"
DOWNLOADING = "downloading"
DONE = "done"
FAILED = "failed"
CANCELLED = "cancelled"


class Download:
    __slots__ = ("rom_id", "name", "system", "filename", "size", "directory",
                 "target", "status", "received", "error")

    def __init__(self, rom_id, name, system, filename, size=0, directory="",
                 target="", status=QUEUED, received=0, error=""):
        self.rom_id = rom_id
        self.name = name
        self.system = system
        self.filename = filename
        self.size = size
        self.directory = directory
        self.target = target
        self.status = status
        self.received = received
        self.error = error

    @property
    def progress(self) -> float:
        if self.size <= 0:
            return 0.0
        return min(1.0, self.received / float(self.size))

    def to_dict(self):
        return {slot: getattr(self, slot) for slot in self.__slots__}

    @classmethod
    def from_dict(cls, data):
        return cls(**{key: data.get(key) for key in cls.__slots__
                      if key in data})


class DownloadQueue:
    def __init__(self, client=None, provider=None, rom_target="sd"):
        self.client = client
        self.provider = provider
        self.rom_target = rom_target
        self.items = []
        self.load()

    # ------------------------------------------------------------ persistence

    def load(self):
        try:
            with open(QUEUE_PATH, "r") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            self.items = []
            return
        items = []
        dropped = 0
        for raw in data.get("downloads", []) if isinstance(data, dict) else []:
            try:
                item = Download.from_dict(raw)
            except TypeError:
                continue
            # A finished download is history by the next run; the game is on
            # the Installed tab now. Queued and failed ones are still work.
            if item.status in (DONE, CANCELLED):
                dropped += 1
                continue
            # Anything caught mid-flight last run is resumable, not lost: the
            # .part file is still there and Range picks up where it stopped.
            if item.status == DOWNLOADING:
                item.status = QUEUED
            items.append(item)
        self.items = items
        if dropped:
            self.save()

    def save(self):
        payload = {"downloads": [item.to_dict() for item in self.items]}
        try:
            os.makedirs(MISTER_CONFIG_DIR, exist_ok=True)
        except OSError:
            pass
        temp = QUEUE_PATH + ".part"
        try:
            with open(temp, "w") as handle:
                json.dump(payload, handle, indent=2)
            os.replace(temp, QUEUE_PATH)
        except OSError:
            pass

    # ------------------------------------------------------------------ queue

    def enqueue(self, rom):
        """Add one catalogue row. Returns the Download, or None if refused."""
        rom_id = str(rom.get("rom_id") or rom.get("title_id") or "")
        if not rom_id:
            return None
        for existing in self.items:
            if existing.rom_id == rom_id and existing.status in (
                    QUEUED, DOWNLOADING, DONE):
                return existing

        system = str(rom.get("system") or "").upper()
        filename = safe_file_name(rom.get("filename") or rom.get("name") or "")
        display = str(rom.get("name") or "")
        directory, target_name = install_target(
            self.provider, system, filename, display, self.rom_target)
        if not directory:
            item = Download(rom_id, display or filename, system, filename,
                            int(rom.get("size") or 0), status=FAILED)
            item.error = "no MiSTer folder for %s" % (system or "?")
            self.items.append(item)
            self.save()
            return item

        item = Download(
            rom_id=rom_id,
            name=display or filename,
            system=system,
            filename=target_name,
            size=int(rom.get("size") or 0),
            directory=directory,
            target=posixpath.join(directory, target_name),
        )
        self.items.append(item)
        self.save()
        return item

    def pending(self):
        return [item for item in self.items if item.status == QUEUED]

    def clear_finished(self):
        self.items = [item for item in self.items
                      if item.status in (QUEUED, DOWNLOADING)]
        self.save()

    def remove(self, item):
        if item in self.items:
            self.items.remove(item)
            self.save()

    def retry(self, item):
        """Put a failed download back in the queue. Returns True if it was.

        Whatever ``.part`` the failed attempt left is kept: the next run
        resumes from it with a Range request rather than starting over.
        A download that failed before it had a destination (no core folder
        for the system) is re-resolved, since that is what installing the
        core - or switching ROM target - fixes.
        """
        if item.status != FAILED:
            return False
        if not item.target:
            directory, target_name = install_target(
                self.provider, item.system, item.filename, item.name,
                self.rom_target)
            if not directory:
                return False
            item.directory = directory
            item.filename = target_name
            item.target = posixpath.join(directory, target_name)
        item.status = QUEUED
        item.error = ""
        self.save()
        return True

    # --------------------------------------------------------------- transfer

    def run_next(self, progress=None):
        """Download the next queued item. Returns it, or None when idle."""
        pending = self.pending()
        if not pending:
            return None
        item = pending[0]
        try:
            self._download(item, progress)
            item.status = DONE
            item.error = ""
        except Exception as exc:
            item.status = FAILED
            item.error = str(exc)
        self.save()
        return item

    def run_all(self, progress=None):
        done = failed = 0
        while self.pending():
            item = self.run_next(progress)
            if item is None:
                break
            if item.status == DONE:
                done += 1
            else:
                failed += 1
        return done, failed

    def _download(self, item, progress=None):
        item.status = DOWNLOADING
        self.save()

        self._prepare_directory(item)

        part = item.target + ".part"
        existing = 0
        if os.path.exists(part):
            existing = os.path.getsize(part)

        request = urllib.request.Request(
            self._url(item), method="GET")
        if self.client is not None and self.client.api_key:
            request.add_header("X-API-Key", self.client.api_key)
        if existing:
            request.add_header("Range", "bytes=%d-" % existing)

        response = urllib.request.urlopen(request, timeout=60)
        resumed = existing and response.status == 206
        if existing and not resumed:
            # The server ignored the range; start over rather than corrupting
            # the file by appending to a partial one.
            existing = 0

        total = item.size
        header_length = response.headers.get("Content-Length")
        if header_length:
            try:
                total = int(header_length) + (existing if resumed else 0)
            except ValueError:
                pass
        content_range = response.headers.get("Content-Range")
        if content_range and "/" in content_range:
            try:
                total = int(content_range.rsplit("/", 1)[1])
            except ValueError:
                pass
        if total:
            item.size = total

        item.received = existing if resumed else 0
        last_persist = time.time()
        mode = "ab" if resumed else "wb"

        with response, open(part, mode) as handle:
            while True:
                chunk = response.read(CHUNK)
                if not chunk:
                    break
                handle.write(chunk)
                item.received += len(chunk)
                now = time.time()
                if now - last_persist > PERSIST_INTERVAL:
                    handle.flush()
                    self.save()
                    last_persist = now
                if progress:
                    progress(item)
            handle.flush()
            try:
                os.fsync(handle.fileno())
            except OSError:
                pass

        os.replace(part, item.target)
        item.received = os.path.getsize(item.target)

    def _url(self, item):
        base = self.client.base_url if self.client is not None else ""
        return "%s/roms/%s" % (base,
                               urllib.parse.quote(str(item.rom_id), safe=""))

    def _prepare_directory(self, item):
        """Create the target folder, seeding a USB core folder's BIOS first.

        A core stops looking at the SD card as soon as the USB folder exists,
        so the BIOS has to be there before the first game is.
        """
        created = not os.path.isdir(item.directory)
        try:
            os.makedirs(item.directory, exist_ok=True)
        except OSError as exc:
            raise RuntimeError("cannot create %s: %s" % (item.directory, exc))

        if not created or self.provider is None:
            return
        for source in bios_seed_sources(self.provider, item.system,
                                        self.rom_target):
            # The BIOS belongs beside the games folder for the core, which for
            # a CD system is the parent of the per-game folder.
            destination_dir = self._core_dir(item)
            destination = posixpath.join(destination_dir,
                                         posixpath.basename(source))
            if os.path.exists(destination):
                continue
            try:
                os.makedirs(destination_dir, exist_ok=True)
                with open(source, "rb") as src, open(destination, "wb") as dst:
                    dst.write(src.read())
            except OSError:
                pass

    def _core_dir(self, item):
        """The ``games/<Core>`` folder, above any per-game subfolder."""
        from shared.mister_install import games_root

        root = games_root(self.rom_target).rstrip("/")
        directory = item.directory.rstrip("/")
        if not directory.startswith(root + "/"):
            return directory
        remainder = directory[len(root) + 1:]
        first = remainder.split("/", 1)[0]
        return posixpath.join(root, first)
