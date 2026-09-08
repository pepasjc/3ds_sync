"""Walking a MiSTer's save and games directories, over any transport.

The desktop client reaches a MiSTer over SFTP and the on-device client reads
the same files locally. The *rules* are identical in both cases - which folders
map to which system, which extensions count, where a downloaded save has to be
written so the core finds it - so they live here once, behind a tiny file
provider, rather than being written twice and drifting.

A provider needs four methods: ``listdir``, ``is_dir``, ``stat`` and ``read``.
:class:`LocalProvider` is the on-device one; the desktop supplies an SFTP-backed
equivalent.
"""

from __future__ import annotations

import os
import posixpath
from typing import Callable, List, Optional

from shared.mister import (
    MISTER_FOLDER_TO_SYSTEM,
    MISTER_GAMES_ROOTS,
    MISTER_SHARED_SAVE_FOLDERS,
    mister_system_folder_candidates,
    mister_system_save_folder_candidates,
)
from shared.rom_id import make_title_id, normalize_rom_name
from shared.systems import SAVE_EXTENSIONS

MISTER_SAVES_DIR = "/media/fat/saves"

#: MiSTer cores always write ``.sav`` regardless of what other emulators use
#: for the system, so a downloaded save must land with this extension or the
#: core will never see it.
MISTER_SAVE_EXT = ".sav"

__all__ = [
    "LocalProvider",
    "system_for_save",
    "MISTER_SAVES_DIR",
    "MiSTerSaveFile",
    "build_save_path",
    "find_installed_rom_stem",
    "save_folder_for_system",
    "scan_saves",
]


class LocalProvider:
    """Reads the MiSTer's own filesystem, for the on-device client."""

    def listdir(self, path: str) -> List[str]:
        try:
            return os.listdir(path)
        except OSError:
            return []

    def is_dir(self, path: str) -> bool:
        return os.path.isdir(path)

    def stat(self, path: str):
        """Return ``(size, mtime)``, or ``(0, 0.0)`` when unavailable."""
        try:
            info = os.stat(path)
            return int(info.st_size), float(info.st_mtime)
        except OSError:
            return 0, 0.0

    def read(self, path: str) -> bytes:
        with open(path, "rb") as handle:
            return handle.read()


class MiSTerSaveFile:
    """One save file found on the device, before its identity is resolved."""

    __slots__ = ("system", "folder", "filename", "path", "title_id", "size",
                 "mtime")

    def __init__(self, system, folder, filename, path, title_id, size, mtime):
        self.system = system
        self.folder = folder
        self.filename = filename
        self.path = path
        self.title_id = title_id
        self.size = size
        self.mtime = mtime

    @property
    def stem(self) -> str:
        return posixpath.splitext(self.filename)[0]

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return "MiSTerSaveFile(%s, %s)" % (self.system, self.filename)


def scan_saves(provider, saves_root: str = MISTER_SAVES_DIR,
               systems: Optional[set] = None) -> List[MiSTerSaveFile]:
    """Every recognised save under ``saves_root``.

    Folders that map to no known system are skipped rather than guessed at, and
    the full save-extension set is honoured - the legacy shell script only
    looked at three of the twelve.
    """
    found: List[MiSTerSaveFile] = []
    extensions = tuple(SAVE_EXTENSIONS)

    for folder in sorted(provider.listdir(saves_root)):
        system = MISTER_FOLDER_TO_SYSTEM.get(folder)
        if not system:
            continue
        if systems and system not in systems:
            continue
        folder_path = posixpath.join(saves_root, folder)
        if not provider.is_dir(folder_path):
            continue

        for filename in sorted(provider.listdir(folder_path)):
            if not filename.lower().endswith(extensions):
                continue
            path = posixpath.join(folder_path, filename)
            stem = posixpath.splitext(filename)[0]
            # One folder can serve two systems; the installed game decides.
            resolved = system_for_save(provider, folder, system, stem)
            if systems and resolved not in systems and system not in systems:
                continue
            try:
                title_id = make_title_id(resolved, filename)
            except Exception:
                continue
            size, mtime = provider.stat(path)
            found.append(MiSTerSaveFile(resolved, folder, filename, path,
                                        title_id, size, mtime))
    return _prefer_canonical_folder(provider, found, saves_root)


def _prefer_canonical_folder(provider, found, saves_root):
    """Drop a duplicate of the same save sitting in a non-canonical folder.

    A save can end up in two places - ``saves/TGFX16-CD`` was a plausible
    guess for PC Engine CD before it turned out the core writes everything to
    ``saves/TGFX16`` - and the two would then show as two rows fighting over
    one server slot. The core only ever reads one of them, so that is the one
    to keep.
    """
    by_identity = {}
    for item in found:
        by_identity.setdefault((item.system, item.title_id), []).append(item)

    kept = []
    for (system, _title_id), items in by_identity.items():
        if len(items) == 1:
            kept.extend(items)
            continue
        canonical = save_folder_for_system(provider, system, saves_root)
        preferred = [item for item in items if item.folder == canonical]
        kept.extend(preferred or items[:1])

    # Preserve the original folder/filename ordering.
    order = {id(item): index for index, item in enumerate(found)}
    return sorted(kept, key=lambda item: order[id(item)])


