#!/usr/bin/env python3
"""DS Save Sync - Sync Nintendo DS .sav files with the 3DS Save Sync server.

Supports saves from TWiLight Menu++ (DSi/3DS), flashcards (R4, DSTT), etc.
Uses the same server API and bundle format as the 3DS client.

Usage:
    python ds_sync.py --sd-path E:\\ --server http://192.168.1.201:8000 --api-key mykey
    python ds_sync.py --roms-dir E:\\nds --saves-dir E:\\nds\\saves --server ... --api-key ...
    python ds_sync.py --sd-path E:\\ --server ... --api-key ... --dry-run
"""

import argparse
import hashlib
import json
import random
import struct
import sys
import time
import zlib
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError

# --- Constants ---

TITLE_ID_PREFIX = 0x00048000  # Prefix for DS game title IDs
NDS_GAMECODE_OFFSET = 0x0C    # Offset of 4-char game code in NDS ROM header
BUNDLE_MAGIC = b"3DSS"
BUNDLE_VERSION_COMPRESSED = 2
SYNC_DIR_NAME = ".ds_sync"    # Hidden folder on SD card for sync data


# --- Title ID generation ---

def gamecode_to_title_id(code: str) -> str:
    """Convert a 4-char NDS game code to a 16-char hex title ID.

    Format: 00048000 + ASCII hex of game code
    Example: "A2DE" -> "0004800041324445"
    """
    hex_suffix = code.encode("ascii").hex().upper()
    return f"00048000{hex_suffix}"


def title_id_to_gamecode(title_id: str) -> str:
    """Extract the 4-char game code from a DS title ID."""
    hex_suffix = title_id[8:]
    return bytes.fromhex(hex_suffix).decode("ascii")


# --- NDS ROM scanning ---

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


def find_sav_for_rom(rom_path: Path, saves_dir: Path | None = None) -> Path | None:
    """Find the .sav file matching an NDS ROM.

    Search order:
    1. Same directory as ROM (flashcard style)
    2. saves/ subfolder relative to ROM's parent
    3. Explicit saves_dir if provided
    """
    base = rom_path.stem
    candidates = []

    # Same directory
    candidates.append(rom_path.with_suffix(".sav"))

    # saves/ subfolder next to ROM
    candidates.append(rom_path.parent / "saves" / f"{base}.sav")

    # Explicit saves directory
    if saves_dir:
        candidates.append(saves_dir / f"{base}.sav")

    for path in candidates:
        if path.exists():
            return path

    return None


def scan_roms(scan_path: Path, saves_dir: Path | None = None) -> list[dict]:
    """Scan for NDS ROMs and their matching .sav files.

    Returns list of dicts with: rom_path, sav_path, gamecode, title_id, name, has_save
    ROMs without saves are included (has_save=False) so server-only saves can be downloaded.
    """
    found = []
    seen_codes = set()

    # Find all .nds files recursively
    for rom_path in sorted(scan_path.rglob("*.nds")):
        code = read_gamecode(rom_path)
        if not code:
            continue

        # Skip duplicates (same game code from different regions/copies)
        if code in seen_codes:
            continue

        sav_path = find_sav_for_rom(rom_path, saves_dir)
        has_save = sav_path is not None

        # Default sav_path for ROMs without saves (where to write downloaded saves)
        if not sav_path:
            # Prefer saves/ subfolder if it exists, otherwise same dir as ROM
            saves_subdir = rom_path.parent / "saves"
            if saves_subdir.is_dir():
                sav_path = saves_subdir / f"{rom_path.stem}.sav"
            else:
                sav_path = rom_path.with_suffix(".sav")

        seen_codes.add(code)
        found.append({
            "rom_path": rom_path,
            "sav_path": sav_path,
            "gamecode": code,
            "title_id": gamecode_to_title_id(code),
            "name": rom_path.stem,  # Will be updated by server lookup
            "has_save": has_save,
        })

    return found


# --- Game name lookup ---

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


# --- Hash computation ---

