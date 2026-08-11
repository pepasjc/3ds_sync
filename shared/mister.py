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


# Compatibility aliases for current desktop imports.
FOLDER_TO_SYSTEM = MISTER_FOLDER_TO_SYSTEM
SYSTEM_TO_FOLDER = MISTER_SYSTEM_TO_FOLDER
MISTER_FOLDER_MAP = MISTER_FOLDER_TO_SYSTEM

__all__ = [
    "FOLDER_TO_SYSTEM",
    "MISTER_CD_SYSTEMS",
    "MISTER_FOLDER_MAP",
    "MISTER_FOLDER_TO_SYSTEM",
    "MISTER_GAMES_ROOTS",
    "MISTER_SYSTEM_FOLDER_CANDIDATES",
    "MISTER_SYSTEM_TO_FOLDER",
    "SYSTEM_TO_FOLDER",
    "mister_system_folder_candidates",
]
