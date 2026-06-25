from __future__ import annotations

import os
import re
import shutil
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

import requests

from config import get_api_headers, get_base_url
from systems import MISTER_SYSTEM_TO_FOLDER


ROM_FORMAT_OPTIONS: list[tuple[str, str]] = [
    ("auto", "Auto"),
    ("raw", "Raw"),
    ("cue", "CUE/BIN"),
    ("psio", "PSIO BIN/CU2"),
    ("gdi", "GDI"),
    ("iso", "ISO"),
    ("cso", "CSO"),
    ("cia", "CIA"),
    ("decrypted_cci", "Decrypted CCI"),
    ("eboot", "PSP EBOOT.PBP"),
    ("vcd", "PS1 VCD (POPStarter)"),
    ("cci", "Xbox CCI"),
    ("folder", "Extracted Folder"),
]
ROM_FORMAT_LABELS = dict(ROM_FORMAT_OPTIONS)

ARCHIVE_EXTRACT_FORMATS = {"cue", "psio", "gdi", "cci", "folder"}
# Formats where the server stitches every disc of a multi-disc game into a
# single output (one PSIO BIN/CU2 set, one multi-disc EBOOT.PBP).  For these
# we collapse the per-disc catalog rows into one installable entry; for raw /
# cue / chd each disc stays its own file.
COMBINED_DISC_FORMATS = {"psio", "eboot"}
_DISC_TAG_RE = re.compile(
    r"\s*[\(\[]\s*Dis[ck]\s*\d+(?:\s*of\s*\d+)?\s*[\)\]]", re.IGNORECASE
)
_BRACKET_TAG_RE = re.compile(r"\s*\[[^\]]*\]")
DISC_CUE_SYSTEMS = {
    "PS1",
    "PSX",
    "SAT",
    "SEGACD",
    "SCD",
    "PCECD",
    "TGCD",
    "NEOCD",
    "NGCD",
    "3DO",
    "PCFX",
    "JAGCD",
    "AMIGACD32",
}
DISC_ISO_SYSTEMS = {"PS2", "PSP", "GC", "WII", "XBOX", "X360", "XBOX360"}
EMULATOR_DEVICE_TYPES = {"RetroArch", "EmuDeck"}
HARDWARE_3DS_DEVICE_TYPES = {"Generic", "Everdrive", "CD Folder"}
XBOX_SYSTEMS = {"XBOX", "X360", "XBOX360"}
OPL_SYSTEMS = {"PS1", "PS2"}

# PSIO's menu refuses filenames > 60 chars and cannot render non-ASCII bytes.
# We keep folder + member names within this limit for PSIO installs.
PSIO_MAX_NAME = 60


ROM_SUBDIRS: dict[str, list[str]] = {
    "PS1": ["psx", "PSX", "PS1", "ps1", "PlayStation"],
    "PS2": ["ps2", "PS2"],
    "PS3": ["ps3", "PS3"],
    "PSP": ["psp", "PSP"],
    "GBA": ["gba", "GBA"],
    "GB": ["gb", "GB"],
    "GBC": ["gbc", "GBC"],
    "NES": ["nes", "NES"],
    "FDS": ["fds", "FDS"],
    "SNES": ["snes", "SNES"],
    "N64": ["n64", "N64"],
    "NDS": ["nds", "NDS"],
    "3DS": ["3ds", "n3ds", "Nintendo 3DS"],
    "GC": ["gc", "GC", "GameCube"],
    "WII": ["wii", "Wii"],
    "MD": ["megadrive", "genesis", "MD"],
    "SEGACD": ["segacd", "megacd", "Sega CD"],
    "SMS": ["mastersystem", "SMS"],
    "GG": ["gamegear", "GG"],
    "SAT": ["saturn", "Saturn"],
    "DC": ["dreamcast", "dc", "Dreamcast"],
    "32X": ["sega32x", "32x", "32X"],
    "PCE": ["pcengine", "tg16", "PCE"],
    "PCECD": ["pcenginecd", "tgcd", "PCECD"],
    "NEOGEO": ["neogeo", "NeoGeo"],
    "NGP": ["ngp", "NGP"],
    "NGPC": ["ngpc", "NGPC"],
    "WSWAN": ["wonderswan", "WSWAN"],
    "WSWANC": ["wonderswancolor", "WSWANC"],
    "A2600": ["atari2600", "A2600"],
    "A7800": ["atari7800", "A7800"],
    "LYNX": ["lynx", "Lynx"],
    "MAME": ["mame", "MAME", "arcade"],
    "ARCADE": ["arcade", "mame"],
    "XBOX": ["xbox", "ogxbox", "XBOX"],
}


