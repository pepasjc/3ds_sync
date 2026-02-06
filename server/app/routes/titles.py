from fastapi import APIRouter
from pydantic import BaseModel

from app.services import storage, game_names

router = APIRouter()


class NameLookupRequest(BaseModel):
    codes: list[str]


@router.get("/titles")
async def list_titles():
    titles = storage.list_titles()
    return {"titles": titles}


@router.post("/titles/names")
async def lookup_game_names(request: NameLookupRequest):
    """Look up game names for product codes.

    Accepts codes in formats:
    - Full: CTR-P-BRBE
    - Short: BRBE (4-char code)

    Returns: {"names": {"CTR-P-BRBE": "Resident Evil Revelations", ...}}
    """
    names = game_names.lookup_names(request.codes)
    return {"names": names}
