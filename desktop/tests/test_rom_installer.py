from pathlib import Path
import zipfile

import pytest

from rom_installer import (
    build_install_plan,
    choose_extract_format,
    derive_download_filename,
    resolve_profile_rom_folder,
    _safe_extract_zip,
)


def test_real_hardware_disc_chd_defaults_to_extracted_cue(tmp_path):
    profile = {
        "name": "PS1 SD",
        "device_type": "CD Folder",
        "path": str(tmp_path),
        "system": "PS1",
        "rom_format": "auto",
    }
    rom = {
        "rom_id": "SLUS00001",
        "system": "PS1",
        "name": "Example Game",
        "filename": "Example Game (USA).chd",
        "extract_format": "cue",
        "extract_formats": ["cue"],
    }

    plan = build_install_plan(profile, rom, "PS1")

    assert plan.extract_format == "cue"
    assert plan.extract_archive is True
    assert plan.target_path == tmp_path / "Example Game"


def test_psio_profile_defaults_to_psio_archive(tmp_path):
    profile = {
        "name": "PSIO SD",
        "device_type": "PSIO",
        "path": str(tmp_path),
        "system": "PS1",
        "rom_format": "auto",
    }
    rom = {
        "rom_id": "SLUS00001",
        "system": "PS1",
        "name": "Example Game",
        "filename": "Example Game (USA).chd",
        "extract_format": "cue",
        "extract_formats": ["cue", "eboot", "psio"],
    }

    plan = build_install_plan(profile, rom, "PS1")

    assert plan.extract_format == "psio"
    assert plan.extract_archive is True
    assert plan.target_path == tmp_path / "Example Game"
    assert derive_download_filename("Example Game (USA).chd", "psio") == "Example Game (USA).cu2"


def test_3ds_profile_defaults_follow_hardware_vs_emulator(tmp_path):
    rom = {
        "rom_id": "0004000000000000",
        "system": "3DS",
        "name": "3DS Game",
        "filename": "3DS Game.3ds",
        "extract_formats": ["cia", "decrypted_cci"],
    }

    hardware = {"device_type": "Generic", "path": str(tmp_path), "system": "3DS"}
    emulator = {"device_type": "RetroArch", "path": str(tmp_path), "systems": [{"system": "3DS"}]}

    assert choose_extract_format(hardware, rom, "3DS") == "cia"
    assert choose_extract_format(emulator, rom, "3DS") == "decrypted_cci"
    assert derive_download_filename("3DS Game.3ds", "cia") == "3DS Game.cia"
    assert derive_download_filename("3DS Game.3ds", "decrypted_cci") == "3DS Game.cci"


def test_profile_rom_folder_uses_override_before_global_root(tmp_path):
    root = tmp_path / "root"
    override = tmp_path / "saturn"
    profile = {
        "device_type": "RetroArch",
        "path": str(root),
        "systems": [
            {"system": "SAT", "enabled": True, "rom_folder": str(override)},
        ],
    }

    assert resolve_profile_rom_folder(profile, "SAT") == override


def test_emudeck_profile_uses_system_subfolder(tmp_path):
    profile = {
        "device_type": "EmuDeck",
        "path": str(tmp_path),
        "systems": [{"system": "GBA", "enabled": True}],
    }

    assert resolve_profile_rom_folder(profile, "GBA") == tmp_path / "gba"


def test_safe_extract_zip_rejects_path_traversal(tmp_path):
    archive = tmp_path / "bad.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("../escape.bin", b"bad")

    with pytest.raises(ValueError):
        _safe_extract_zip(archive, tmp_path / "out")