@dataclass(frozen=True)
class InstallPlan:
    rom_id: str
    display_name: str
    system: str
    source_filename: str
    target_path: Path
    extract_format: str | None
    extract_archive: bool = False
    target_is_directory: bool = False

    @property
    def format_label(self) -> str:
        return ROM_FORMAT_LABELS.get(self.extract_format or "raw", "Raw")


def fetch_rom_catalog(system: str = "", search: str = "") -> list[dict]:
    params: dict[str, str | int] = {"limit": 20000}
    if system:
        params["system"] = system.upper()
    if search:
        params["search"] = search
    resp = requests.get(
        f"{get_base_url()}/api/v1/roms",
        headers=get_api_headers(),
        params=params,
        timeout=30,
    )
    resp.raise_for_status()
    return list(resp.json().get("roms", []))


def profile_systems(profile: dict) -> list[str]:
    if "systems" in profile:
        return [
            str(s.get("system", "")).upper()
            for s in profile.get("systems", [])
            if s.get("enabled", True) and str(s.get("system", "")).strip()
        ]
    system = str(profile.get("system", "")).upper()
    return [system] if system else []


def system_profile_info(profile: dict, system: str) -> dict:
    system = system.upper()
    for entry in profile.get("systems", []) or []:
        if str(entry.get("system", "")).upper() == system:
            return entry
    return {}


def profile_rom_format(profile: dict, system: str) -> str:
    info = system_profile_info(profile, system)
    return str(info.get("rom_format") or profile.get("rom_format") or "auto").strip().lower() or "auto"


def default_rom_format(profile: dict, rom: dict, system: str) -> str | None:
    system_up = (system or rom.get("system") or "").upper()
    device_type = str(profile.get("device_type", "Generic"))
    source_ext = Path(str(rom.get("filename") or "")).suffix.lower()
    advertised = {
        str(v).strip().lower()
        for v in (rom.get("extract_formats") or [])
        if str(v).strip()
    }
    legacy = str(rom.get("extract_format") or "").strip().lower()

    if system_up == "3DS":
        if device_type in EMULATOR_DEVICE_TYPES:
            return "decrypted_cci"
        if device_type in HARDWARE_3DS_DEVICE_TYPES or "cia" in advertised:
            return "cia"
        return "decrypted_cci" if "decrypted_cci" in advertised else None

    if device_type == "PSIO" and system_up in {"PS1", "PSX"}:
        return "psio"

    # OPL: PS1 → POPStarter VCD, PS2 → ISO (both CD and DVD media).
    if device_type == "OPL":
        if system_up == "PS1":
            return "vcd"
        if system_up == "PS2":
            return "iso"

    if system_up in XBOX_SYSTEMS:
        return "iso"

    if source_ext == ".rvz" and system_up in {"GC", "WII"}:
        return "iso"

    if source_ext != ".chd":
        if legacy in {"cue", "gdi", "iso", "cso", "rvz", "eboot"}:
            return legacy
        return None

    if system_up == "DC":
        return "gdi"
    if system_up in DISC_ISO_SYSTEMS:
        return "iso"
    if system_up in DISC_CUE_SYSTEMS:
        return "cue"
    if legacy:
        return legacy
    return None


def choose_extract_format(
    profile: dict,
    rom: dict,
    system: str,
    override_format: str = "",
) -> str | None:
    selected = (override_format or profile_rom_format(profile, system)).strip().lower()
    if selected in {"", "auto"}:
        return default_rom_format(profile, rom, system)
    if selected == "raw":
        return None
    return selected


def strip_disc_tag(name: str) -> str:
    """Drop ``(Disc N)`` / ``(Disk N of M)`` tokens from a game name."""
    cleaned = _DISC_TAG_RE.sub("", str(name or ""))
    return re.sub(r"\s{2,}", " ", cleaned).strip()


