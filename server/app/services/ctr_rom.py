"""3DS cart image (NCSD/CCI) header inspection — "is this already decrypted?"

No keys and no crypto live here: the module only parses NCSD/NCCH headers and
applies a plaintext heuristic to the ExeFS / ExHeader of each partition.  That
is enough to route a ROM down the right conversion path.

Why this matters
----------------
Every 3DS tool (ninfs/pyctr's ``mount_cci``, ``3dsconv``, ctrtool) decides
whether to decrypt from the NCCH crypto flags at ``NCCH+0x188``.  Plenty of
"decrypted .3ds" dumps in the wild carry plaintext data while still *claiming*
to be encrypted, because the decryptor never patched the flags.  Handed such a
file the tools happily "decrypt" plaintext into garbage, which surfaces as::

    pyctr.type.exefs.BadOffsetError: offset is not a multiple of 0x200: 0x50fcd949

or as 3dsconv silently producing no CIA at all.  Detecting the plaintext up
front lets the server skip the converter entirely for decrypted-CCI output and
hand the CIA converter a flag-corrected copy.

Layout reference: 3dbrew.org NCSD / NCCH / ExeFS pages.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

MEDIA_UNIT = 0x200

NCSD_MAGIC = b"NCSD"
NCSD_MAGIC_OFFSET = 0x100
NCSD_PARTITION_TABLE_OFFSET = 0x120
NCSD_PARTITION_COUNT = 8

NCCH_MAGIC = b"NCCH"
NCCH_MAGIC_OFFSET = 0x100
NCCH_EXHEADER_SIZE_OFFSET = 0x180
NCCH_FLAGS_OFFSET = 0x188
NCCH_EXEFS_OFFSET = 0x1A0
NCCH_HEADER_SIZE = 0x200

# flags[3] selects the secondary key slot, flags[7] carries the booleans.
FLAG_CRYPTO_METHOD = 3
FLAG_BITMASK = 7
FLAG_FIXED_CRYPTO_KEY = 0x01
FLAG_NO_CRYPTO = 0x04

_COPY_CHUNK = 8 * 1024 * 1024


@dataclass
class NcchPartition:
    """One NCCH partition inside the cart image."""

    index: int
    offset: int          # absolute byte offset of the NCCH header
    size: int            # partition size in bytes
    flags: bytes         # the 8 raw bytes at NCCH+0x188
    exefs_offset: int    # absolute byte offset, 0 when the partition has none
    exefs_size: int
    exheader_size: int
    plaintext: bool = False
    reason: str = ""

    @property
    def no_crypto(self) -> bool:
        return bool(self.flags[FLAG_BITMASK] & FLAG_NO_CRYPTO)

    @property
    def needs_flag_patch(self) -> bool:
        """True when the data is plaintext but the header still says otherwise."""
        return self.plaintext and (
            not self.no_crypto
            or self.flags[FLAG_CRYPTO_METHOD] != 0
            or bool(self.flags[FLAG_BITMASK] & FLAG_FIXED_CRYPTO_KEY)
        )

    def patched_flags(self) -> bytes:
        flags = bytearray(self.flags)
        flags[FLAG_CRYPTO_METHOD] = 0x00
        flags[FLAG_BITMASK] = (flags[FLAG_BITMASK] & ~FLAG_FIXED_CRYPTO_KEY) | FLAG_NO_CRYPTO
        return bytes(flags)


@dataclass
class CartInfo:
    """Result of :func:`probe`."""

    path: Path
    is_ncsd: bool = False
    decrypted: bool = False       # every partition's data is plaintext
    flags_marked: bool = False    # ...and every header already says NoCrypto
    partitions: list[NcchPartition] = field(default_factory=list)
    detail: str = ""

    @property
    def needs_flag_patch(self) -> bool:
        return self.decrypted and not self.flags_marked

    def describe(self) -> str:
        if not self.is_ncsd:
            return f"not an NCSD cart image ({self.detail})"
        state = "decrypted" if self.decrypted else "encrypted"
        marked = "flags say NoCrypto" if self.flags_marked else "flags still say encrypted"
        return f"{state}, {marked} ({len(self.partitions)} partition(s))"


def probe(path: Path | str) -> CartInfo:
    """Inspect a .3ds/.cci and report whether its contents are already plaintext.

    Never raises for malformed input — an unreadable or non-NCSD file comes
    back as ``is_ncsd=False`` so callers fall through to the normal converter.
    """
    path = Path(path)
    info = CartInfo(path=path)
    try:
        with open(path, 'rb') as fh:
            header = fh.read(0x200)
            if len(header) < 0x200 or header[NCSD_MAGIC_OFFSET:NCSD_MAGIC_OFFSET + 4] != NCSD_MAGIC:
                info.detail = "missing NCSD magic"
                return info
            info.is_ncsd = True
            file_size = path.stat().st_size

            for index in range(NCSD_PARTITION_COUNT):
                entry = NCSD_PARTITION_TABLE_OFFSET + index * 8
                media_offset, media_size = struct.unpack_from('<II', header, entry)
                if media_offset == 0 or media_size == 0:
                    continue
                part_offset = media_offset * MEDIA_UNIT
                part_size = media_size * MEDIA_UNIT
                if part_offset + NCCH_HEADER_SIZE > file_size:
                    # Trimmed dump: the partition isn't there to inspect.
                    continue
                partition = _read_partition(fh, index, part_offset, part_size)
                if partition is None:
                    continue
                _classify(fh, partition, file_size)
                info.partitions.append(partition)
    except OSError as exc:
        info.detail = f"unreadable: {exc}"
        return info

    if not info.partitions:
        info.detail = info.detail or "no readable NCCH partitions"
        return info

    info.decrypted = all(p.plaintext for p in info.partitions)
    info.flags_marked = info.decrypted and not any(p.needs_flag_patch for p in info.partitions)
    info.detail = '; '.join(f"p{p.index}:{p.reason}" for p in info.partitions)
    return info


def _read_partition(fh, index: int, offset: int, size: int) -> NcchPartition | None:
    fh.seek(offset)
    ncch = fh.read(NCCH_HEADER_SIZE)
    if len(ncch) < NCCH_HEADER_SIZE:
        return None
    if ncch[NCCH_MAGIC_OFFSET:NCCH_MAGIC_OFFSET + 4] != NCCH_MAGIC:
        return None
    exefs_media_offset, exefs_media_size = struct.unpack_from('<II', ncch, NCCH_EXEFS_OFFSET)
    exheader_size = struct.unpack_from('<I', ncch, NCCH_EXHEADER_SIZE_OFFSET)[0]
    return NcchPartition(
        index=index,
        offset=offset,
        size=size,
        flags=ncch[NCCH_FLAGS_OFFSET:NCCH_FLAGS_OFFSET + 8],
        exefs_offset=offset + exefs_media_offset * MEDIA_UNIT if exefs_media_offset else 0,
        exefs_size=exefs_media_size * MEDIA_UNIT,
        exheader_size=exheader_size,
    )


def _classify(fh, partition: NcchPartition, file_size: int) -> None:
    """Decide whether ``partition``'s payload is plaintext, cheapest test first."""
    if partition.no_crypto:
        partition.plaintext = True
        partition.reason = "NoCrypto flag"
        return

    if partition.exefs_offset and partition.exefs_offset + NCCH_HEADER_SIZE <= file_size:
        fh.seek(partition.exefs_offset)
        if _exefs_header_is_plaintext(fh.read(NCCH_HEADER_SIZE)):
            partition.plaintext = True
            partition.reason = "plaintext ExeFS header"
            return

    if partition.exheader_size:
        exheader_offset = partition.offset + NCCH_HEADER_SIZE
        if exheader_offset + 8 <= file_size:
            fh.seek(exheader_offset)
            if _exheader_is_plaintext(fh.read(8)):
                partition.plaintext = True
                partition.reason = "plaintext ExHeader title"
                return

    partition.reason = "encrypted"


