from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.config import settings
from app.middleware.auth import APIKeyMiddleware
from app.routes import saves, status, sync, titles
from app.services import game_names


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings.save_dir.mkdir(parents=True, exist_ok=True)
    # Load game names database
    count = game_names.load_database()
    print(f"Loaded {count} game names from database")
    yield


def create_app() -> FastAPI:
    app = FastAPI(title="3DS Save Sync", version="1.0.0", lifespan=lifespan)

    app.add_middleware(APIKeyMiddleware)

    app.include_router(status.router, prefix="/api/v1")
    app.include_router(titles.router, prefix="/api/v1")
    app.include_router(saves.router, prefix="/api/v1")
    app.include_router(sync.router, prefix="/api/v1")

    return app


app = create_app()