def _ascii_only(value: str) -> str:
    """Best-effort ASCII: decompose accents, drop combining marks, drop the rest.

    PSIO cannot render non-ASCII bytes; NFKD salvages accented Latin names
    (``Pokémon`` -> ``Pokemon``) while remaining CJK / symbol bytes are dropped.
    """
    decomposed = unicodedata.normalize("NFKD", str(value or ""))
    stripped = "".join(c for c in decomposed if not unicodedata.combining(c))
    return stripped.encode("ascii", "ignore").decode("ascii")


def clean_ps1_title(name: str) -> str:
    """PSIO install folder name: drop disc tags and ``[..]`` tags (translation /
    dump flags) but keep region parens like ``(USA)``.  Strips non-ASCII bytes
    and caps at ``PSIO_MAX_NAME`` so PSIO's menu can list the folder.  Mirrors
    the on-disk PSIO convention where the game folder is the bare title."""
    cleaned = _DISC_TAG_RE.sub("", str(name or ""))
    cleaned = _BRACKET_TAG_RE.sub("", cleaned)
    cleaned = _ascii_only(cleaned)
    cleaned = re.sub(r"\s{2,}", " ", cleaned).strip(" ._")
    if len(cleaned) > PSIO_MAX_NAME:
        cleaned = cleaned[:PSIO_MAX_NAME].rstrip(" ._")
    return cleaned or "game"


def group_multidisc_roms(
    profile: dict,
    roms: list[dict],
    system: str,
    override_format: str = "",
) -> list[dict]:
    """Collapse multi-disc PS1 groups into one catalog entry per game.

    Only collapses when the effective install format produces a single
    combined output (see ``COMBINED_DISC_FORMATS``).  The merged entry keeps
    the group's ``primary_rom_id`` as its ``rom_id`` — requesting that from the
    server returns the full multi-disc set (the server groups siblings by
    title_id).  A synthetic ``disc_members`` list and ``disc_total`` are
    attached for display.  Non-PS1 / single-disc / non-combined rows pass
    through unchanged and keep their original order.
    """
    result: list[dict] = []
    aggregates: dict[str, dict] = {}
    for rom in roms:
        try:
            fmt = choose_extract_format(profile, rom, system, override_format)
        except Exception:
            fmt = None
        total = int(rom.get("disc_total") or 1)
        primary = str(rom.get("primary_rom_id") or "")
        has_disc_tag = bool(
            _DISC_TAG_RE.search(str(rom.get("filename") or rom.get("name") or ""))
        )
        if fmt in COMBINED_DISC_FORMATS and total > 1 and primary and has_disc_tag:
            agg = aggregates.get(primary)
            if agg is None:
                agg = dict(rom)
                agg["rom_id"] = primary
                agg["disc_members"] = []
                aggregates[primary] = agg
                result.append(agg)
            agg["disc_members"].append(rom)
        else:
            result.append(rom)

    for agg in aggregates.values():
        members = sorted(
            agg["disc_members"], key=lambda r: int(r.get("disc_index") or 0)
        )
        primary_member = next(
            (m for m in members if str(m.get("rom_id")) == str(agg["rom_id"])),
            members[0],
        )
        agg["disc_members"] = members
        agg["disc_total"] = len(members)
        agg["size"] = sum(int(m.get("size") or 0) for m in members)
        agg["filename"] = primary_member.get("filename") or agg.get("filename")
        agg["name"] = strip_disc_tag(
            primary_member.get("name") or primary_member.get("filename") or ""
        )
    return result


def derive_download_filename(filename: str, extract_format: str | None) -> str:
    fmt = (extract_format or "").strip().lower()
    if not fmt:
        return filename

    stem = filename
    lower = stem.lower()
    if lower.endswith(".zip"):
        stem = stem[:-4]
        lower = stem.lower()
    for ext in (".3ds", ".cci", ".cia", ".chd", ".rvz", ".iso", ".cso", ".cue", ".gdi", ".bin", ".img"):
        if lower.endswith(ext):
            stem = stem[: -len(ext)]
            break

    if fmt == "decrypted_cci":
        return f"{stem}.cci"
    if fmt == "cia":
        return f"{stem}.cia"
    if fmt in {"iso", "rvz"}:
        return f"{stem}.iso"
    if fmt == "cso":
        return f"{stem}.cso"
    if fmt == "gdi":
        return f"{stem}.gdi"
    if fmt == "cue":
        return f"{stem}.cue"
    if fmt == "psio":
        return f"{stem}.cu2"
    if fmt == "eboot":
        return "EBOOT.PBP"
    if fmt == "vcd":
        return f"{stem}.vcd"
    if fmt == "cci":
        return f"{stem}.cci"
    return filename


