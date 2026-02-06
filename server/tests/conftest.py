import shutil
import tempfile
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app.config import settings


@pytest.fixture(autouse=True)
def tmp_save_dir(tmp_path):
    """Use a temporary directory for saves during tests."""
    original = settings.save_dir
    settings.save_dir = tmp_path / "saves"
    settings.save_dir.mkdir()
    yield settings.save_dir
    settings.save_dir = original


@pytest.fixture()
def client():
    # Import here so settings are patched first
    from app.main import create_app

    app = create_app()
    return TestClient(app)


@pytest.fixture()
def api_key():
    return settings.api_key


@pytest.fixture()
def auth_headers(api_key):
    return {"X-API-Key": api_key}
