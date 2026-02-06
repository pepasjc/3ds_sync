from fastapi import APIRouter

from app.models.save import SyncPlan, SyncRequest
from app.services import storage

router = APIRouter()


@router.post("/sync")
async def sync(request: SyncRequest) -> SyncPlan:
    """Compare 3DS title metadata against server state and return a sync plan."""
    upload: list[str] = []
    download: list[str] = []
    conflict: list[str] = []
    up_to_date: list[str] = []

    client_title_ids = set()

    for title in request.titles:
        client_title_ids.add(title.title_id)
        server_meta = storage.get_metadata(title.title_id)

        if server_meta is None:
            # Server has no save for this title -> 3DS should upload
            upload.append(title.title_id)
            continue

        if title.save_hash == server_meta.save_hash:
            up_to_date.append(title.title_id)
            continue

        # Hashes differ -> use three-way comparison with last_synced_hash
        if title.last_synced_hash is not None:
            if title.last_synced_hash == server_meta.save_hash:
                # Server unchanged since last sync, only client changed -> upload
                upload.append(title.title_id)
            elif title.last_synced_hash == title.save_hash:
                # Client unchanged since last sync, only server changed -> download
                download.append(title.title_id)
            else:
                # Both changed since last sync -> conflict
                conflict.append(title.title_id)
        else:
            # No sync history -> can't determine direction safely
            conflict.append(title.title_id)

    # Find titles that exist only on the server
    server_only: list[str] = []
    for meta in storage.list_titles():
        tid = meta["title_id"]
        if tid not in client_title_ids:
            server_only.append(tid)

    return SyncPlan(
        upload=upload,
        download=download,
        conflict=conflict,
        up_to_date=up_to_date,
        server_only=server_only,
    )
