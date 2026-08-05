"""Coverage for the Nintendo - Wii U DAT.

Three things go wrong if the Wii U DAT is treated like the Wii one:

  1. dat_normalizer's filename map matches on substrings, so "Nintendo -
     Wii U.dat" hit the ("nintendo - wii", "WII") entry and merged 2871 Wii U
     titles into the Wii ROM-rename index.
  2. game_names' loader has the same prefix trap, and would have parsed bare
     Wii U product codes (AMKE) as RVL-XXXX-RGN, dropping them all.
  3. The gametdb Wii U DAT writes multi-line ``rom (`` blocks, whose inner
     ``name`` line overwrote the game name — every entry came out as
     "Foo (USA).wux".
"""

from pathlib import Path

import pytest

from app.services import game_names
from app.services.dat_normalizer import _DAT_SYSTEM_MAP

DATS = Path(__file__).resolve().parents[1] / "data" / "dats"
WIIU_DAT = DATS / "Nintendo - Wii U.dat"
WII_DAT = DATS / "Nintendo - Wii.dat"


def _system_for(filename: str) -> str | None:
    lower = filename.lower()
    for keyword, code in _DAT_SYSTEM_MAP:
        if keyword in lower:
            return code
    return None


class TestDatSystemMapping:
    def test_wii_u_dat_is_not_classified_as_wii(self):
        assert _system_for("Nintendo - Wii U.dat") == "WIIU"

    def test_plain_wii_dat_still_maps_to_wii(self):
        assert _system_for("Nintendo - Wii.dat") == "WII"
        assert _system_for("Nintendo - Wii (Digital).dat") == "WII"

    def test_wiiu_is_a_known_system_code(self):
        from shared.systems import ALL_CONSOLE_TYPES, SYSTEM_CHOICES, SYSTEM_CODES

        assert "WIIU" in SYSTEM_CODES
        assert "WIIU" in SYSTEM_CHOICES
        assert "WIIU" in ALL_CONSOLE_TYPES


@pytest.mark.skipif(not WIIU_DAT.is_file(), reason="Wii U DAT not present")
class TestWiiUNames:
    @pytest.fixture(autouse=True)
    def _load(self):
        game_names.load_libretro_dat_to_dicts(WII_DAT)
        game_names.load_libretro_dat_to_dicts(WIIU_DAT)

    def test_wiiu_codes_land_in_their_own_dict(self):
        assert game_names._wiiu_names, "Wii U DAT produced no entries"
        # Wii U product codes must NOT pollute the shared GC/Wii table —
        # a collision there would rename a GameCube save's game.
        assert "ADRP" not in game_names._wii_names

    def test_multi_line_rom_block_does_not_leak_into_the_name(self):
        name = game_names._wiiu_names.get("AMKE", "")
        assert name, "Mario Kart 8 missing from the Wii U DAT"
        assert not name.endswith((".wux", ".wud", ".iso"))

    def test_lookup_resolves_wiiu_rom_ids(self):
        got = game_names.lookup_names_typed(["WIIU_AMKE"])
        assert got["WIIU_AMKE"][1] == "WIIU"
        assert "Mario Kart 8" in got["WIIU_AMKE"][0]

    def test_lookup_resolves_vwii_save_ids(self):
        """WII_<code> is what the Wii U client sends for vWii NAND saves."""
        got = game_names.lookup_names_typed(["WII_RMCE"])
        assert got["WII_RMCE"][1] == "WII"
        assert "Mario Kart Wii" in got["WII_RMCE"][0]

    def test_gc_ids_still_resolve(self):
        game_names.load_libretro_dat_to_dicts(DATS / "Nintendo - GameCube.dat")
        got = game_names.lookup_names_typed(["GC_GALE"])
        assert got["GC_GALE"][1] == "GC"
        assert "Melee" in got["GC_GALE"][0]

    def test_wiiu_saves_are_not_resolved_by_product_code(self):
        """A Wii U *save* is a 16-hex title id, and its low word is NOT the
        ASCII product code (unlike vWii), so the DAT cannot name it — the
        client reads meta.xml instead.  Pinned so nobody 'fixes' it wrongly."""
        assert game_names.lookup_names_typed(["0005000010101D00"]) == {}
