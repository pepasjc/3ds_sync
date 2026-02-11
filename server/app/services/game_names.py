"""Game name lookup service using 3dstdb.txt and dstdb.txt databases."""

from pathlib import Path

# Global cache for game names (loaded once at startup)
# Keep DS and 3DS databases separate to handle duplicate product codes
_3ds_names: dict[str, str] = {}
_ds_names: dict[str, str] = {}


def load_database(db_path: Path | None = None) -> int:
    """Load a game names database from file into the appropriate cache.

    Automatically detects whether it's loading a 3DS or DS database based on filename.
    Returns the number of entries loaded.
    """
    global _3ds_names, _ds_names

    if db_path is None:
        # Default path relative to server root
        db_path = Path(__file__).parent.parent.parent / "data" / "3dstdb.txt"

    if not db_path.exists():
        return 0

    # Determine which database to load into based on filename
    is_ds = "ds" in db_path.name.lower() and "3ds" not in db_path.name.lower()
    target_dict = _ds_names if is_ds else _3ds_names

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
                if code and name:
                    target_dict[code] = name
                    added += 1

    return added


def lookup_names(product_codes: list[str]) -> dict[str, str]:
    """Look up game names for a list of product codes.

    Product codes can be:
    - Full format: CTR-P-XXXX or CTR-N-XXXX (3DS games)
    - Short format: Just the 4-char code (XXXX) - could be DS or 3DS

    For duplicate product codes (exist in both databases), prioritize:
    - 3DS database if product code starts with "CTR"
    - DS database otherwise (short codes are assumed to be DS)

    Returns a dict mapping the input codes to their game names.
    Unknown codes are omitted from the result.
    """
    result = {}

    for code in product_codes:
        # Extract the 4-char game code
        code_upper = code.upper().strip()
        is_3ds_format = code_upper.startswith("CTR-")

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

        # Check appropriate database based on format
        # For CTR- prefix, check 3DS first, then DS as fallback
        # For short codes, check DS first, then 3DS as fallback
        name = None
        if is_3ds_format:
            name = _3ds_names.get(game_code) or _ds_names.get(game_code)
        else:
            name = _ds_names.get(game_code) or _3ds_names.get(game_code)

        if name:
            result[code] = name

    return result


def get_name(product_code: str) -> str | None:
    """Look up a single game name. Returns None if not found."""
    result = lookup_names([product_code])
    return result.get(product_code)
