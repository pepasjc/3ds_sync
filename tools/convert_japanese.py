#!/usr/bin/env python3
"""Convert Japanese characters in game database to romaji."""

import re
from pathlib import Path

import pykakasi

DB_PATH = Path(__file__).parent.parent / "server" / "data" / "3dstdb.txt"

# Japanese character ranges (hiragana, katakana, kanji)
JAPANESE_RE = re.compile(r'[\u3040-\u309F\u30A0-\u30FF\u4E00-\u9FFF]')

# Korean characters - can't convert these
KOREAN_RE = re.compile(r'[\uAC00-\uD7AF\u1100-\u11FF]')

# Special characters to replace
SPECIAL_CHARS = {
    '\u30FB': ' ',  # Katakana middle dot → space
    '\u30FC': '-',  # Katakana prolonged sound mark → dash
    '\u3000': ' ',  # Ideographic space → space
    '\u2019': "'",  # Right single quote → apostrophe
    '\u2018': "'",  # Left single quote → apostrophe
    '\u201C': '"',  # Left double quote
    '\u201D': '"',  # Right double quote
    '\u2014': '-',  # Em dash
    '\u2013': '-',  # En dash
    '\u00D7': 'x',  # Multiplication sign
    '\u2606': '',   # White star
    '\u2605': '',   # Black star
    '\u266A': '',   # Music note
    '\u2764': '',   # Heart
    '\uFF01': '!',  # Fullwidth exclamation
    '\uFF1F': '?',  # Fullwidth question mark
}


def clean_special_chars(text: str) -> str:
    """Replace special Unicode chars with ASCII equivalents."""
    for char, replacement in SPECIAL_CHARS.items():
        text = text.replace(char, replacement)
    # Clean up multiple spaces
    text = re.sub(r'\s+', ' ', text).strip()
    return text


def main():
    kks = pykakasi.kakasi()

    lines = DB_PATH.read_text(encoding='utf-8').splitlines()
    new_lines = []
    converted_count = 0
    korean_count = 0

    for line in lines:
        if not line.strip():
            new_lines.append(line)
            continue

        parts = line.split(',', 1)
        if len(parts) != 2:
            new_lines.append(line)
            continue

        code, name = parts

        # Skip Korean - can't convert to romaji
        if KOREAN_RE.search(name):
            korean_count += 1
            continue

        # Convert Japanese to romaji
        if JAPANESE_RE.search(name):
            # First clean special chars
            name = clean_special_chars(name)

            result = kks.convert(name)
            romaji_parts = []
            for item in result:
                # Use hepburn romanization
                romaji_parts.append(item['hepburn'])

            # Join and clean up
            romaji = ''.join(romaji_parts)
            # Clean any remaining special chars after conversion
            romaji = clean_special_chars(romaji)
            # Capitalize first letter of each word
            romaji = ' '.join(word.capitalize() for word in romaji.split())

            new_lines.append(f"{code},{romaji}")
            converted_count += 1
        else:
            # Also clean special chars in non-Japanese entries
            name = clean_special_chars(name)
            new_lines.append(f"{code},{name}")

    # Write back
    DB_PATH.write_text('\n'.join(new_lines) + '\n', encoding='utf-8')

    print(f"Converted {converted_count} Japanese entries to romaji")
    print(f"Removed {korean_count} Korean entries (no romaji available)")
    print(f"Total lines: {len(new_lines)}")


if __name__ == '__main__':
    main()