def hash_save(sav_path: Path) -> str:
    """Compute SHA-256 hash of a save file (matches server/3DS client format)."""
    data = sav_path.read_bytes()
    return hashlib.sha256(data).hexdigest()


# --- Bundle format ---

def create_bundle(title_id_int: int, sav_data: bytes) -> bytes:
    """Create a 3DSS V2 (compressed) bundle from save data."""
    timestamp = int(time.time())
    file_path = b"save.dat"
    file_hash = hashlib.sha256(sav_data).digest()

    # Build payload: file table + file data
    payload_parts = []
    # File table entry
    payload_parts.append(struct.pack("<H", len(file_path)))
    payload_parts.append(file_path)
    payload_parts.append(struct.pack("<I", len(sav_data)))
    payload_parts.append(file_hash)
    # File data
    payload_parts.append(sav_data)
    payload = b"".join(payload_parts)

    # Compress
    compressed = zlib.compress(payload, level=6)

    # Build bundle
    header = []
    header.append(BUNDLE_MAGIC)
    header.append(struct.pack("<I", BUNDLE_VERSION_COMPRESSED))
    header.append(struct.pack(">Q", title_id_int))
    header.append(struct.pack("<I", timestamp))
    header.append(struct.pack("<I", 1))  # file_count
    header.append(struct.pack("<I", len(payload)))  # uncompressed size
    header.append(compressed)

    return b"".join(header)


def parse_bundle(data: bytes) -> bytes:
    """Parse a 3DSS bundle and return the save file data."""
    if len(data) < 28:
        raise ValueError("Bundle too small")

    magic = data[0:4]
    if magic != BUNDLE_MAGIC:
        raise ValueError(f"Invalid magic: {magic!r}")

    version = struct.unpack_from("<I", data, 4)[0]
    file_count = struct.unpack_from("<I", data, 20)[0]
    size_field = struct.unpack_from("<I", data, 24)[0]

    if version == BUNDLE_VERSION_COMPRESSED:
        payload = zlib.decompress(data[28:])
        if len(payload) != size_field:
            raise ValueError("Decompressed size mismatch")
    elif version == 1:
        payload = data[28:]
    else:
        raise ValueError(f"Unsupported version: {version}")

    # Parse file table to get sizes, then extract data
    offset = 0
    files_info = []
    for _ in range(file_count):
        path_len = struct.unpack_from("<H", payload, offset)[0]
        offset += 2
        offset += path_len  # skip path
        file_size = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        offset += 32  # skip hash
        files_info.append(file_size)

    # For DS saves we expect 1 file, but concatenate all just in case
    all_data = []
    for size in files_info:
        all_data.append(payload[offset:offset + size])
        offset += size

    return b"".join(all_data)


# --- SD card identity & sync state ---

def get_sync_dir(sd_root: Path) -> Path:
    """Get (and create if needed) the .ds_sync directory on the SD card."""
    sync_dir = sd_root / SYNC_DIR_NAME
    sync_dir.mkdir(exist_ok=True)
    return sync_dir


def get_console_id(sync_dir: Path) -> str:
    """Get or generate a unique console ID for this SD card.

    Stored in .ds_sync/console_id on the SD card.
    Auto-generated on first run so each SD card gets a unique identity.
    """
    id_file = sync_dir / "console_id"
    if id_file.exists():
        cid = id_file.read_text().strip()
        if cid:
            return cid

    # Generate a new unique ID
    rand_hex = "%08x" % random.getrandbits(32)
    cid = f"ds_{rand_hex}"
    id_file.write_text(cid)
    print(f"Generated new console ID for this SD card: {cid}")
    return cid


def load_state(sync_dir: Path) -> dict:
    """Load sync state (last_synced_hash per title_id) from SD card."""
    state_file = sync_dir / "state.json"
    if state_file.exists():
        return json.loads(state_file.read_text())
    return {}


def save_state(sync_dir: Path, state: dict):
    """Save sync state to SD card."""
    state_file = sync_dir / "state.json"
    state_file.write_text(json.dumps(state, indent=2))


# --- HTTP client ---

