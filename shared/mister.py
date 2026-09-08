"""Shared MiSTer folder mappings.

These mappings are used by both the desktop MiSTer integration and the
standalone MiSTer shell script via generated JSON.
"""

from __future__ import annotations

# Storage roots a MiSTer scans for game folders.  Cores search /media/usb0..5
# before /media/fat, so a game folder existing on USB takes precedence.
MISTER_GAMES_ROOTS: dict[str, str] = {
    "sd": "/media/fat/games",
    "usb": "/media/usb0/games",
}

MISTER_FOLDER_TO_SYSTEM: dict[str, str] = {
    "GBA": "GBA",
    "SNES": "SNES",
    "NES": "NES",
    "Genesis": "MD",
    "MegaDrive": "MD",
    "N64": "N64",
    "Gameboy": "GB",
    "GAMEBOY": "GB",
    "GBC": "GBC",
    "GameGear": "GG",
    "SMS": "SMS",
    "PCEngine": "PCE",
    "TurboGrafx16": "PCE",
    "TGFX16": "PCE",
    "TGFX16-CD": "PCECD",
    "Atari2600": "A2600",
    "Atari7800": "A7800",
    "ATARI7800": "A7800",
    "Lynx": "LYNX",
    "AtariLynx": "LYNX",
    "NeoGeo": "NEOGEO",
    "NeoGeo-CD": "NEOCD",
    "NeoGeoPocket": "NGP",
    "NeoGeoPocket-Color": "NGPC",
    "32X": "32X",
    "S32X": "32X",
    "MegaCD": "SEGACD",
    "PSX": "PS1",
    "WonderSwan": "WSWAN",
    "WonderSwanColor": "WSWANC",
    "3DO": "3DO",
    # Some MiSTer builds use slightly different names.
    "GG": "GG",
    "NEOGEO": "NEOGEO",
    "Lynx48": "LYNX",
    "Saturn": "SAT",
}

MISTER_SYSTEM_TO_FOLDER: dict[str, str] = {
    "GBA": "GBA",
    "SNES": "SNES",
    "NES": "NES",
    "MD": "Genesis",
    "N64": "N64",
    "GB": "Gameboy",
    "GBC": "GBC",
    "GG": "GameGear",
    "SMS": "SMS",
    "PCE": "PCEngine",
    "A2600": "Atari2600",
    "A7800": "Atari7800",
    "LYNX": "Lynx",
    "NEOGEO": "NeoGeo",
    "32X": "32X",
    "SEGACD": "MegaCD",
    "PS1": "PSX",
    "SAT": "Saturn",
}

# Ordered game-folder candidates per system for ROM installs.  Core folder
# names drifted over MiSTer releases (Genesis -> MegaDrive, PCEngine ->
# TGFX16, 32X -> S32X, ...): prefer the name current cores create, but honor
# a legacy folder if it already exists on the target.
MISTER_SYSTEM_FOLDER_CANDIDATES: dict[str, list[str]] = {
    "GBA": ["GBA"],
    "SNES": ["SNES"],
    "NES": ["NES"],
    "MD": ["MegaDrive", "Genesis"],
    "N64": ["N64"],
    "GB": ["GAMEBOY", "Gameboy"],
    "GBC": ["GBC"],
    "GG": ["GameGear"],
    "SMS": ["SMS"],
    "PCE": ["TGFX16", "PCEngine", "TurboGrafx16"],
    "PCECD": ["TGFX16-CD"],
    "A2600": ["Atari2600"],
    "A7800": ["ATARI7800", "Atari7800"],
    "LYNX": ["AtariLynx", "Lynx"],
    "NEOGEO": ["NEOGEO", "NeoGeo"],
    "NEOCD": ["NeoGeo-CD"],
    "NGP": ["NeoGeoPocket"],
    "NGPC": ["NeoGeoPocket-Color"],
    "32X": ["S32X", "32X"],
    "SEGACD": ["MegaCD"],
    "PS1": ["PSX"],
    "SAT": ["Saturn"],
    "WSWAN": ["WonderSwan"],
    "WSWANC": ["WonderSwanColor"],
    "3DO": ["3DO"],
}


# Some cores keep their saves somewhere other than their own games folder.
#
# The TurboGrafx-16 core writes **both** HuCard and CD saves into
# ``saves/TGFX16``; there is no ``saves/TGFX16-CD``, even though CD *games*
# live in ``games/TGFX16-CD``. Writing a downloaded CD save to a folder named
# after the games folder puts it somewhere the core never looks.
MISTER_SYSTEM_SAVE_FOLDERS: dict[str, list[str]] = {
    "PCECD": ["TGFX16"],
}

