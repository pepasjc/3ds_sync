"""Parse and create the 3DSS binary save bundle format.

Bundle format v1 (uncompressed):
  [4B]  Magic: "3DSS"
  [4B]  Version: 1 (uint32 LE)
  [8B]  Title ID (uint64 BE)
  [4B]  Timestamp - unix epoch (uint32 LE)
  [4B]  File count (uint32 LE)
  [4B]  Total data size (uint32 LE)
  -- File table (for each file): --
    [2B]  Path length (uint16 LE)
    [NB]  Path (UTF-8)
    [4B]  File size (uint32 LE)
    [32B] SHA-256 hash
  -- File data (for each file, same order): --
    [NB]  Raw file data

Bundle format v2 (compressed):
  [4B]  Magic: "3DSS"
  [4B]  Version: 2 (uint32 LE)
  [8B]  Title ID (uint64 BE)
  [4B]  Timestamp - unix epoch (uint32 LE)
  [4B]  File count (uint32 LE)
  [4B]  Uncompressed payload size (uint32 LE)
  -- Zlib compressed payload: --
    File table + file data (same format as v1 payload)
"""

from __future__ import annotations

import hashlib
import struct
import zlib

from app.models.save import (
    BUNDLE_MAGIC,
    BUNDLE_VERSION,
    BUNDLE_VERSION_COMPRESSED,
    BundleFile,
    SaveBundle,
)


class BundleError(Exception):
    pass


def _parse_payload(data: bytes, file_count: int) -> list[BundleFile]:
    """Parse the file table and file data from payload bytes."""
    offset = 0
    files: list[BundleFile] = []

    # File table
    for _ in range(file_count):
        if offset + 2 > len(data):
            raise BundleError("Truncated file table")

        (path_len,) = struct.unpack_from("<H", data, offset)
        offset += 2

        if offset + path_len > len(data):
            raise BundleError("Truncated file path")
        path = data[offset : offset + path_len].decode("utf-8")
        offset += path_len

        if offset + 4 > len(data):
            raise BundleError("Truncated file size")
        (file_size,) = struct.unpack_from("<I", data, offset)
        offset += 4

        if offset + 32 > len(data):
            raise BundleError("Truncated file hash")
        sha256 = data[offset : offset + 32]
        offset += 32

        files.append(BundleFile(path=path, size=file_size, sha256=sha256))

    # File data
    for f in files:
        if offset + f.size > len(data):
            raise BundleError(f"Truncated file data for {f.path}")
        f.data = data[offset : offset + f.size]
        offset += f.size

        # Verify hash
        actual_hash = hashlib.sha256(f.data).digest()
        if actual_hash != f.sha256:
            raise BundleError(
                f"Hash mismatch for {f.path}: "
                f"expected {f.sha256.hex()}, got {actual_hash.hex()}"
            )

    return files


def parse_bundle(data: bytes) -> SaveBundle:
    """Parse a binary save bundle into a SaveBundle object.

    Supports both v1 (uncompressed) and v2 (zlib compressed) formats.
    """
    if len(data) < 28:
        raise BundleError("Bundle too small for header")

    offset = 0

    # Header
    magic = data[offset : offset + 4]
    if magic != BUNDLE_MAGIC:
        raise BundleError(f"Invalid magic: {magic!r}")
    offset += 4

    (version,) = struct.unpack_from("<I", data, offset)
    if version not in (BUNDLE_VERSION, BUNDLE_VERSION_COMPRESSED):
        raise BundleError(f"Unsupported version: {version}")
    offset += 4

    (title_id,) = struct.unpack_from(">Q", data, offset)
    offset += 8

    (timestamp,) = struct.unpack_from("<I", data, offset)
    offset += 4

    (file_count,) = struct.unpack_from("<I", data, offset)
    offset += 4

    (size_field,) = struct.unpack_from("<I", data, offset)
    offset += 4

    # Get payload (compressed or not)
    if version == BUNDLE_VERSION_COMPRESSED:
        # v2: decompress payload
        compressed_payload = data[offset:]
        try:
            payload = zlib.decompress(compressed_payload)
        except zlib.error as e:
            raise BundleError(f"Decompression failed: {e}")

        if len(payload) != size_field:
            raise BundleError(
                f"Decompressed size mismatch: expected {size_field}, got {len(payload)}"
            )
    else:
        # v1: payload is uncompressed
        payload = data[offset:]

    files = _parse_payload(payload, file_count)

    return SaveBundle(title_id=title_id, timestamp=timestamp, files=files)


def _build_payload(bundle: SaveBundle) -> bytes:
    """Build the file table + file data payload."""
    parts: list[bytes] = []

    # File table
    for f in bundle.files:
        path_bytes = f.path.encode("utf-8")
        parts.append(struct.pack("<H", len(path_bytes)))
        parts.append(path_bytes)
        parts.append(struct.pack("<I", f.size))
        parts.append(f.sha256)

    # File data
    for f in bundle.files:
        parts.append(f.data)

    return b"".join(parts)


def create_bundle(bundle: SaveBundle, compress: bool = True) -> bytes:
    """Serialize a SaveBundle into the binary bundle format.

    Args:
        bundle: The save bundle to serialize.
        compress: If True, create a v2 compressed bundle. If False, create v1.
    """
    payload = _build_payload(bundle)

    parts: list[bytes] = []

    # Header
    parts.append(BUNDLE_MAGIC)

    if compress:
        # v2 compressed format
        compressed_payload = zlib.compress(payload, level=6)
        parts.append(struct.pack("<I", BUNDLE_VERSION_COMPRESSED))
        parts.append(struct.pack(">Q", bundle.title_id))
        parts.append(struct.pack("<I", bundle.timestamp))
        parts.append(struct.pack("<I", len(bundle.files)))
        parts.append(struct.pack("<I", len(payload)))  # uncompressed size
        parts.append(compressed_payload)
    else:
        # v1 uncompressed format
        parts.append(struct.pack("<I", BUNDLE_VERSION))
        parts.append(struct.pack(">Q", bundle.title_id))
        parts.append(struct.pack("<I", bundle.timestamp))
        parts.append(struct.pack("<I", len(bundle.files)))
        parts.append(struct.pack("<I", bundle.total_size))
        parts.append(payload)

    return b"".join(parts)
