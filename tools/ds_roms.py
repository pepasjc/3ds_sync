#!/usr/bin/env python3
"""DS ROM Sync - Two-way sync of NDS ROM files between SD card and PC folder.

Identifies ROMs by game code (from NDS header), not filename, to avoid
duplicates. Renames files to a standardized name from the game database.

Usage:
    python ds_roms.py --sd-path E:\\ --local-dir C:\\roms\\nds
    python ds_roms.py --sd-path E:\\ --local-dir C:\\roms\\nds --dry-run
"""

import argparse
import shutil
import sys
from pathlib import Path

NDS_GAMECODE_OFFSET = 0x0C


# --- ROM identification ---

def read_gamecode(rom_path: Path) -> str | None:
    """Read the 4-char game code from an NDS ROM header."""
    try:
        with open(rom_path, "rb") as f:
            f.seek(NDS_GAMECODE_OFFSET)
            code = f.read(4)
            if len(code) == 4 and all(0x20 <= b <= 0x7E for b in code):
                return code.decode("ascii")
    except (OSError, UnicodeDecodeError):
        pass
    return None


def standard_name(code: str, name_db: dict[str, str]) -> str:
    """Generate a standardized ROM filename from game code.

    Format: "Game Name (CODE).nds"
    Falls back to just "(CODE).nds" if name not in database.
    """
    name = name_db.get(code)
    if name:
        # Clean characters not allowed in filenames
        for ch in '<>:"/\\|?*':
            name = name.replace(ch, "")
        return f"{name} ({code}).nds"
    return f"({code}).nds"


def scan_roms(directory: Path) -> dict[str, Path]:
    """Find all .nds files recursively. Returns dict of game_code -> path.

    Deduplicates by game code (first found wins).
    """
    roms = {}
    if not directory.exists():
        return roms
    for f in sorted(directory.rglob("*.nds")):
        code = read_gamecode(f)
        if code and code not in roms:
            roms[code] = f
    return roms


def scan_saves(directory: Path) -> dict[str, Path]:
    """Find all .sav files recursively. Returns dict of stem -> path.

    Deduplicates by stem (first found wins).
    """
    saves = {}
    if not directory.exists():
        return saves
    for f in sorted(directory.rglob("*.sav")):
        if f.stem not in saves:
            saves[f.stem] = f
    return saves


def find_rom_dir(sd_path: Path) -> Path | None:
    """Find the directory on the SD card where ROMs are stored.

    Returns the directory containing the most .nds files.
    """
    candidates = [sd_path, sd_path / "nds", sd_path / "roms",
                  sd_path / "roms" / "nds"]

    best_dir = None
    best_count = 0

    for d in candidates:
        if d.is_dir():
            count = sum(1 for _ in d.glob("*.nds"))
            if count > best_count:
                best_count = count
                best_dir = d

    if best_count == 0:
        for f in sd_path.rglob("*.nds"):
            return f.parent

    return best_dir


# --- Game name database ---