def api_request(server: str, path: str, api_key: str,
                method: str = "GET", data: bytes | None = None,
                content_type: str = "application/octet-stream") -> tuple[int, bytes]:
    """Make an HTTP request to the server API.

    Returns (status_code, response_body).
    """
    url = f"{server}/api/v1{path}"
    headers = {"X-API-Key": api_key}

    if data is not None:
        headers["Content-Type"] = content_type
        req = Request(url, data=data, headers=headers, method=method)
    else:
        req = Request(url, headers=headers, method=method)

    try:
        with urlopen(req, timeout=30) as resp:
            return resp.status, resp.read()
    except HTTPError as e:
        return e.code, e.read()
    except URLError as e:
        print(f"  Connection error: {e.reason}")
        return 0, b""


def api_get(server: str, path: str, api_key: str) -> tuple[int, bytes]:
    return api_request(server, path, api_key, method="GET")


def api_post_bundle(server: str, path: str, api_key: str, bundle: bytes) -> tuple[int, bytes]:
    return api_request(server, path, api_key, method="POST", data=bundle)


def api_post_json(server: str, path: str, api_key: str, payload: dict) -> tuple[int, bytes]:
    data = json.dumps(payload).encode("utf-8")
    return api_request(server, path, api_key, method="POST", data=data,
                       content_type="application/json")


# --- Sync logic ---

