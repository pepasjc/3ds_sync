from __future__ import annotations

import os
import re
import shutil
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
    ("cci", "Xbox CCI"),
    ("folder", "Extracted Folder"),
]
ROM_FORMAT_LABELS = dict(ROM_FORMAT_OPTIONS)

ARCHIVE_EXTRACT_FORMATS = {"cue", "psio", "gdi", "cci", "folder"}
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


def build_install_plan(
    profile: dict,
    rom: dict,
    system: str | None = None,
    override_format: str = "",
) -> InstallPlan:
    system_up = (system or rom.get("system") or "").upper()
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
        target_dir = target_root / safe_folder_name(display_name)
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
