"""Game name lookup service using 3dstdb.txt and dstdb.txt databases."""

from pathlib import Path

# Global cache for game names (loaded once at startup)
_game_names: dict[str, str] = {}


def load_database(db_path: Path | None = None) -> int:
    """Load a game names database from file, merging into the global cache.

    Can be called multiple times to load multiple databases.
    Returns the number of NEW entries loaded (not counting duplicates).
    """
    global _game_names

    if db_path is None:
        # Default path relative to server root
        db_path = Path(__file__).parent.parent.parent / "data" / "3dstdb.txt"

    if not db_path.exists():
        return 0

    added = 0
    with open(db_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or "," not in line:
                continue

            # Format: CODE,Game Name
            parts = line.split(",", 1)
            if len(parts) == 2:
                code = parts[0].strip().upper()
                name = parts[1].strip()
                if code and name and code not in _game_names:
                    _game_names[code] = name
                    added += 1

    return added


def lookup_names(product_codes: list[str]) -> dict[str, str]:
    """Look up game names for a list of product codes.

    Product codes can be:
    - Full format: CTR-P-XXXX or CTR-N-XXXX
    - Short format: Just the 4-char code (XXXX)

    Returns a dict mapping the input codes to their game names.
    Unknown codes are omitted from the result.
    """
    result = {}

    for code in product_codes:
        # Extract the 4-char game code
        code_upper = code.upper().strip()

        if len(code_upper) >= 10 and "-" in code_upper:
            # Full format like CTR-P-BRBE - extract last 4 chars before any suffix
            parts = code_upper.split("-")
            if len(parts) >= 3:
                game_code = parts[2][:4]  # Take first 4 chars of the game code part
            else:
                game_code = code_upper[-4:]
        elif len(code_upper) == 4:
            # Already just the 4-char code
            game_code = code_upper
        else:
            # Try last 4 chars as fallback
            game_code = code_upper[-4:] if len(code_upper) >= 4 else code_upper

        if game_code in _game_names:
            result[code] = _game_names[game_code]

    return result


def get_name(product_code: str) -> str | None:
    """Look up a single game name. Returns None if not found."""
    result = lookup_names([product_code])
    return result.get(product_code)