def load_name_database(db_path: Path) -> dict[str, str]:
    """Load game names from dstdb.txt. Returns dict of code -> name."""
    names = {}
    if not db_path.exists():
        return names
    with open(db_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or "," not in line:
                continue
            parts = line.split(",", 1)
            if len(parts) == 2:
                code = parts[0].strip().upper()
                name = parts[1].strip()
                if code and name:
                    names[code] = name
    return names


def load_scene_database(db_path: Path) -> dict[str, str]:
    """Load scene release names from ds_releases.txt.

    Returns dict of archive_name -> game_code.
    Maps scene filenames (e.g. "xpa-bbme") to 4-char game codes ("BBME").
    """
    mapping = {}
    if not db_path.exists():
        return mapping
    with open(db_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or "," not in line:
                continue
            parts = line.split(",", 1)
            if len(parts) == 2:
                archive = parts[0].strip().lower()
                code = parts[1].strip().upper()
                if archive and code:
                    mapping[archive] = code
    return mapping


# --- Helpers ---

def format_size(size: int) -> str:
    if size >= 1024 * 1024 * 1024:
        return f"{size / (1024 * 1024 * 1024):.1f} GB"
    elif size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.1f} MB"
    elif size >= 1024:
        return f"{size / 1024:.1f} KB"
    return f"{size} B"


def rename_rom(rom_path: Path, new_name: str, dry_run: bool,
               known_sav: Path | None = None) -> Path:
    """Rename a ROM file and its matching .sav file.

    If known_sav is provided, use it directly instead of searching by ROM stem.
    This handles the case where the ROM was already renamed but the save wasn't.
    Returns the new ROM path.
    """
    new_sav_name = Path(new_name).with_suffix(".sav").name

    # Find save file to rename
    if known_sav and known_sav.exists():
        sav_to_rename = known_sav
    else:
        sav_to_rename = None
        sav_candidates = [
            rom_path.with_suffix(".sav"),                              # same directory
            rom_path.parent / "saves" / f"{rom_path.stem}.sav",       # saves/ subfolder
        ]
        for candidate in sav_candidates:
            if candidate.exists():
                sav_to_rename = candidate
                break

    # Rename save if found and needed
    if sav_to_rename:
        # Skip if a correctly-named save already exists anywhere expected
        correct_exists = (
            (rom_path.parent / new_sav_name).exists() or
            (rom_path.parent / "saves" / new_sav_name).exists()
        )
        if sav_to_rename.name != new_sav_name and not correct_exists:
            new_sav = sav_to_rename.parent / new_sav_name
            print(f"    Rename save: {sav_to_rename.name} -> {new_sav_name}")
            if not dry_run:
                sav_to_rename.rename(new_sav)

    # Rename ROM if needed
    if rom_path.name == new_name:
        return rom_path

    new_path = rom_path.parent / new_name
    if new_path.exists() and new_path != rom_path:
        return rom_path

    print(f"    Rename: {rom_path.name} -> {new_name}")
    if not dry_run:
        rom_path.rename(new_path)
        return new_path

    return rom_path


# --- Main ---

def main():
    parser = argparse.ArgumentParser(
        description="Two-way sync of NDS ROM files between SD card and PC folder.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python ds_roms.py --sd-path E:\\ --local-dir C:\\roms\\nds
  python ds_roms.py --sd-path E:\\ --local-dir C:\\roms\\nds --dry-run
  python ds_roms.py --sd-path E:\\ --local-dir C:\\roms\\nds --no-rename
        """,
    )
    parser.add_argument("--sd-path", type=Path, required=True,
                        help="Path to SD card root or ROM directory on SD")
    parser.add_argument("--local-dir", type=Path, required=True,
                        help="Local PC folder to sync ROMs with")
    parser.add_argument("--no-rename", action="store_true",
                        help="Don't rename files to standardized names")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be done without making changes")
    parser.add_argument("--clean-orphans", action="store_true",
                        help="Delete orphan save files (saves with no matching ROM)")

    args = parser.parse_args()

    if not args.sd_path.exists():
        print(f"Error: SD card path does not exist: {args.sd_path}")
        sys.exit(1)

    # Find where ROMs are on the SD card
    sd_rom_dir = find_rom_dir(args.sd_path)
    if not sd_rom_dir:
        print(f"No .nds ROM files found on SD card at: {args.sd_path}")
        sys.exit(1)

    # Create local dir if needed
    if not args.dry_run:
        args.local_dir.mkdir(parents=True, exist_ok=True)

    # Load game name database
    db_path = Path(__file__).parent.parent / "server" / "data" / "dstdb.txt"
    name_db = load_name_database(db_path)

    # Load scene release database (archive_name -> game_code)
    scene_db_path = Path(__file__).parent / "ds_releases.txt"
    scene_db = load_scene_database(scene_db_path)

    print("DS ROM Sync")
    print(f"SD card ROMs: {sd_rom_dir}")
    print(f"Local folder: {args.local_dir}")
    print(f"Game database: {len(name_db)} entries")
    if scene_db:
        print(f"Scene database: {len(scene_db)} entries")
    if args.dry_run:
        print("(dry run)")
    print()

    # Scan both sides by game code
    sd_roms = scan_roms(sd_rom_dir)
    local_roms = scan_roms(args.local_dir)

    # Filter out ROMs not in the database (boot ROMs, utilities like GodMode9, etc.)
    sd_skipped = {c: p for c, p in sd_roms.items() if c not in name_db}
    local_skipped = {c: p for c, p in local_roms.items() if c not in name_db}
    sd_roms = {c: p for c, p in sd_roms.items() if c in name_db}
    local_roms = {c: p for c, p in local_roms.items() if c in name_db}

    if sd_skipped or local_skipped:
        total_skipped = len(sd_skipped) + len(local_skipped)
        print(f"Skipped {total_skipped} non-game ROM(s) not in database:")
        for code, path in {**sd_skipped, **local_skipped}.items():
            print(f"  {path.name} ({code})")
        print()

    sd_codes = set(sd_roms.keys())
    local_codes = set(local_roms.keys())

    to_download = sd_codes - local_codes   # SD -> PC
    to_upload = local_codes - sd_codes     # PC -> SD
    in_common = sd_codes & local_codes

    print(f"SD card:    {len(sd_roms)} unique ROM(s)")
    print(f"Local:      {len(local_roms)} unique ROM(s)")
    print(f"In common:  {len(in_common)} (by game code)")
    print(f"SD -> PC:   {len(to_download)}")
    print(f"PC -> SD:   {len(to_upload)}")

    do_rename = not args.no_rename

    # Step 1: Rename existing files to standard names
    if do_rename:
        renamed = 0
        for roms_dict, rom_dir in [(sd_roms, sd_rom_dir),
                                    (local_roms, args.local_dir)]:
            # Scan all saves in this directory tree
            all_saves = scan_saves(rom_dir)

            # Build set of ROM stems for orphan detection
            rom_stems = {p.stem for p in roms_dict.values()}

            # Orphan saves: .sav files whose stem doesn't match any ROM
            orphan_by_dir: dict[Path, list[tuple[str, Path]]] = {}
            for stem, sav_path in all_saves.items():
                if stem not in rom_stems:
                    d = sav_path.parent
                    orphan_by_dir.setdefault(d, []).append((stem, sav_path))

            for code, path in list(roms_dict.items()):
                std = standard_name(code, name_db)
                std_stem = Path(std).stem
                needs_rom_rename = (path.name != std)

                # Check if the correctly-named save already exists
                known_sav = None
                if not needs_rom_rename:
                    has_correct_sav = (
                        path.with_suffix(".sav").exists() or
                        (path.parent / "saves" / f"{std_stem}.sav").exists()
                    )
                    if not has_correct_sav:
                        # ROM already has standard name but save is missing.
                        # Try matching orphan saves using three strategies:
                        # 0) Scene DB lookup (e.g. "cat-tbgp" -> code TBGP)
                        # 1) Game name substring (e.g. "New Super Mario Bros." in
                        #    "0018 - New Super Mario Bros. (USA).sav")
                        # 2) Game code substring (e.g. "BBME" in "XPA-BBME.sav")
                        game_name = name_db.get(code, "").lower()
                        code_upper = code.upper()
                        for search_dir in [path.parent, path.parent / "saves"]:
                            orphans = orphan_by_dir.get(search_dir, [])
                            # Pass 0: scene database exact match
                            if scene_db:
                                for idx, (stem, sav_path) in enumerate(orphans):
                                    if scene_db.get(stem.lower()) == code_upper:
                                        known_sav = sav_path
                                        orphans.pop(idx)
                                        break
                            # Pass 1: game name match
                            if not known_sav and game_name:
                                for idx, (stem, sav_path) in enumerate(orphans):
                                    if game_name in stem.lower():
                                        known_sav = sav_path
                                        orphans.pop(idx)
                                        break
                            # Pass 2: game code match (handles scene names)
                            if not known_sav:
                                for idx, (stem, sav_path) in enumerate(orphans):
                                    if code_upper in stem.upper():
                                        known_sav = sav_path
                                        orphans.pop(idx)
                                        break
                            if known_sav:
                                break

                if needs_rom_rename or known_sav:
                    roms_dict[code] = rename_rom(path, std, args.dry_run, known_sav)
                    renamed += 1

        if renamed:
            print(f"\nRenamed {renamed} file(s) to standard names")

    # Step 2: Copy SD -> PC
    if to_download:
        total_size = sum(sd_roms[c].stat().st_size for c in to_download)
        print(f"\nCopying {len(to_download)} ROM(s) from SD to PC ({format_size(total_size)}):")
        for code in sorted(to_download):
            src = sd_roms[code]
            dst_name = standard_name(code, name_db) if do_rename else src.name
            dst = args.local_dir / dst_name
            size = format_size(src.stat().st_size)
            db_name = name_db.get(code, "?")
            print(f"  {db_name:<40s} {code}  {size:>8s}")
            if not args.dry_run:
                shutil.copy2(src, dst)

    # Step 3: Copy PC -> SD
    if to_upload:
        total_size = sum(local_roms[c].stat().st_size for c in to_upload)
        print(f"\nCopying {len(to_upload)} ROM(s) from PC to SD ({format_size(total_size)}):")
        for code in sorted(to_upload):
            src = local_roms[code]
            dst_name = standard_name(code, name_db) if do_rename else src.name
            dst = sd_rom_dir / dst_name
            size = format_size(src.stat().st_size)
            db_name = name_db.get(code, "?")
            print(f"  {db_name:<40s} {code}  {size:>8s}")
            if not args.dry_run:
                shutil.copy2(src, dst)

    # Step 4: Copy save files from SD to local folder
    # For each ROM in common (or just copied), check if SD has a save but local doesn't
    saves_copied = 0
    missing_saves = []
    all_codes = sd_codes | local_codes
    for code in sorted(all_codes):
        std = standard_name(code, name_db) if do_rename else None
        sav_name = Path(std).with_suffix(".sav").name if std else f"({code}).sav"

        # Check if local already has this save
        local_sav = args.local_dir / sav_name
        local_saves_dir = args.local_dir / "saves" / sav_name
        if local_sav.exists() or local_saves_dir.exists():
            continue

        # Check if SD has this save (by ROM path, or by standard name in rom dir)
        src_sav = None
        sd_path = sd_roms.get(code)
        if sd_path:
            # ROM exists on SD - check next to it and in saves/ subdir
            for candidate in [sd_path.with_suffix(".sav"),
                              sd_path.parent / "saves" / f"{sd_path.stem}.sav"]:
                if candidate.exists():
                    src_sav = candidate
                    break
        if not src_sav:
            # ROM might not be on SD (PC-only), but save could still be there
            # Check by standard save name in SD rom dir
            for candidate in [sd_rom_dir / sav_name,
                              sd_rom_dir / "saves" / sav_name]:
                if candidate.exists():
                    src_sav = candidate
                    break

        if src_sav:
            db_name = name_db.get(code, "?")
            size = format_size(src_sav.stat().st_size)
            if saves_copied == 0:
                print(f"\nCopying save files from SD to local:")
            print(f"  {db_name:<40s} {code}  {size:>8s}")
            if not args.dry_run:
                shutil.copy2(src_sav, local_sav)
            saves_copied += 1
        else:
            missing_saves.append(code)

    if missing_saves:
        print(f"\nNo save file found for {len(missing_saves)} game(s):")
        for code in missing_saves:
            db_name = name_db.get(code, "?")
            print(f"  {db_name:<40s} {code}")

    # Step 5: Report orphan save files (saves with no matching ROM)
    # Build set of all known ROM stems (SD + standard names for all known codes)
    known_stems = {p.stem for p in sd_roms.values()}
    if do_rename:
        for code in all_codes:
            known_stems.add(Path(standard_name(code, name_db)).stem)
    orphan_savs = []
    all_sd_saves = scan_saves(sd_rom_dir)
    for stem, sav_path in sorted(all_sd_saves.items()):
        if stem not in known_stems:
            orphan_savs.append(sav_path)
    if orphan_savs:
        if args.clean_orphans:
            total_size = sum(s.stat().st_size for s in orphan_savs)
            print(f"\nDeleting {len(orphan_savs)} orphan save file(s) ({format_size(total_size)}):")
            for sav in orphan_savs:
                size = format_size(sav.stat().st_size)
                print(f"  {sav.name:<50s} {size:>8s}")
                if not args.dry_run:
                    sav.unlink()
        else:
            print(f"\nOrphan save files ({len(orphan_savs)} with no matching ROM):")
            for sav in orphan_savs:
                size = format_size(sav.stat().st_size)
                print(f"  {sav.name:<50s} {size:>8s}")
            print("  (use --clean-orphans to delete these)")

    if not to_download and not to_upload and not (do_rename and renamed) and not saves_copied:
        print("\nAlready in sync!")
    elif not args.dry_run:
        print("\nDone.")
    else:
        print("\n(dry run - no changes made)")


if __name__ == "__main__":
    main()