def safe_folder_name(value: str) -> str:
    text = Path(value or "download").stem.strip()
    text = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", text)
    text = re.sub(r"\s+", " ", text).strip(" .")
    return text or "download"


def safe_file_name(value: str) -> str:
    """Reduce a server-supplied filename to a safe basename (keeps extension).

    Strips any directory components so a crafted catalog ``filename`` (e.g.
    ``../../x.rom``) can't escape the resolved target folder.
    """
    base = Path(str(value or "").replace("\\", "/")).name.strip()
    base = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", base).strip(" .")
    return base or "download.rom"


def _system_subdir(root: Path, system: str, device_type: str) -> Path:
    system_up = system.upper()
    if device_type == "MiSTer":
        folder = MISTER_SYSTEM_TO_FOLDER.get(system_up)
        if folder:
            return root / folder

    candidates = ROM_SUBDIRS.get(system_up, [system_up.lower(), system_up])
    for name in candidates:
        candidate = root / name
        if candidate.is_dir():
            return candidate

    if device_type in {"EmuDeck", "MiSTer"}:
        return root / candidates[0]
    return root


def resolve_profile_rom_folder(profile: dict, system: str) -> Path:
    info = system_profile_info(profile, system)
    override = str(info.get("rom_folder", "")).strip()
    if override:
        return Path(override).expanduser()

    base = Path(str(profile.get("path") or ".")).expanduser()
    if profile.get("systems"):
        return _system_subdir(base, system, str(profile.get("device_type", "")))
    return base


# ── OPL (Open PS2 Loader) install layout ────────────────────────────────────
#
# OPL reads PS2 games from ``DVD/`` (DVD-ROM) or ``CD/`` (CD-ROM) folders on a
# FAT32 USB drive, and PS1 games (via POPStarter) from ``POPS/``.  Filenames
# use the Sony disc serial in an OPL-parsed form: ``SLPS_204.36`` — the first
# four letters, an underscore, then the 5-digit number split as ``DDD.DD``.
# OPL treats the dot-separated tokens as ``<startup>.<display name>.<ext>``
# for PS2, so PS2 keeps the game name: ``SLPS_204.36.Game Name.iso``.  PS1
# POPStarter VCDs are matched to their VMC by filename prefix, so they use the
# serial only: ``SLUS_012.34.VCD``.

_SERIAL_RE = re.compile(r"^[A-Z]{4}\d{5}$")


def opl_disc_id(serial: str | None) -> str:
    """Convert a Sony PS1/PS2 disc serial to the OPL filename prefix.

    ``SLPS20436`` / ``SLPS-20436`` / ``slps_204.36`` → ``SLPS_204.36``.
    Returns ``""`` when the value doesn't resolve to a 4-letter + 5-digit
    Sony code (so callers can fall back to the bare game name).
    """
    compact = re.sub(r"[^A-Za-z0-9]", "", str(serial or "")).upper()
    if _SERIAL_RE.match(compact):
        letters = compact[:4]
        digits = compact[4:]
        return f"{letters}_{digits[:3]}.{digits[3:]}"
    return ""


def opl_ps2_media(rom: dict) -> str:
    """Return ``"CD"`` or ``"DVD"`` for a PS2 catalog row.

    The server's native ``extract_format`` recommendation encodes the media:
    DVD CHDs advertise ``iso``, CD CHDs advertise ``cue``.  Defaults to DVD
    (the overwhelming majority of PS2 titles) when the catalog can't tell us.
    """
    native = str(rom.get("extract_format") or "").strip().lower()
    if native == "cue":
        return "CD"
    return "DVD"


def clean_opl_name(name: str) -> str:
    """Sanitize a game name for the OPL filename middle token.

    Keeps spaces (OPL displays them) but strips characters illegal on FAT32
    and collapses ``(Disc N)`` tags (each disc is its own file).
    """
    cleaned = strip_disc_tag(str(name or ""))
    cleaned = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "", cleaned)
    cleaned = re.sub(r"\s+", " ", cleaned).strip(" .")
    return cleaned or "game"


