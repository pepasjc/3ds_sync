from fastapi import APIRouter

from app.services import storage

router = APIRouter()


@router.get("/status")
async def get_status():
    titles = storage.list_titles()
    return {
        "status": "ok",
        "version": "1.0.0",
        "save_count": len(titles),
    }