def do_sync(games: list[dict], server: str, api_key: str, console_id: str,
            state: dict, dry_run: bool = False) -> dict:
    """Run the sync protocol against the server.

    Returns updated state dict.
    """
    if not games:
        print("No games found.")
        return state

    # Step 1: Build sync request with metadata for all titles
    print(f"\nPreparing sync for {len(games)} title(s)...")
    titles_meta = []
    for g in games:
        if g["has_save"]:
            save_hash = hash_save(g["sav_path"])
            g["save_hash"] = save_hash
            g["save_size"] = g["sav_path"].stat().st_size
        else:
            g["save_hash"] = ""
            g["save_size"] = 0

        meta = {
            "title_id": g["title_id"],
            "save_hash": g["save_hash"],
            "timestamp": int(time.time()),
            "size": g["save_size"],
        }

        # Include last synced hash for three-way comparison
        last_hash = state.get(g["title_id"])
        if last_hash:
            meta["last_synced_hash"] = last_hash

        titles_meta.append(meta)

    sync_request = {"console_id": console_id, "titles": titles_meta}

    # Step 2: Send sync request
    print("Sending sync request to server...")
    status, resp = api_post_json(server, "/sync", api_key, sync_request)
    if status != 200:
        print(f"Sync request failed (HTTP {status})")
        if resp:
            print(f"  {resp.decode('utf-8', errors='replace')[:200]}")
        return state

    plan = json.loads(resp)
    upload_ids = set(plan.get("upload", []))
    download_ids = set(plan.get("download", []))
    conflict_ids = set(plan.get("conflict", []))
    up_to_date_ids = set(plan.get("up_to_date", []))
    server_only_ids = set(plan.get("server_only", []))

    # Build lookup
    games_by_id = {g["title_id"]: g for g in games}

    # Auto-resolve conflicts for games without local saves -> download
    # (no local save means nothing to lose, safe to download from server)
    no_save_conflicts = set()
    for tid in list(conflict_ids):
        g = games_by_id.get(tid)
        if g and not g["has_save"]:
            no_save_conflicts.add(tid)
            conflict_ids.discard(tid)
            download_ids.add(tid)

    # Also move upload requests for games without saves to skip
    # (server has no save, we have no save -> nothing to do)
    for tid in list(upload_ids):
        g = games_by_id.get(tid)
        if g and not g["has_save"]:
            upload_ids.discard(tid)

    # Summary
    print(f"\nSync plan:")
    print(f"  Upload:     {len(upload_ids)}")
    print(f"  Download:   {len(download_ids)}")
    print(f"  Up to date: {len(up_to_date_ids)}")
    print(f"  Conflicts:  {len(conflict_ids)}")
    print(f"  Server only:{len(server_only_ids)}")
    if no_save_conflicts:
        print(f"  (auto-downloading {len(no_save_conflicts)} conflict(s) with no local save)")

    if dry_run:
        if upload_ids:
            print("\nWould upload:")
            for tid in upload_ids:
                g = games_by_id.get(tid)
                name = g["name"] if g else tid
                print(f"  {name} ({tid})")
        if download_ids:
            print("\nWould download:")
            for tid in download_ids:
                g = games_by_id.get(tid)
                name = g["name"] if g else tid
                print(f"  {name} ({tid})")
        if conflict_ids:
            print("\nConflicts (would need resolution):")
            for tid in conflict_ids:
                g = games_by_id.get(tid)
                name = g["name"] if g else tid
                print(f"  {name} ({tid})")
        return state

    # Step 3: Process uploads
    for tid in upload_ids:
        g = games_by_id.get(tid)
        if not g or not g["has_save"]:
            continue
        print(f"  Uploading: {g['name']}...")
        sav_data = g["sav_path"].read_bytes()
        title_id_int = int(tid, 16)
        bundle = create_bundle(title_id_int, sav_data)
        s, r = api_post_bundle(server, f"/saves/{tid}?force=true&source=ds_sync", api_key, bundle)
        if s == 200:
            state[tid] = g["save_hash"]
            print(f"    OK")
        else:
            print(f"    Failed (HTTP {s})")

    # Step 4: Process downloads
    for tid in list(download_ids) + list(server_only_ids):
        g = games_by_id.get(tid)
        if not g:
            continue
        print(f"  Downloading: {g['name']}...")
        s, r = api_get(server, f"/saves/{tid}", api_key)
        if s == 200:
            try:
                sav_data = parse_bundle(r)
                g["sav_path"].write_bytes(sav_data)
                new_hash = hashlib.sha256(sav_data).hexdigest()
                state[tid] = new_hash
                print(f"    OK ({len(sav_data)} bytes)")
            except Exception as e:
                print(f"    Bundle parse error: {e}")
        else:
            print(f"    Failed (HTTP {s})")

    # Step 5: Handle conflicts
    for tid in conflict_ids:
        g = games_by_id.get(tid)
        if not g:
            continue
        print(f"\n  CONFLICT: {g['name']} ({tid})")
        print(f"    Local hash:  {g['save_hash'][:16]}...")
        print(f"    Choose action:")
        print(f"      [u] Upload local save to server")
        print(f"      [d] Download server save (overwrites local)")
        print(f"      [s] Skip")
        choice = input(f"    > ").strip().lower()

        if choice == "u":
            print(f"    Uploading...")
            sav_data = g["sav_path"].read_bytes()
            title_id_int = int(tid, 16)
            bundle = create_bundle(title_id_int, sav_data)
            s, r = api_post_bundle(server, f"/saves/{tid}?force=true&source=ds_sync", api_key, bundle)
            if s == 200:
                state[tid] = g["save_hash"]
                print(f"    OK")
            else:
                print(f"    Failed (HTTP {s})")
        elif choice == "d":
            print(f"    Downloading...")
            s, r = api_get(server, f"/saves/{tid}", api_key)
            if s == 200:
                try:
                    sav_data = parse_bundle(r)
                    g["sav_path"].write_bytes(sav_data)
                    new_hash = hashlib.sha256(sav_data).hexdigest()
                    state[tid] = new_hash
                    print(f"    OK ({len(sav_data)} bytes)")
                except Exception as e:
                    print(f"    Bundle parse error: {e}")
            else:
                print(f"    Failed (HTTP {s})")
        else:
            print(f"    Skipped")

    # Mark up-to-date titles in state
    for tid in up_to_date_ids:
        g = games_by_id.get(tid)
        if g and "save_hash" in g:
            state[tid] = g["save_hash"]

    return state


# --- Main ---

