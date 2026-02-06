from __future__ import annotations

import struct
import hashlib
from dataclasses import dataclass, field
from datetime import datetime, timezone

from pydantic import BaseModel, field_validator


BUNDLE_MAGIC = b"3DSS"
BUNDLE_VERSION = 1
BUNDLE_VERSION_COMPRESSED = 2


@dataclass
class BundleFile:
    path: str
    size: int
    sha256: bytes  # 32 bytes
    data: bytes = b""


@dataclass
class SaveBundle:
    title_id: int
    timestamp: int  # unix epoch
    files: list[BundleFile] = field(default_factory=list)

    @property
    def title_id_hex(self) -> str:
        return f"{self.title_id:016X}"

    @property
    def total_size(self) -> int:
        return sum(f.size for f in self.files)


@dataclass
class SaveMetadata:
    title_id: str
    name: str
    last_sync: str  # ISO 8601
    last_sync_source: str
    save_hash: str  # sha256 of the full bundle
    save_size: int
    file_count: int
    client_timestamp: int  # timestamp reported by the 3DS
    server_timestamp: str  # server wall-clock time at upload

    def to_dict(self) -> dict:
        return {
            "title_id": self.title_id,
            "name": self.name,
            "last_sync": self.last_sync,
            "last_sync_source": self.last_sync_source,
            "save_hash": self.save_hash,
            "save_size": self.save_size,
            "file_count": self.file_count,
            "client_timestamp": self.client_timestamp,
            "server_timestamp": self.server_timestamp,
        }


class TitleSyncInfo(BaseModel):
    """Metadata for a single title sent by the 3DS during sync."""
    title_id: str
    save_hash: str
    timestamp: int
    size: int
    last_synced_hash: str | None = None

    @field_validator("title_id")
    @classmethod
    def validate_title_id(cls, v: str) -> str:
        v = v.upper()
        if len(v) != 16 or not all(c in "0123456789ABCDEF" for c in v):
            raise ValueError("title_id must be 16 hex characters")
        return v


class SyncRequest(BaseModel):
    """Batch metadata from 3DS for sync planning."""
    titles: list[TitleSyncInfo]


class SyncPlan(BaseModel):
    """Server's response telling the 3DS what to do."""
    upload: list[str]    # title IDs where 3DS is newer -> 3DS should upload
    download: list[str]  # title IDs where server is newer -> 3DS should download
    conflict: list[str]  # title IDs where both changed -> needs user decision
    up_to_date: list[str]  # title IDs with matching hashes
    server_only: list[str]  # title IDs only on server -> 3DS should download
