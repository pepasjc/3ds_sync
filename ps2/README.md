# ps2sync — PS2 Save Sync client

PlayStation 2 homebrew client for the Save Sync server.  Phase 1 ships a ROM
catalog browser + USB ISO installer.  Phase 2 (planned) adds Memcard PRO 2
save sync via the GameID protocol.

## Build environment (WSL)

Once-only install of the PS2 toolchain (~30-90 min on first build):

```bash
wsl bash -c "cd /mnt/e/projects/3dssync && bash install-ps2sdk-wsl.sh"
source ~/.bashrc
```

Builds `$PS2DEV` at `/usr/local/ps2dev`, sets `PS2SDK=$PS2DEV/ps2sdk`,
adds `ee-gcc` etc. to `PATH`.

## Build

```bash
wsl bash -c "export PS2DEV=/usr/local/ps2dev && \
  export PS2SDK=\$PS2DEV/ps2sdk && \
  export PATH=\$PS2DEV/bin:\$PS2DEV/ee/bin:\$PS2DEV/iop/bin:\$PS2SDK/bin:\$PATH && \
  cd /mnt/e/projects/3dssync/ps2 && make"
```

Output: `ps2sync.elf`.

## Install on PS2

1. Format a USB drive as FAT32 (MBR).
2. Copy `ps2sync.elf` to the root, or to `mass:/3dssync/ps2sync.elf`.
3. Create `mc0:/3DSSYNC/CONFIG.TXT`, or let the client create one with
   default settings on first launch if the memory card is writable:

   ```
   server_url=http://192.168.1.201:8000
   api_key=anything
   use_static_ip=true
   static_ip=192.168.1.95
   static_netmask=255.255.255.0
   static_gateway=192.168.1.1
   ```

4. Boot the ELF via uLaunchELF / FMCB / OPL ELF launcher.  The USB drive is
   only required for queue persistence and ROM downloads; catalog browsing can
   work without it once networking is up.

## Controls

| Button   | Action                                              |
|----------|-----------------------------------------------------|
| START    | Cycle view (ROMs → Downloads → Config)              |
| D-Pad    | Navigate list                                       |
| L/R      | Page up/down                                        |
| CROSS    | ROMs view: fetch catalog · Downloads view: run       |
| SQUARE   | ROMs: queue download · Downloads: remove entry      |
| TRIANGLE | ROMs: download immediately                          |
| CIRCLE   | Exit                                                |

## On-disk layout

```
mc0:/
└── 3DSSYNC/
    ├── CONFIG.TXT
    └── CONSOLEID.TXT

mass:/
├── 3dssync/
│   ├── downloads.dat
│   └── downloads/
├── DVD/
│   └── SLUS_213.71.God of War.iso
└── CD/
    └── SLUS_201.13.Some CD Game.iso
```

OPL picks games up automatically.  CD vs DVD subdir is chosen by the
catalog: ≤ 750 MB ⇒ CD, otherwise DVD.

## Networking

- The PS2 ethernet adapter (or built-in NIC on slim) must be present.
- Static IP is enabled by default.  Set `use_static_ip=false` or `dhcp=true`
  in `CONFIG.TXT` to use DHCP instead.
- Server URL must be a dotted-IP address (lwIP gethostbyname is not
  shipped in our build).

## Troubleshooting

- `USB not ready`: use a FAT32 drive with an MBR partition table and plug it
  in before booting the ELF. The client probes `mass:`, `mass0:`, `mass1:`,
  `mass2:`, and `mass3:` and creates the OPL folders on the detected root.
- `Config not found`: edit the generated `mc0:/3DSSYNC/CONFIG.TXT` and
  replace the sample IP/API key with your server values.
- `Network not ready (ip=no-link/no-dhcp/bad-static)`: the catalog fetch is
  blocked before HTTP; check the ethernet adapter, cable, and `CONFIG.TXT`
  static IP values.
- `Catalog fetch failed (HTTP 401, ...)`: the PS2 reached the server but
  `api_key` does not match `SYNC_API_KEY`.

## Status

- **Phase 1: ROM installer** — catalog browse, queue, resumable HTTP/1.0
  downloads to the detected USB root's `DVD/` or `CD/` folder.  Working in
  this branch.
- **Phase 2: MCP2 save sync** — not yet implemented.  Will use the
  GameID broadcast protocol to switch the MCP2 channel, then read the
  per-channel virtual memcard via libmc and POST it as a `.bin` save
  bundle to the server.
- **Phase 3: HDD support** — not yet implemented.  Will require apa /
  pfs IRX modules + per-game APA partitions in `hdl_dump` format.

## File map

```
include/
  common.h        shared paths + SyncState
  config.h        config loader
  network.h       ps2ip + NetMan + static IP / DHCP
  http.h          BSD-socket HTTP/1.0 client
  roms.h          catalog model + path resolution
  downloads.h     pause/resume manager
  ui.h            libdebug screen wrapper
  sha256.h        hash helper
source/
  main.c          IRX bootstrap + menu loop
  config.c
  network.c
  http.c
  roms.c
  downloads.c
  ui.c
  config.c
  sha256.c
tools/
  gen_irx_mods.sh embeds IRX blobs into the ELF at build time
```
