# 3DS Save Sync

Sync Nintendo 3DS save files between multiple CFW consoles via a PC server over local WiFi.

## Features

- **Multi-console sync**: Keep saves in sync across multiple 3DS consoles
- **Three-way hash sync**: Automatically detects which side changed to avoid conflicts
- **Cartridge support**: Sync saves between digital and physical copies of the same game
- **Compression**: zlib compression allows syncing saves up to ~1-2MB (compressed to 448KB limit)
- **Game name lookup**: Shows actual game names instead of title IDs (4500+ games in database)
- **Conflict detection**: Highlights conflicting saves in red for manual resolution
- **Save history**: Server keeps previous versions of saves for recovery
- **Auto-update**: Check for and install updates directly from the 3DS (no FBI needed)

## Requirements

### Server (PC)
- Python 3.11+
- [uv](https://github.com/astral-sh/uv) package manager

### Client (3DS)
- Custom firmware (Luma3DS recommended)
- Homebrew Launcher
- WiFi connection to the same network as the server

## Quick Start

### 1. Start the Server

```bash
cd server
uv sync                    # Install dependencies
uv run python run.py       # Start server on port 8000
```

The server will display `Loaded XXXX game names from database` on startup.

### 2. Configure the 3DS Client

Create `sdmc:/3ds/3dssync/config.txt` on your SD card:

```
server_url=http://192.168.1.100:8000
api_key=your-secret-key
```

Replace `192.168.1.100` with your PC's local IP address.

### 3. Set Server API Key

Set the same API key on the server via environment variable:

```bash
# Windows
set SYNC_API_KEY=your-secret-key
uv run python run.py

# Linux/Mac
SYNC_API_KEY=your-secret-key uv run python run.py
```

### 4. Install the Client

Copy `client/3dssync.3dsx` to `sdmc:/3ds/` on your SD card and launch via Homebrew Launcher.

## Usage

### Controls

| Button | Action |
|--------|--------|
| D-Pad | Navigate title list |
| A | Upload selected save to server |
| B | Download save from server |
| X | Sync all SD titles automatically |
| Y | Rescan titles |
| SELECT | Check for updates |
| START | Exit |

### Title List Colors

- **White**: Normal SD game
- **Yellow**: Currently selected
- **Cyan**: Cartridge game (manual A/B sync only)
- **Red**: Save conflict (needs manual resolution)

### Syncing Workflow

**First time setup:**
1. On your "main" 3DS, press X to sync all - this uploads all saves to the server

**Regular use:**
1. Play games on any 3DS
2. Before switching consoles, press X to sync all
3. On the other console, press X to sync all - changed saves download automatically

**Resolving conflicts:**
If both consoles changed the same save:
1. The save shows in red
2. Select it and press A to keep local version, or B to use server version

### Cartridge Saves

Cartridge games appear in cyan and are excluded from "Sync All" to prevent accidental overwrites.

To sync a cartridge save:
1. Select the cartridge game (cyan, "Card" prefix)
2. Press A to upload to server, or B to download from server

This lets you transfer saves between:
- Physical cartridge ↔ Digital copy
- Cartridge on one console ↔ Cartridge on another console

### Auto-Update

Press SELECT to check for new versions. If an update is available:
1. Press A to download and install directly (no FBI needed)
2. Restart the application after installation

Updates are downloaded from the server, which proxies GitHub releases.

## Server Configuration

Environment variables (prefix with `SYNC_`):

| Variable | Default | Description |
|----------|---------|-------------|
| `SYNC_API_KEY` | `dev-key-change-me` | API key for authentication |
| `SYNC_SAVE_DIR` | `./saves` | Directory to store saves |
| `SYNC_HOST` | `0.0.0.0` | Server bind address |
| `SYNC_PORT` | `8000` | Server port |
| `SYNC_MAX_HISTORY_VERSIONS` | `5` | Number of save versions to keep |

## Building from Source

### Server

```bash
cd server
uv sync
uv run pytest tests/ -v  # Run tests
```

### Client (.3dsx)

Requires [devkitPro](https://devkitpro.org/) with 3DS development tools.

```bash
# Install zlib for 3DS (one time)
pacman -S 3ds-zlib

# Build
cd client
make
```

The output is `3dssync.3dsx` (launch via Homebrew Launcher).

### Client (.cia)

To build an installable CIA (appears on home menu):

1. Download additional tools and place in `C:\devkitPro\tools\bin\` (or `client/` directory):
   - [makerom](https://github.com/3DSGuy/Project_CTR/releases) - get `makerom-*-win_x86_64.zip`
   - [bannertool](https://github.com/Steveice10/bannertool/releases) - get `bannertool.zip`

2. Build:
```bash
cd client
make cia
```

The output is `3dssync.cia` (~210KB) - install with FBI or other CIA installer.

Note: The required files (`banner_img.png`, `silent.wav`, `icon.png`, `cia.rsf`) are included in the repository.

## Technical Details

### Sync Protocol

1. Client scans local saves and computes SHA-256 hashes
2. Client sends metadata to `POST /api/v1/sync` with:
   - Current hash
   - Last synced hash (stored locally per title)
   - Save size
3. Server compares using three-way logic:
   - Hashes match → up to date
   - Only client changed (last_synced == server) → upload
   - Only server changed (last_synced == client) → download
   - Both changed → conflict
4. Client executes the sync plan (uploads/downloads as needed)
5. After successful sync, client stores the new hash as "last synced"

### Bundle Format

Saves are transferred as compressed binary bundles:

```
Header (28 bytes):
  [4B]  Magic: "3DSS"
  [4B]  Version: 2
  [8B]  Title ID (big-endian)
  [4B]  Timestamp (unix epoch)
  [4B]  File count
  [4B]  Uncompressed payload size

Payload (zlib compressed):
  For each file:
    [2B]  Path length
    [NB]  Path (UTF-8)
    [4B]  File size
    [32B] SHA-256 hash
  For each file:
    [NB]  File data
```

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/status` | GET | Health check (no auth required) |
| `/api/v1/titles` | GET | List all titles on server |
| `/api/v1/titles/names` | POST | Look up game names by product code |
| `/api/v1/saves/{title_id}` | GET | Download save bundle |
| `/api/v1/saves/{title_id}` | POST | Upload save bundle |
| `/api/v1/saves/{title_id}/meta` | GET | Get save metadata |
| `/api/v1/sync` | POST | Get sync plan for multiple titles |

All endpoints except `/status` require `X-API-Key` header.

## License

MIT

## Acknowledgments

- [devkitPro](https://devkitpro.org/) for the 3DS development toolchain
- [libctru](https://github.com/devkitPro/libctru) for 3DS homebrew libraries
- [3dstdb](https://www.3dsdb.com/) for the game names database