def _build_opl_install_plan(
    profile: dict,
    rom: dict,
    system_up: str,
    override_format: str,
) -> InstallPlan:
    rom_id = str(rom.get("rom_id") or rom.get("title_id") or "")
    if not rom_id:
        raise ValueError("Catalog entry is missing a ROM id.")
    filename = safe_file_name(str(rom.get("filename") or f"{rom_id}.rom"))
    extract = choose_extract_format(profile, rom, system_up, override_format)
    target_root = resolve_profile_rom_folder(profile, system_up)
    display_name = str(rom.get("name") or Path(filename).stem or rom_id)
    serial_source = str(rom.get("title_id") or rom_id)
    disc_id = opl_disc_id(serial_source)
    name = clean_opl_name(display_name)

    if system_up == "PS1":
        # PS1 → POPStarter VCD in POPS/.  VCDs are matched to their VMC by
        # filename prefix, so name by serial only (no game name).
        folder = "POPS"
        ext = "vcd"
        if extract is None:
            extract = "vcd"
        base = disc_id or name
        target_path = target_root / folder / f"{base}.{ext}"
    else:  # PS2 — DVD/ vs CD/ by media, both served as .iso
        folder = "CD" if opl_ps2_media(rom) == "CD" else "DVD"
        ext = "iso"
        if extract is None:
            extract = "iso"
        # OPL parses ``<startup>.<display name>.iso``; serial first so it
        # boots correctly, name second for the OPL game list.
        if disc_id:
            target_path = target_root / folder / f"{disc_id}.{name}.{ext}"
        else:
            target_path = target_root / folder / f"{name}.{ext}"

    return InstallPlan(
        rom_id=rom_id,
        display_name=display_name,
        system=system_up,
        source_filename=filename,
        target_path=target_path,
        extract_format=extract,
    )


def build_install_plan(
    profile: dict,
    rom: dict,
    system: str | None = None,
    override_format: str = "",
) -> InstallPlan:
    system_up = (system or rom.get("system") or "").upper()
    # OPL (Open PS2 Loader) has its own folder/naming layout (DVD/, CD/,
    # POPS/ with serial-prefixed filenames) — handle it before the generic
    # extract/archive logic below.
    if str(profile.get("device_type", "")).strip() == "OPL" and system_up in OPL_SYSTEMS:
        return _build_opl_install_plan(profile, rom, system_up, override_format)
    rom_id = str(rom.get("rom_id") or rom.get("title_id") or "")
    if not rom_id:
        raise ValueError("Catalog entry is missing a ROM id.")
    filename = safe_file_name(str(rom.get("filename") or f"{rom_id}.rom"))
    extract = choose_extract_format(profile, rom, system_up, override_format)
    target_root = resolve_profile_rom_folder(profile, system_up)
    display_name = str(rom.get("name") or Path(filename).stem or rom_id)
    target_filename = derive_download_filename(filename, extract)

    if extract == "eboot":
        target_path = target_root / safe_folder_name(display_name) / "EBOOT.PBP"
        return InstallPlan(rom_id, display_name, system_up, filename, target_path, extract)

    if extract in ARCHIVE_EXTRACT_FORMATS or bool(rom.get("is_bundle")):
        # PSIO installs into a bare game folder (translation/disc tags stripped);
        # the server already names the BIN/CU2 inside it.
        folder_name = clean_ps1_title(display_name) if extract == "psio" else display_name
        target_dir = target_root / safe_folder_name(folder_name)
        return InstallPlan(
            rom_id=rom_id,
            display_name=display_name,
            system=system_up,
            source_filename=filename,
            target_path=target_dir,
            extract_format=extract,
            extract_archive=True,
            target_is_directory=True,
        )

    return InstallPlan(
        rom_id=rom_id,
        display_name=display_name,
        system=system_up,
        source_filename=filename,
        target_path=target_root / target_filename,
        extract_format=extract,
    )


