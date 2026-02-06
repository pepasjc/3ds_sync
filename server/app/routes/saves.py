import hashlib
import re

from fastapi import APIRouter, HTTPException, Query, Request, Response

from app.models.save import BundleFile, SaveBundle
from app.services import storage
from app.services.bundle import BundleError, create_bundle, parse_bundle

router = APIRouter()

_TITLE_ID_RE = re.compile(r"^[0-9A-Fa-f]{16}$")


def _validate_title_id(title_id: str) -> str:
    """Validate and normalize title ID to uppercase hex."""
    if not _TITLE_ID_RE.match(title_id):
        raise HTTPException(status_code=400, detail="Invalid title ID format")
    return title_id.upper()


@router.get("/saves/{title_id}/meta")
async def get_save_meta(title_id: str):
    title_id = _validate_title_id(title_id)
    meta = storage.get_metadata(title_id)
    if meta is None:
        raise HTTPException(status_code=404, detail="No save found for this title")
    return meta.to_dict()


@router.get("/saves/{title_id}")
async def download_save(title_id: str):
    title_id = _validate_title_id(title_id)
    meta = storage.get_metadata(title_id)
    if meta is None:
        raise HTTPException(status_code=404, detail="No save found for this title")

    files = storage.load_save_files(title_id)
    if files is None:
        raise HTTPException(status_code=404, detail="Save data missing on disk")

    # Build a bundle from stored files
    bundle_files = []
    for path, data in files:
        bundle_files.append(
            BundleFile(
                path=path,
                size=len(data),
                sha256=hashlib.sha256(data).digest(),
                data=data,
            )
        )

    bundle = SaveBundle(
        title_id=int(title_id, 16),
        timestamp=meta.client_timestamp,
        files=bundle_files,
    )

    bundle_data = create_bundle(bundle)
    return Response(
        content=bundle_data,
        media_type="application/octet-stream",
        headers={
            "X-Save-Timestamp": str(meta.client_timestamp),
            "X-Save-Hash": meta.save_hash,
            "X-Save-Size": str(meta.save_size),
        },
    )


@router.post("/saves/{title_id}")
async def upload_save(
    title_id: str,
    request: Request,
    force: bool = Query(False),
    source: str = Query("3ds"),
):
    title_id = _validate_title_id(title_id)

    # Get console ID from header
    console_id = request.headers.get("X-Console-ID", "")

    body = await request.body()
    if not body:
        raise HTTPException(status_code=400, detail="Empty request body")

    try:
        bundle = parse_bundle(body)
    except BundleError as e:
        raise HTTPException(status_code=400, detail=f"Invalid bundle: {e}")

    # Verify title ID in URL matches bundle
    if bundle.title_id_hex != title_id:
        raise HTTPException(
            status_code=400,
            detail=f"Title ID mismatch: URL={title_id}, bundle={bundle.title_id_hex}",
        )

    # Conflict check
    if not force:
        existing = storage.get_metadata(title_id)
        if existing and existing.client_timestamp >= bundle.timestamp:
            raise HTTPException(
                status_code=409,
                detail="Server has a newer or equal save. Use ?force=true to override.",
                headers={
                    "X-Server-Timestamp": str(existing.client_timestamp),
                    "X-Server-Hash": existing.save_hash,
                },
            )

    meta = storage.store_save(bundle, source=source, console_id=console_id)
    return {
        "status": "ok",
        "timestamp": meta.last_sync,
        "sha256": meta.save_hash,
    }