# The other half of that: one save folder now serves two systems, so the
# folder alone no longer says which system a save belongs to. The games folders
# are still separate, so an installed game is what disambiguates.
MISTER_SHARED_SAVE_FOLDERS: dict[str, tuple] = {
    "TGFX16": ("PCECD", "PCE"),
}


def mister_system_save_folder_candidates(system: str) -> list[str]:
    """Ordered save-folder names for a system (best first).

    Defaults to the games-folder names, since most cores use the same name for
    both, and is overridden where a core does not.
    """
    system_up = (system or "").upper()
    override = MISTER_SYSTEM_SAVE_FOLDERS.get(system_up)
    if override:
        return list(override)
    return mister_system_folder_candidates(system_up)


# CD-based cores.  Their games install into a per-game subfolder
# (games/PSX/<Game>/<Game>.chd): the cores name the autosave memory card /
# backup RAM after the game's folder, so each game gets a dedicated save and
# multi-disc games (disc tag stripped from the folder name) share one card.
MISTER_CD_SYSTEMS: frozenset[str] = frozenset(
    {"PS1", "SAT", "SEGACD", "PCECD", "NEOCD", "3DO"}
)


def mister_system_folder_candidates(system: str) -> list[str]:
    """Ordered MiSTer game-folder names for a system code (best first)."""
    system_up = (system or "").upper()
    candidates = MISTER_SYSTEM_FOLDER_CANDIDATES.get(system_up)
    if candidates:
        return list(candidates)
    fallback = MISTER_SYSTEM_TO_FOLDER.get(system_up)
    return [fallback] if fallback else []


# ── Where GameSync keeps its files on the device ────────────────────────────
#
# MiSTer scripts keep their data in ``/media/fat/Scripts/.config/<script>/``;
# the stock ``downloader`` puts its config, state and log there together, so
# GameSync follows suit rather than littering the SD card root.
#
# These constants are shared because three different programs read and write
# the same state file - the on-device client, the desktop client over SFTP and
# the legacy ``sync_saves.sh``. If they ever disagreed on the path, each would
# keep its own idea of the last synced hash and every save would look like a
# conflict.

#: MiSTer's Saturn core reads and writes the internal backup RAM byte-expanded
#: to 64 KB (0xFF padding at even offsets) - the same layout Yabause uses - and
#: always names it ``.sav``. The server keeps the canonical 32 KB image, so a
#: download has to be converted into this shape or the core ignores it.
MISTER_SATURN_FORMAT = "yabause"

MISTER_CONFIG_DIR = "/media/fat/Scripts/.config/gamesync"
MISTER_CONFIG_FILE = MISTER_CONFIG_DIR + "/gamesync.cfg"
MISTER_STATE_FILE = MISTER_CONFIG_DIR + "/state.json"
MISTER_LOG_FILE = MISTER_CONFIG_DIR + "/gamesync.log"

#: Pre-0.5.4 locations. Still read when the current path holds nothing, so an
#: existing install keeps its sync state instead of re-conflicting everything.
LEGACY_MISTER_CONFIG_FILE = "/media/fat/3dssync.cfg"
LEGACY_MISTER_STATE_FILE = "/media/fat/3dssync_state.json"


# Compatibility aliases for current desktop imports.
FOLDER_TO_SYSTEM = MISTER_FOLDER_TO_SYSTEM
SYSTEM_TO_FOLDER = MISTER_SYSTEM_TO_FOLDER
MISTER_FOLDER_MAP = MISTER_FOLDER_TO_SYSTEM

__all__ = [
    "FOLDER_TO_SYSTEM",
    "LEGACY_MISTER_CONFIG_FILE",
    "LEGACY_MISTER_STATE_FILE",
    "MISTER_CD_SYSTEMS",
    "MISTER_CONFIG_DIR",
    "MISTER_CONFIG_FILE",
    "MISTER_FOLDER_MAP",
    "MISTER_FOLDER_TO_SYSTEM",
    "MISTER_GAMES_ROOTS",
    "MISTER_LOG_FILE",
    "MISTER_SATURN_FORMAT",
    "MISTER_STATE_FILE",
    "MISTER_SHARED_SAVE_FOLDERS",
    "MISTER_SYSTEM_FOLDER_CANDIDATES",
    "MISTER_SYSTEM_SAVE_FOLDERS",
    "MISTER_SYSTEM_TO_FOLDER",
    "SYSTEM_TO_FOLDER",
    "mister_system_folder_candidates",
    "mister_system_save_folder_candidates",
]