def _safe_extract_zip(zip_path: Path, target_dir: Path) -> list[Path]:
    written: list[Path] = []
    target_root = target_dir.resolve()
    target_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            destination = (target_dir / info.filename).resolve()
            try:
                destination.relative_to(target_root)
            except ValueError as exc:
                raise ValueError(f"Refusing unsafe ZIP member: {info.filename}") from exc
            destination.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as src, open(destination, "wb") as dst:
                shutil.copyfileobj(src, dst)
            written.append(destination)
    return written


def install_rom(
    plan: InstallPlan,
    progress_callback: Callable[[int, int], None] | None = None,
) -> list[Path]:
    params = {}
    if plan.extract_format:
        params["extract"] = plan.extract_format

    target = plan.target_path
    tmp_parent = target if plan.target_is_directory else target.parent
    tmp_parent.mkdir(parents=True, exist_ok=True)
    tmp_path = tmp_parent / f".{safe_folder_name(target.name)}.part"
    if plan.extract_archive:
        tmp_path = tmp_parent / f".{safe_folder_name(target.name)}.zip.part"

    downloaded = 0
    with requests.get(
        f"{get_base_url()}/api/v1/roms/{plan.rom_id}",
        headers=get_api_headers(),
        params=params,
        stream=True,
        timeout=(30, 900),
    ) as resp:
        if resp.status_code != 200:
            detail = ""
            try:
                detail = resp.text.strip()
            except Exception:
                pass
            raise RuntimeError(
                f"HTTP {resp.status_code}" + (f": {detail}" if detail else "")
            )
        total = int(resp.headers.get("Content-Length", "0") or 0)
        with open(tmp_path, "wb") as fh:
            for chunk in resp.iter_content(chunk_size=256 * 1024):
                if not chunk:
                    continue
                fh.write(chunk)
                downloaded += len(chunk)
                if progress_callback:
                    progress_callback(downloaded, total)

    try:
        if plan.extract_archive:
            written = _safe_extract_zip(tmp_path, target)
            return written

        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            tmp_path.replace(target)
        except OSError:
            shutil.copy2(tmp_path, target)
            tmp_path.unlink(missing_ok=True)
        return [target]
    finally:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


def available_systems_for_profiles(profiles: Iterable[dict]) -> list[str]:
    systems: set[str] = set()
    for profile in profiles:
        systems.update(profile_systems(profile))
    return sorted(s for s in systems if s)


# ── PSIO install sanitization ────────────────────────────────────────────────
#
# PSIO's menu refuses filenames > 60 chars and cannot render non-ASCII bytes.
# Games installed before these limits were enforced (or copied onto the SD
# card by hand) need renaming.  ``sanitize_installed_files`` walks a profile's
# ROM folder and renames every offending file / folder, keeping each game's
# ``.bin``/``.cu2`` pair on a matching stem and rewriting ``MULTIDISC.LST``.

_PSIO_PAIR_EXTS = (".bin", ".cu2")


def psio_safe_name(name: str, reserve: int = 0) -> str:
    """ASCII-only, ``PSIO_MAX_NAME - reserve`` chars.  ``reserve`` is the number
    of characters to leave for a suffix the caller appends (e.g. an extension
    or `` (Disc N)``)."""
    text = _ascii_only(name)
    text = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", text)
    text = re.sub(r"\s+", " ", text).strip(" ._")
    limit = max(1, PSIO_MAX_NAME - reserve)
    if len(text) > limit:
        text = text[:limit].rstrip(" ._")
    return text or "game"


def _free_pair_stem(parent: Path, desired: str, own: set[Path]) -> str:
    """A stem whose ``.bin`` and ``.cu2`` are both free in ``parent``.

    Collisions (two different source stems collapsing to the same ASCII name)
    get a ``~n`` suffix, re-truncated so the final filename still fits.
    """
    if _pair_stem_free(parent, desired, own):
        return desired
    n = 2
    while n < 1000:
        suffix = f"~{n}"
        limit = max(1, PSIO_MAX_NAME - 4 - len(suffix))
        candidate = desired[:limit].rstrip(" ._") + suffix
        if _pair_stem_free(parent, candidate, own):
            return candidate
        n += 1
    return desired


def _pair_stem_free(parent: Path, stem: str, own: set[Path]) -> bool:
    for ext in _PSIO_PAIR_EXTS:
        path = parent / f"{stem}{ext}"
        if path.exists() and path not in own:
            return False
    return True


