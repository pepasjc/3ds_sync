from fastapi import APIRouter

from app.services import storage

router = APIRouter()


@router.get("/titles")
async def list_titles():
    titles = storage.list_titles()
    return {"titles": titles}
