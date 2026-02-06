"""Update checking endpoint - proxies GitHub releases for 3DS client."""

from fastapi import APIRouter
from pydantic import BaseModel
import httpx

router = APIRouter()

# GitHub repository info
# The server proxies GitHub releases API because 3DS has issues with GitHub's HTTPS
GITHUB_OWNER = "pepasjc"
GITHUB_REPO = "3ds_sync"
GITHUB_API_URL = f"https://api.github.com/repos/{GITHUB_OWNER}/{GITHUB_REPO}/releases/latest"


class UpdateInfo(BaseModel):
    available: bool
    current_version: str
    latest_version: str | None = None
    download_url: str | None = None
    changelog: str | None = None
    file_size: int | None = None


@router.get("/update/check")
async def check_update(current: str = "0.0.0") -> UpdateInfo:
    """Check if a newer version is available.

    Args:
        current: The client's current version (e.g., "0.1.0")

    Returns:
        Update info with download URL if available.
    """
    try:
        async with httpx.AsyncClient() as client:
            resp = await client.get(
                GITHUB_API_URL,
                headers={"Accept": "application/vnd.github.v3+json"},
                timeout=10.0,
            )

            if resp.status_code != 200:
                return UpdateInfo(available=False, current_version=current)

            data = resp.json()
            latest_version = data.get("tag_name", "").lstrip("v")

            # Find .cia asset
            download_url = None
            file_size = None
            for asset in data.get("assets", []):
                if asset["name"].endswith(".cia"):
                    download_url = asset["browser_download_url"]
                    file_size = asset["size"]
                    break

            # Compare versions (simple string comparison works for semver)
            is_newer = _compare_versions(latest_version, current) > 0

            return UpdateInfo(
                available=is_newer and download_url is not None,
                current_version=current,
                latest_version=latest_version,
                download_url=download_url,
                changelog=data.get("body", ""),
                file_size=file_size,
            )

    except Exception:
        # On any error, report no update available
        return UpdateInfo(available=False, current_version=current)


@router.get("/update/download")
async def proxy_download(url: str):
    """Proxy download from GitHub (3DS has HTTPS issues with GitHub).

    This streams the file to avoid loading it all into memory.
    """
    from fastapi.responses import StreamingResponse

    async def stream_download():
        async with httpx.AsyncClient() as client:
            async with client.stream("GET", url, follow_redirects=True, timeout=300.0) as resp:
                async for chunk in resp.aiter_bytes(chunk_size=8192):
                    yield chunk

    return StreamingResponse(
        stream_download(),
        media_type="application/octet-stream",
        headers={"Content-Disposition": "attachment; filename=3dssync.cia"}
    )


def _compare_versions(v1: str, v2: str) -> int:
    """Compare two version strings. Returns >0 if v1 > v2, <0 if v1 < v2, 0 if equal."""
    def parse(v: str) -> list[int]:
        try:
            return [int(x) for x in v.split(".")]
        except ValueError:
            return [0]

    p1, p2 = parse(v1), parse(v2)
    # Pad to same length
    while len(p1) < len(p2):
        p1.append(0)
    while len(p2) < len(p1):
        p2.append(0)

    for a, b in zip(p1, p2):
        if a > b:
            return 1
        if a < b:
            return -1
    return 0