def _free_single_name(parent: Path, desired: str, own: Path) -> Path:
    candidate = parent / desired
    if candidate == own or not candidate.exists():
        return candidate
    stem = Path(desired).stem
    ext = Path(desired).suffix
    n = 2
    while n < 1000:
        suffix = f"~{n}"
        limit = max(1, PSIO_MAX_NAME - len(ext) - len(suffix))
        candidate = parent / f"{stem[:limit].rstrip(' ._')}{suffix}{ext}"
        if candidate == own or not candidate.exists():
            return candidate
        n += 1
    return parent / desired


def _sanitize_dir_files(directory: Path, renames: list[tuple[str, str]]) -> None:
    """Rename ``.bin``/``.cu2`` pairs + loose files in ``directory`` to PSIO-safe
    names, keeping paired stems aligned and rewriting ``MULTIDISC.LST``."""
    pairs: dict[str, dict[str, Path]] = {}
    loose: list[Path] = []
    for entry in sorted(directory.iterdir()):
        if not entry.is_file():
            continue
        ext = entry.suffix.lower()
        if ext in _PSIO_PAIR_EXTS:
            pairs.setdefault(entry.stem, {})[ext] = entry
        elif entry.name.upper() != "MULTIDISC.LST":
            loose.append(entry)

    bin_renames: dict[str, str] = {}

    for stem, files in sorted(pairs.items()):
        desired = psio_safe_name(stem, reserve=4)
        if desired == stem:
            continue
        own = {p for p in files.values()}
        final_stem = _free_pair_stem(directory, desired, own)
        if final_stem == stem:
            continue
        for ext in _PSIO_PAIR_EXTS:
            old = files.get(ext)
            if not old:
                continue
            new_path = directory / f"{final_stem}{ext}"
            old.rename(new_path)
            renames.append((str(old), str(new_path)))
            if ext == ".bin":
                bin_renames[old.name] = new_path.name

    for old in loose:
        ext = old.suffix
        desired = psio_safe_name(old.stem, reserve=len(ext)) + ext
        if desired == old.name:
            continue
        new_path = _free_single_name(old.parent, desired, old)
        if new_path == old:
            continue
        old.rename(new_path)
        renames.append((str(old), str(new_path)))

    lst = directory / "MULTIDISC.LST"
    if bin_renames and lst.is_file():
        # newline="" disables platform translation so the on-disk CRLF
        # endings PSIO expects are preserved exactly.
        with lst.open("r", encoding="utf-8", errors="replace", newline="") as fh:
            lines = [
                bin_renames.get(line.strip(), line.strip())
                for line in fh.read().splitlines()
            ]
        with lst.open("w", encoding="utf-8", newline="") as fh:
            fh.write("\r\n".join(lines) + "\r\n")


def _sanitize_folder_name(folder: Path, renames: list[tuple[str, str]]) -> None:
    desired = psio_safe_name(folder.name, reserve=0)
    if desired == folder.name:
        return
    new_path = _free_single_name(folder.parent, desired, folder)
    if new_path == folder:
        return
    folder.rename(new_path)
    renames.append((str(folder), str(new_path)))


def sanitize_installed_files(profile: dict, system: str) -> list[tuple[str, str]]:
    """Rename PSIO-installed files/folders that break the ASCII + 60-char limits.

    Walks the profile's ROM folder: each game subfolder has its ``.bin``/``.cu2``
    pairs aligned to a matching PSIO-safe stem (with ``MULTIDISC.LST`` rewritten)
    and is itself renamed; loose files under the root are fixed too.  Returns a
    list of ``(old_path, new_path)`` records.  Does nothing if the folder is
    missing.  Never raises on individual rename failures — problems are skipped
    so one locked file doesn't abort the whole sweep.
    """
    renames: list[tuple[str, str]] = []
    try:
        root = resolve_profile_rom_folder(profile, system)
    except Exception:
        return renames
    if not root.is_dir():
        return renames

    try:
        _sanitize_dir_files(root, renames)
    except OSError:
        pass
    for child in sorted(root.iterdir()):
        if not child.is_dir():
            continue
        try:
            _sanitize_dir_files(child, renames)
            _sanitize_folder_name(child, renames)
        except OSError:
            continue
    return renames