def _exefs_header_is_plaintext(header: bytes) -> bool:
    """An ExeFS header is 10 * 16-byte entries, 0x20 reserved zeros, then hashes.

    Encrypted, those first 0xC0 bytes are indistinguishable from random, so the
    combination of "32 reserved zero bytes" + "first entry is a named blob at
    offset 0 with 0x200-aligned siblings" is a decisive plaintext signal.
    """
    if len(header) < 0xC0:
        return False
    if header[0xA0:0xC0] != b'\0' * 0x20:
        return False

    name, offset, size = _exefs_entry(header, 0)
    if offset != 0 or size == 0 or not _is_printable_name(name):
        return False

    for index in range(1, 10):
        name, offset, size = _exefs_entry(header, index)
        if name == b'\0' * 8 and offset == 0 and size == 0:
            continue           # unused slot
        if offset % MEDIA_UNIT != 0 or not _is_printable_name(name):
            return False
    return True


def _exefs_entry(header: bytes, index: int) -> tuple[bytes, int, int]:
    base = index * 0x10
    name = header[base:base + 8]
    offset, size = struct.unpack_from('<II', header, base + 8)
    return name, offset, size


def _is_printable_name(name: bytes) -> bool:
    """ExeFS names are short ASCII (".code", "icon", "banner"), NUL-padded."""
    stripped = name.rstrip(b'\0')
    if not stripped:
        return False
    return all(0x20 <= b < 0x7F for b in stripped)


def _exheader_is_plaintext(title: bytes) -> bool:
    """The ExHeader opens with an 8-byte ASCII application title."""
    stripped = title.rstrip(b'\0')
    if not stripped:
        return False
    return all(0x20 <= b < 0x7F for b in stripped)


def write_decrypted_copy(src: Path | str, dst: Path | str, info: CartInfo | None = None) -> CartInfo:
    """Copy ``src`` to ``dst``, rewriting each NCCH's crypto flags to NoCrypto.

    Used for ROMs whose data is already plaintext but whose headers still claim
    encryption.  The payload is copied byte for byte; only 8 bytes per partition
    header change, which is exactly what a decryptor would have written.
    """
    src = Path(src)
    dst = Path(dst)
    if info is None:
        info = probe(src)

    dst.parent.mkdir(parents=True, exist_ok=True)
    with open(src, 'rb') as fin, open(dst, 'wb') as fout:
        while True:
            chunk = fin.read(_COPY_CHUNK)
            if not chunk:
                break
            fout.write(chunk)

        for partition in info.partitions:
            if not partition.plaintext:
                continue
            fout.seek(partition.offset + NCCH_FLAGS_OFFSET)
            fout.write(partition.patched_flags())
    return info
