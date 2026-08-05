"""Tests for the NCSD/NCCH "is this cart image already decrypted?" probe."""

import struct

from app.services import ctr_rom

MEDIA_UNIT = 0x200
PARTITION_OFFSET = 0x4000
EXHEADER_OFFSET = 0x200          # relative to the NCCH header
EXHEADER_SIZE = 0x400
EXEFS_MEDIA_OFFSET = 0x8         # 0x1000 bytes into the partition
EXEFS_MEDIA_SIZE = 0x4
PARTITION_MEDIA_SIZE = 0x20      # 0x4000 bytes


def _plaintext_exefs_header() -> bytes:
    header = bytearray(0x200)
    header[0x00:0x08] = b'.code\0\0\0'
    struct.pack_into('<II', header, 0x08, 0, 0x100)
    header[0x10:0x18] = b'icon\0\0\0\0'
    struct.pack_into('<II', header, 0x18, 0x200, 0x40)
    return bytes(header)


def _encrypted_blob(size: int, seed: int) -> bytes:
    # Deterministic high-entropy filler — every byte has the high bit set so it
    # can never be mistaken for the ASCII names/titles the heuristic looks for.
    return bytes(0x80 | ((seed + i * 37) & 0x7F) for i in range(size))


def _build_cart(decrypted: bool, no_crypto_flag: bool, partitions: int = 1) -> bytes:
    """Synthesize a minimal but structurally valid NCSD cart image."""
    total = PARTITION_OFFSET + partitions * PARTITION_MEDIA_SIZE * MEDIA_UNIT
    image = bytearray(total)
    image[0x100:0x104] = ctr_rom.NCSD_MAGIC
    struct.pack_into('<I', image, 0x104, total // MEDIA_UNIT)

    for index in range(partitions):
        part_offset = PARTITION_OFFSET + index * PARTITION_MEDIA_SIZE * MEDIA_UNIT
        struct.pack_into(
            '<II', image, 0x120 + index * 8,
            part_offset // MEDIA_UNIT, PARTITION_MEDIA_SIZE,
        )

        ncch = bytearray(0x200)
        ncch[0x100:0x104] = ctr_rom.NCCH_MAGIC
        struct.pack_into('<I', ncch, 0x180, EXHEADER_SIZE)
        flags = bytearray(8)
        if no_crypto_flag:
            flags[7] = ctr_rom.FLAG_NO_CRYPTO
        else:
            flags[3] = 0x01                      # pretend: 7.x key slot
            flags[7] = ctr_rom.FLAG_FIXED_CRYPTO_KEY
        ncch[0x188:0x190] = bytes(flags)
        struct.pack_into('<II', ncch, 0x1A0, EXEFS_MEDIA_OFFSET, EXEFS_MEDIA_SIZE)
        image[part_offset:part_offset + 0x200] = ncch

        exheader_at = part_offset + EXHEADER_OFFSET
        exefs_at = part_offset + EXEFS_MEDIA_OFFSET * MEDIA_UNIT
        if decrypted:
            image[exheader_at:exheader_at + 8] = b'AppTitl\0'
            image[exefs_at:exefs_at + 0x200] = _plaintext_exefs_header()
        else:
            image[exheader_at:exheader_at + EXHEADER_SIZE] = _encrypted_blob(EXHEADER_SIZE, index + 1)
            image[exefs_at:exefs_at + 0x200] = _encrypted_blob(0x200, index + 9)

    return bytes(image)


def _write(tmp_path, name: str, data: bytes):
    path = tmp_path / name
    path.write_bytes(data)
    return path


class TestProbe:
    def test_non_ncsd_file_is_not_a_cart(self, tmp_path):
        info = ctr_rom.probe(_write(tmp_path, "junk.3ds", b'CARTROM'))
        assert info.is_ncsd is False
        assert info.decrypted is False
        assert info.needs_flag_patch is False

    def test_missing_file_reports_unreadable(self, tmp_path):
        info = ctr_rom.probe(tmp_path / "nope.3ds")
        assert info.is_ncsd is False
        assert "unreadable" in info.detail

    def test_encrypted_cart(self, tmp_path):
        info = ctr_rom.probe(_write(tmp_path, "enc.3ds", _build_cart(decrypted=False, no_crypto_flag=False)))
        assert info.is_ncsd is True
        assert info.decrypted is False
        assert info.needs_flag_patch is False

    def test_decrypted_cart_with_flags_set(self, tmp_path):
        info = ctr_rom.probe(_write(tmp_path, "dec.3ds", _build_cart(decrypted=True, no_crypto_flag=True)))
        assert info.decrypted is True
        assert info.flags_marked is True
        assert info.needs_flag_patch is False

    def test_decrypted_cart_with_stale_flags_is_detected(self, tmp_path):
        """The real-world failure case: plaintext data, headers still say encrypted."""
        info = ctr_rom.probe(_write(tmp_path, "dec.3ds", _build_cart(decrypted=True, no_crypto_flag=False)))
        assert info.decrypted is True
        assert info.flags_marked is False
        assert info.needs_flag_patch is True
        assert "plaintext ExeFS header" in info.detail

    def test_exheader_fallback_when_exefs_is_absent(self, tmp_path):
        data = bytearray(_build_cart(decrypted=True, no_crypto_flag=False))
        struct.pack_into('<II', data, PARTITION_OFFSET + 0x1A0, 0, 0)   # no ExeFS
        info = ctr_rom.probe(_write(tmp_path, "dec.3ds", bytes(data)))
        assert info.decrypted is True
        assert "plaintext ExHeader title" in info.detail

    def test_multi_partition_cart(self, tmp_path):
        info = ctr_rom.probe(
            _write(tmp_path, "dec.3ds", _build_cart(decrypted=True, no_crypto_flag=False, partitions=3))
        )
        assert len(info.partitions) == 3
        assert info.decrypted is True

    def test_mixed_partitions_are_treated_as_encrypted(self, tmp_path):
        data = bytearray(_build_cart(decrypted=True, no_crypto_flag=False, partitions=2))
        # Scramble partition 1's ExeFS + ExHeader so it reads as encrypted.
        second = PARTITION_OFFSET + PARTITION_MEDIA_SIZE * MEDIA_UNIT
        exefs_at = second + EXEFS_MEDIA_OFFSET * MEDIA_UNIT
        data[exefs_at:exefs_at + 0x200] = _encrypted_blob(0x200, 3)
        data[second + EXHEADER_OFFSET:second + EXHEADER_OFFSET + EXHEADER_SIZE] = _encrypted_blob(EXHEADER_SIZE, 5)
        info = ctr_rom.probe(_write(tmp_path, "mixed.3ds", bytes(data)))
        assert info.decrypted is False

    def test_trimmed_image_does_not_crash(self, tmp_path):
        data = _build_cart(decrypted=True, no_crypto_flag=False)[:0x4100]
        info = ctr_rom.probe(_write(tmp_path, "trim.3ds", data))
        assert info.is_ncsd is True


class TestWriteDecryptedCopy:
    def test_copy_patches_flags_and_preserves_payload(self, tmp_path):
        src = _write(tmp_path, "dec.3ds", _build_cart(decrypted=True, no_crypto_flag=False))
        dst = tmp_path / "out.cci"
        ctr_rom.write_decrypted_copy(src, dst)

        original = src.read_bytes()
        copied = dst.read_bytes()
        assert len(copied) == len(original)

        flags_at = PARTITION_OFFSET + ctr_rom.NCCH_FLAGS_OFFSET
        assert copied[flags_at + 3] == 0x00
        assert copied[flags_at + 7] & ctr_rom.FLAG_NO_CRYPTO
        assert not copied[flags_at + 7] & ctr_rom.FLAG_FIXED_CRYPTO_KEY

        # Only the 8 flag bytes changed.
        assert copied[:flags_at] == original[:flags_at]
        assert copied[flags_at + 8:] == original[flags_at + 8:]

        # The copy now reports as a fully marked decrypted image.
        info = ctr_rom.probe(dst)
        assert info.decrypted is True
        assert info.flags_marked is True

    def test_copy_of_encrypted_image_leaves_flags_alone(self, tmp_path):
        src = _write(tmp_path, "enc.3ds", _build_cart(decrypted=False, no_crypto_flag=False))
        dst = tmp_path / "out.cci"
        ctr_rom.write_decrypted_copy(src, dst)
        assert dst.read_bytes() == src.read_bytes()

    def test_copy_patches_every_partition(self, tmp_path):
        src = _write(
            tmp_path, "dec.3ds", _build_cart(decrypted=True, no_crypto_flag=False, partitions=2)
        )
        dst = tmp_path / "out.cci"
        ctr_rom.write_decrypted_copy(src, dst)
        info = ctr_rom.probe(dst)
        assert len(info.partitions) == 2
        assert all(p.no_crypto for p in info.partitions)


class TestLargeCopy:
    def test_copy_spans_multiple_chunks(self, tmp_path, monkeypatch):
        monkeypatch.setattr(ctr_rom, '_COPY_CHUNK', 4096)
        src = _write(tmp_path, "dec.3ds", _build_cart(decrypted=True, no_crypto_flag=False))
        dst = tmp_path / "out.cci"
        ctr_rom.write_decrypted_copy(src, dst)
        assert ctr_rom.probe(dst).flags_marked is True