def system_for_save(provider, folder: str, system: str, stem: str,
                    games_roots=None) -> str:
    """Which system a save belongs to, when one folder serves two.

    The TurboGrafx-16 core writes HuCard and CD saves into the same
    ``saves/TGFX16``, so the folder no longer identifies the system. The games
    folders are still separate, so the installed game decides: a save whose
    name matches a game in ``games/TGFX16-CD`` is a CD save.

    Falls back to the folder's usual system when nothing matches, which is the
    old behaviour.
    """
    shared = MISTER_SHARED_SAVE_FOLDERS.get(folder)
    if not shared:
        return system
    if games_roots is None:
        games_roots = [MISTER_GAMES_ROOTS["usb"], MISTER_GAMES_ROOTS["sd"]]

    wanted = normalize_rom_name(stem)
    if not wanted or wanted == "unknown":
        return system

    for candidate in shared:
        for root in games_roots:
            for folder_name in mister_system_folder_candidates(candidate):
                directory = posixpath.join(root, folder_name)
                if not provider.is_dir(directory):
                    continue
                for entry in provider.listdir(directory):
                    name = entry
                    if not provider.is_dir(posixpath.join(directory, entry)):
                        name = posixpath.splitext(entry)[0]
                    if normalize_rom_name(name) == wanted:
                        return candidate
    return system


def save_folder_for_system(provider, system: str,
                           saves_root: str = MISTER_SAVES_DIR) -> str:
    """The save folder name a core uses, preferring one that already exists.

    Cores were renamed over the years (``Genesis`` -> ``MegaDrive``,
    ``PCEngine`` -> ``TGFX16``), so an existing folder wins over the modern
    name; otherwise the modern name is created. Some cores also write their
    saves under another system's folder entirely - see
    ``MISTER_SYSTEM_SAVE_FOLDERS``.
    """
    candidates = mister_system_save_folder_candidates(system)
    if not candidates:
        return ""
    for candidate in candidates:
        if provider.is_dir(posixpath.join(saves_root, candidate)):
            return candidate
    return candidates[0]


def find_installed_rom_stem(
    provider,
    system: str,
    title_id: str,
    games_roots: Optional[List[str]] = None,
    catalog_lookup: Optional[Callable[[str, str], Optional[str]]] = None,
) -> Optional[str]:
    """Name of the installed game a save belongs to, or None.

    A MiSTer core names its save after the game file or folder it launched, so
    a downloaded save has to reuse that exact name. Two ordering rules matter:

    * USB is searched before the SD card, because cores look at
      ``/media/usb0`` first and a game there shadows the SD copy.
    * Directories are matched before files, because CD cores name the backup
      RAM after the *folder* holding the discs. A folder name is returned whole
      rather than split into stem and extension, since game folders routinely
      contain dots (``... v1.021+hotfix``).

    This mirrors ``desktop/sync_engine._mister_matching_rom_stem`` deliberately:
    the two clients must agree on which installed game a save belongs to.
    """
    if games_roots is None:
        games_roots = [MISTER_GAMES_ROOTS["usb"], MISTER_GAMES_ROOTS["sd"]]

    target = (title_id or "").upper()
    if not target:
        return None

    for root in games_roots:
        for folder in mister_system_folder_candidates(system):
            system_dir = posixpath.join(root, folder)
            if not provider.is_dir(system_dir):
                continue
            entries = sorted(provider.listdir(system_dir))
            directories, files = [], []
            for entry in entries:
                if provider.is_dir(posixpath.join(system_dir, entry)):
                    directories.append(entry)
                else:
                    files.append(entry)

            for entry in directories + files:
                is_dir = entry in directories
                stem = entry if is_dir else posixpath.splitext(entry)[0]
                if _matches(system, stem, entry, target, catalog_lookup):
                    return stem
    return None


def _matches(system, stem, entry, target, catalog_lookup) -> bool:
    try:
        if make_title_id(system, entry).upper() == target:
            return True
    except Exception:
        pass
    if catalog_lookup is not None:
        try:
            found = catalog_lookup(system, stem)
        except Exception:
            found = None
        if found and found.upper() == target:
            return True
    slug = normalize_rom_name(stem)
    return bool(slug) and slug != "unknown" and slug.upper() in target


def build_save_path(
    provider,
    system: str,
    title_id: str,
    game_name: str = "",
    saves_root: str = MISTER_SAVES_DIR,
    games_roots: Optional[List[str]] = None,
    catalog_lookup: Optional[Callable[[str, str], Optional[str]]] = None,
) -> str:
    """Where a save downloaded from the server has to be written.

    The file name has to be the installed game's name, because that is what the
    core will look for. Falling back to the server's name is a guess that only
    works when the two happen to agree.
    """
    folder = save_folder_for_system(provider, system, saves_root)
    if not folder:
        return ""

    stem = find_installed_rom_stem(provider, system, title_id, games_roots,
                                   catalog_lookup)
    if not stem:
        stem = _sanitize(game_name) or _sanitize(title_id)
    if not stem:
        return ""
    return posixpath.join(saves_root, folder, stem + MISTER_SAVE_EXT)


_UNSAFE = set('<>:"/\\|?*')


def _sanitize(name: str) -> str:
    cleaned = "".join(" " if ch in _UNSAFE else ch for ch in str(name or ""))
    return " ".join(cleaned.split()).strip(". ")