def main():
    parser = argparse.ArgumentParser(
        description="Sync Nintendo DS .sav files with the 3DS Save Sync server.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python ds_sync.py --sd-path E:\\ --server http://192.168.1.201:8000 --api-key mykey
  python ds_sync.py --roms-dir E:\\nds --saves-dir E:\\nds\\saves --server ... --api-key ...
  python ds_sync.py --sd-path E:\\ --server ... --api-key ... --dry-run
        """,
    )
    parser.add_argument("--sd-path", type=Path,
                        help="Path to SD card root (auto-detects ROM/save locations)")
    parser.add_argument("--roms-dir", type=Path,
                        help="Path to directory containing .nds ROM files")
    parser.add_argument("--saves-dir", type=Path,
                        help="Path to saves directory (if separate from ROMs)")
    parser.add_argument("--server", required=True,
                        help="Server URL (e.g. http://192.168.1.201:8000)")
    parser.add_argument("--api-key", required=True,
                        help="API key for server authentication")
    parser.add_argument("--console-id", default=None,
                        help="Console ID override (default: auto-generated per SD card)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be synced without making changes")

    args = parser.parse_args()

    # Validate args
    if not args.sd_path and not args.roms_dir:
        parser.error("Either --sd-path or --roms-dir is required")

    # Determine scan paths and SD root (for storing identity/state)
    if args.sd_path:
        if not args.sd_path.exists():
            print(f"Error: SD card path does not exist: {args.sd_path}")
            sys.exit(1)
        scan_path = args.sd_path
        sd_root = args.sd_path
        saves_dir = args.saves_dir
    else:
        if not args.roms_dir.exists():
            print(f"Error: ROMs directory does not exist: {args.roms_dir}")
            sys.exit(1)
        scan_path = args.roms_dir
        sd_root = args.roms_dir  # Store identity in roms dir if no SD root
        saves_dir = args.saves_dir

    # Strip trailing slash from server URL
    server = args.server.rstrip("/")

    # Set up sync directory on SD card (stores console ID + state)
    sync_dir = get_sync_dir(sd_root)
    console_id = args.console_id or get_console_id(sync_dir)

    # Load game name database
    db_path = Path(__file__).parent.parent / "server" / "data" / "dstdb.txt"
    name_db = load_name_database(db_path)

    print("DS Save Sync")
    print(f"Server: {server}")
    print(f"Console ID: {console_id}")
    print(f"Scanning: {scan_path}")

    # Check server connectivity
    status, resp = api_get(server, "/status", args.api_key)
    if status != 200:
        print(f"Error: Cannot reach server (HTTP {status})")
        sys.exit(1)
    print("Server OK")

    # Scan for games
    print("\nScanning for NDS ROMs + saves...")
    games = scan_roms(scan_path, saves_dir)

    # Filter out ROMs not in the database (boot ROMs, utilities, etc.)
    unknown = [g for g in games if g["gamecode"] not in name_db]
    games = [g for g in games if g["gamecode"] in name_db]

    if unknown:
        print(f"Skipped {len(unknown)} non-game ROM(s) not in database:")
        for g in unknown:
            print(f"  {g['rom_path'].name} ({g['gamecode']})")

    # Apply game names from database
    for g in games:
        g["name"] = name_db[g["gamecode"]]

    if not games:
        print("No NDS games found.")
        print(f"\nSearched in: {scan_path}")
        print("Make sure .nds ROM files are present.")
        sys.exit(0)

    with_saves = [g for g in games if g["has_save"]]
    without_saves = [g for g in games if not g["has_save"]]
    print(f"Found {len(games)} game(s) ({len(with_saves)} with saves, {len(without_saves)} without):")
    for g in with_saves:
        size_kb = g["sav_path"].stat().st_size / 1024
        print(f"  {g['name']:<40s} {g['gamecode']}  {size_kb:>7.1f} KB  {g['sav_path'].name}")
    if without_saves:
        print(f"  ({len(without_saves)} game(s) without local saves - will download from server if available)")

    # Load sync state from SD card
    state = load_state(sync_dir)

    # Run sync
    state = do_sync(games, server, args.api_key, console_id, state, args.dry_run)

    # Save state to SD card
    if not args.dry_run:
        save_state(sync_dir, state)

    print("\nDone.")


if __name__ == "__main__":
    main()
