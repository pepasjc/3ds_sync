from pathlib import Path
import zipfile

import pytest

from rom_installer import (
    PSIO_MAX_NAME,
    build_install_plan,
    choose_extract_format,
    clean_ps1_title,
    default_rom_format,
    derive_download_filename,
    group_multidisc_roms,
    opl_disc_id,
    opl_ps2_media,
    psio_safe_name,
    resolve_profile_rom_folder,
    sanitize_installed_files,
    strip_disc_tag,
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


def _multidisc_catalog():
    # Three discs of one game (shared title_id/serial) plus a single-disc game.
    return [
        {
            "rom_id": "SLUS00868",
            "primary_rom_id": "SLUS00868",
            "title_id": "SCUS94163",
            "system": "PS1",
            "name": "Final Fantasy VII (Disc 1) (USA)",
            "filename": "Final Fantasy VII (Disc 1) (USA).chd",
            "size": 700,
            "disc_index": 1,
            "disc_total": 3,
            "extract_formats": ["cue", "eboot", "psio"],
        },
        {
            "rom_id": "SLUS00869",
            "primary_rom_id": "SLUS00868",
            "title_id": "SCUS94163",
            "system": "PS1",
            "name": "Final Fantasy VII (Disc 2) (USA)",
            "filename": "Final Fantasy VII (Disc 2) (USA).chd",
            "size": 710,
            "disc_index": 2,
            "disc_total": 3,
            "extract_formats": ["cue", "eboot", "psio"],
        },
        {
            "rom_id": "SLUS00870",
            "primary_rom_id": "SLUS00868",
            "title_id": "SCUS94163",
            "system": "PS1",
            "name": "Final Fantasy VII (Disc 3) (USA)",
            "filename": "Final Fantasy VII (Disc 3) (USA).chd",
            "size": 720,
            "disc_index": 3,
            "disc_total": 3,
            "extract_formats": ["cue", "eboot", "psio"],
        },
        {
            "rom_id": "SLUS00001",
            "primary_rom_id": "SLUS00001",
            "title_id": "SLUS00001",
            "system": "PS1",
            "name": "Single Disc Game (USA)",
            "filename": "Single Disc Game (USA).chd",
            "size": 500,
            "disc_index": 1,
            "disc_total": 1,
            "extract_formats": ["cue", "eboot", "psio"],
        },
    ]


def test_strip_disc_tag():
    assert strip_disc_tag("Final Fantasy VII (Disc 1) (USA)") == "Final Fantasy VII (USA)"
    assert strip_disc_tag("Game (Disk 2 of 4)") == "Game"
    assert strip_disc_tag("No Disc Game") == "No Disc Game"


def test_clean_ps1_title_strips_translation_and_disc_tags():
    assert (
        clean_ps1_title(
            "Ace Combat 3 - Electrosphere (Japan, Asia) (Rev 1) [T-En by Team NEMO v0.9] (Disc 1)"
        )
        == "Ace Combat 3 - Electrosphere (Japan, Asia) (Rev 1)"
    )


def test_psio_folder_strips_translation_tag(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1", "rom_format": "auto"}
    rom = {
        "rom_id": "SLPS00001",
        "system": "PS1",
        "name": "Some Game (Japan) [T-En by Someone]",
        "filename": "Some Game (Japan) [T-En by Someone].chd",
        "extract_formats": ["cue", "eboot", "psio"],
    }
    plan = build_install_plan(profile, rom, "PS1")
    assert plan.extract_format == "psio"
    assert plan.target_path == tmp_path / "Some Game (Japan)"


def test_psio_collapses_multidisc_into_single_entry(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1", "rom_format": "auto"}
    grouped = group_multidisc_roms(profile, _multidisc_catalog(), "PS1")

    # 3-disc FFVII becomes one row; single-disc game untouched.
    assert len(grouped) == 2
    combined = next(r for r in grouped if r["rom_id"] == "SLUS00868")
    assert len(combined["disc_members"]) == 3
    assert combined["disc_total"] == 3
    assert combined["size"] == 700 + 710 + 720
    assert combined["name"] == "Final Fantasy VII (USA)"

    # Installing the combined entry targets the primary rom id (server returns
    # the full multi-disc set) as a PSIO archive folder.
    plan = build_install_plan(profile, combined, "PS1")
    assert plan.rom_id == "SLUS00868"
    assert plan.extract_format == "psio"
    assert plan.extract_archive is True
    assert plan.target_path == tmp_path / "Final Fantasy VII (USA)"


def test_same_serial_single_disc_revisions_not_combined(tmp_path):
    # Two versions of a single-disc game share a serial (title_id) but carry
    # NO (Disc N) tag — must stay as two separate rows, never a "multi-disc".
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1", "rom_format": "auto"}
    roms = [
        {
            "rom_id": "SCUS94228",
            "primary_rom_id": "SCUS94228",
            "title_id": "SCUS94228",
            "system": "PS1",
            "name": "Alundra (USA)",
            "filename": "Alundra (USA).chd",
            "disc_index": 1,
            "disc_total": 1,
            "extract_formats": ["cue", "eboot", "psio"],
        },
        {
            "rom_id": "SCUS94228__rev1",
            "primary_rom_id": "SCUS94228",
            "title_id": "SCUS94228",
            "system": "PS1",
            "name": "Alundra (USA) (Rev 1) [Un-Worked Design by Supper v1]",
            "filename": "Alundra (USA) (Rev 1) [Un-Worked Design by Supper v1].chd",
            "disc_index": 1,
            "disc_total": 1,
            "extract_formats": ["cue", "eboot", "psio"],
        },
    ]
    grouped = group_multidisc_roms(profile, roms, "PS1")
    assert len(grouped) == 2
    assert all("disc_members" not in r for r in grouped)


def test_raw_format_keeps_discs_separate(tmp_path):
    # Generic profile + raw output: server can't stitch discs, so each disc
    # must remain its own catalog row.
    profile = {"device_type": "Generic", "path": str(tmp_path), "system": "PS1"}
    grouped = group_multidisc_roms(profile, _multidisc_catalog(), "PS1", override_format="raw")
    assert len(grouped) == 4
    assert all("disc_members" not in r for r in grouped)


def test_safe_extract_zip_rejects_path_traversal(tmp_path):
    archive = tmp_path / "bad.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("../escape.bin", b"bad")

    with pytest.raises(ValueError):
        _safe_extract_zip(archive, tmp_path / "out")


def test_clean_ps1_title_strips_non_ascii_and_caps_length():
    assert clean_ps1_title("Pokémon (USA) [T-En]") == "Pokemon (USA)"
    assert clean_ps1_title("A" * 80) == "A" * PSIO_MAX_NAME
    assert len(clean_ps1_title("A" * 80)) <= PSIO_MAX_NAME


def test_psio_safe_name_reserves_suffix_room():
    assert psio_safe_name("A" * 80, reserve=4) == "A" * (PSIO_MAX_NAME - 4)
    assert psio_safe_name("Pokémon", reserve=0) == "Pokemon"


def test_sanitize_renames_non_ascii_pair_and_folder(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1"}
    game = tmp_path / "Pokémon (USA)"
    game.mkdir()
    (game / "Pokémon (USA).bin").write_bytes(b"BIN")
    (game / "Pokémon (USA).cu2").write_bytes(b"CU2")

    renames = sanitize_installed_files(profile, "PS1")

    new_game = tmp_path / "Pokemon (USA)"
    assert new_game.is_dir()
    assert sorted(p.name for p in new_game.iterdir()) == [
        "Pokemon (USA).bin",
        "Pokemon (USA).cu2",
    ]
    assert len(renames) == 3  # two files + the folder


def test_sanitize_keeps_multidisc_pairs_and_updates_lst(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1"}
    game = tmp_path / "Gámé"
    game.mkdir()
    for n in (1, 2):
        stem = f"Gámé (Disc {n})"
        (game / f"{stem}.bin").write_bytes(b"BIN")
        (game / f"{stem}.cu2").write_bytes(b"CU2")
    (game / "MULTIDISC.LST").write_bytes(
        "Gámé (Disc 1).bin\r\nGámé (Disc 2).bin\r\n".encode("utf-8")
    )

    sanitize_installed_files(profile, "PS1")

    new_game = tmp_path / "Game"
    names = sorted(p.name for p in new_game.iterdir())
    assert "Game (Disc 1).bin" in names
    assert "Game (Disc 1).cu2" in names
    assert "Game (Disc 2).bin" in names
    assert "Game (Disc 2).cu2" in names
    lst_lines = sorted(
        line.strip()
        for line in (new_game / "MULTIDISC.LST")
        .read_text(encoding="utf-8")
        .splitlines()
    )
    assert lst_lines == ["Game (Disc 1).bin", "Game (Disc 2).bin"]


def test_sanitize_collision_keeps_pairs_aligned(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1"}
    game = tmp_path / "Collide"
    game.mkdir()
    # Two source stems that both reduce to ASCII "Game".
    (game / "Gámé.bin").write_bytes(b"1")
    (game / "Gámé.cu2").write_bytes(b"1")
    (game / "Game.bin").write_bytes(b"2")  # already ASCII -> not renamed
    (game / "Game.cu2").write_bytes(b"2")

    sanitize_installed_files(profile, "PS1")

    names = sorted(p.name for p in game.iterdir())
    assert "Game.bin" in names
    assert "Game.cu2" in names
    assert "Game~2.bin" in names  # accented pair displaced together
    assert "Game~2.cu2" in names


def test_sanitize_truncates_long_names_under_sixty(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path), "system": "PS1"}
    long_stem = "A" * 80
    game = tmp_path / long_stem
    game.mkdir()
    (game / f"{long_stem}.bin").write_bytes(b"BIN")
    (game / f"{long_stem}.cu2").write_bytes(b"CU2")

    sanitize_installed_files(profile, "PS1")

    for folder in tmp_path.iterdir():
        assert len(folder.name) <= PSIO_MAX_NAME
        for f in folder.iterdir():
            assert len(f.name) <= PSIO_MAX_NAME


def test_sanitize_missing_folder_is_noop(tmp_path):
    profile = {"device_type": "PSIO", "path": str(tmp_path / "missing"), "system": "PS1"}
    assert sanitize_installed_files(profile, "PS1") == []


# ── OPL (Open PS2 Loader) install layout ────────────────────────────────────


def test_opl_disc_id_formats_sony_serial():
    assert opl_disc_id("SLPS20436") == "SLPS_204.36"
    assert opl_disc_id("SLPS-20436") == "SLPS_204.36"
    assert opl_disc_id("slps_204.36") == "SLPS_204.36"
    assert opl_disc_id("SCUS94503") == "SCUS_945.03"
    assert opl_disc_id("not a serial") == ""
    assert opl_disc_id(None) == ""


def test_opl_ps2_dvd_goes_to_dvd_folder_with_serial_name(tmp_path):
    profile = {"device_type": "OPL", "path": str(tmp_path), "system": "PS2"}
    rom = {
        "rom_id": "SLPS_204.36",
        "title_id": "SLPS20436",
        "system": "PS2",
        "name": "Sakigake Otokojuku",
        "filename": "Sakigake Otokojuku (Japan).chd",
        "extract_format": "iso",
        "extract_formats": ["iso"],
    }
    plan = build_install_plan(profile, rom, "PS2")

    assert plan.extract_format == "iso"
    assert plan.target_path == tmp_path / "DVD" / "SLPS_204.36.Sakigake Otokojuku.iso"


def test_opl_ps2_cd_uses_cd_folder(tmp_path):
    # The server advertises 'cue' for PS2 CD media — OPL still wants .iso,
    # routed to the CD/ folder.
    profile = {"device_type": "OPL", "path": str(tmp_path), "system": "PS2"}
    rom = {
        "rom_id": "SLUS_203.44",
        "title_id": "SLUS20344",
        "system": "PS2",
        "name": "Mr. Mosquito",
        "filename": "Mr. Mosquito (USA).chd",
        "extract_format": "cue",
        "extract_formats": ["cue"],
    }
    plan = build_install_plan(profile, rom, "PS2")

    assert plan.extract_format == "iso"
    assert plan.target_path == tmp_path / "CD" / "SLUS_203.44.Mr. Mosquito.iso"
    assert opl_ps2_media(rom) == "CD"


def test_opl_ps1_vcd_goes_to_pops_folder_serial_only(tmp_path):
    # POPStarter matches VMCs by VCD filename prefix, so VCDs are named by
    # serial only (no game name) in the POPS/ folder.
    profile = {"device_type": "OPL", "path": str(tmp_path), "system": "PS1"}
    rom = {
        "rom_id": "SLUS_012.34",
        "title_id": "SLUS01234",
        "system": "PS1",
        "name": "Castlevania Symphony of the Night",
        "filename": "Castlevania SOTN (USA).chd",
        "extract_format": "cue",
        "extract_formats": ["cue", "vcd"],
    }
    plan = build_install_plan(profile, rom, "PS1")

    assert plan.extract_format == "vcd"
    assert plan.target_path == tmp_path / "POPS" / "SLUS_012.34.vcd"


def test_opl_default_rom_format(tmp_path):
    ps2_rom = {"filename": "Game.chd", "extract_format": "iso", "extract_formats": ["iso"]}
    ps1_rom = {"filename": "Game.chd", "extract_format": "cue", "extract_formats": ["cue", "vcd"]}
    profile = {"device_type": "OPL", "path": str(tmp_path)}

    assert default_rom_format(profile, ps2_rom, "PS2") == "iso"
    assert default_rom_format(profile, ps1_rom, "PS1") == "vcd"


def test_opl_strips_disc_tag_and_illegal_chars_in_name(tmp_path):
    profile = {"device_type": "OPL", "path": str(tmp_path), "system": "PS2"}
    rom = {
        "rom_id": "SCES_000.01",
        "title_id": "SCES00001",
        "system": "PS2",
        "name": 'Cool: Game (Disc 1) [Hack]',
        "filename": "Cool Game.chd",
        "extract_format": "iso",
        "extract_formats": ["iso"],
    }
    plan = build_install_plan(profile, rom, "PS2")

    # Disc tag dropped, ':' stripped (illegal on FAT32), serial prefix kept.
    assert plan.target_path == tmp_path / "DVD" / "SCES_000.01.Cool Game [Hack].iso"
