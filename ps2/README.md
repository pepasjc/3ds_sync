# ps2sync — PS2 Save Sync client

PlayStation 2 homebrew client for the Save Sync server.  Phase 1 ships a ROM
catalog browser and installer for USB mass storage or classic PS2 fat internal
HDDs using APA/HDLoader partitions.  Phase 2 (planned) adds Memcard PRO 2 save
sync via the GameID protocol.

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

1. Format a USB drive as FAT32 (MBR), or use Config → `TRIANGLE` twice on the
   PS2 to format a fat internal HDD as APA for OPL HDD mode.
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
   storage=auto
   ```

4. Boot the ELF via uLaunchELF / FMCB / OPL ELF launcher.  Storage is only
   required for queue persistence and ROM downloads; catalog browsing can work
   without it once networking is up.

`storage` controls where ROM installs go:

- `auto` tries APA/HDLoader on `hdd0:` first, then falls back to USB or BDM
  mass storage if no APA HDD is ready.
- `hdd` uses the PS2 fat internal HDD in classic APA/HDLoader mode.
- `usb` uses folder-based OPL ISO installs on `mass:/DVD` and `mass:/CD`.

The Config view also has an internal HDD formatter: press `TRIANGLE` once for
the warning, then `TRIANGLE` again to format. This is destructive. It formats
the PS2 internal HDD as APA/PFS, creates the standard PS2 system partitions,
and creates a `+OPL` common partition if one is missing. This is the classic
PS2 HDD format used by OPL's HDD mode. HDLoader downloads are written as
`PP.<SERIAL>..<TITLE>` APA partitions with OPL-compatible game metadata.

## Controls

| Button   | Action                                              |
|----------|-----------------------------------------------------|
| START    | Cycle view (ROMs → Local → Downloads → Config)      |
| D-Pad    | Navigate list                                       |
| Left/Right | Config: choose storage (`auto`, `usb`, `hdd`)     |
| L/R      | Page up/down                                        |
| CROSS    | ROMs view: fetch catalog · Downloads view: run       |
| SQUARE   | ROMs: queue download · Downloads: remove entry      |
| TRIANGLE | ROMs: download now · Config: format APA HDD         |
| CIRCLE   | Exit                                                |

## On-disk layout

```
mc0:/
└── 3DSSYNC/
    ├── CONFIG.TXT
    ├── CONSOLEID.TXT
    └── HDL_DOWNLOADS.DAT   (queue used by APA/HDLoader mode)

mass:/  (USB or internal HDD BDM FAT/exFAT)
├── 3dssync/
│   ├── downloads.dat
│   └── downloads/
├── DVD/
│   └── SLUS_213.71.God of War.iso
└── CD/
    └── SLUS_201.13.Some CD Game.iso

hdd0:  (APA/HDLoader mode)
└── PP.SLUS-21371..GOD_OF_WAR   (APA game partition)
```

OPL picks games up automatically in either mode.  USB uses `DVD/` and `CD/`
folders; HDD mode uses the HDLoader partition list.  HDLoader installs are
rewritten from the beginning if interrupted so the APA table is not left with a
half-valid game entry.

## Networking

- The PS2 ethernet adapter (or built-in NIC on slim) must be present.
- Static IP is enabled by default.  Set `use_static_ip=false` or `dhcp=true`
  in `CONFIG.TXT` to use DHCP instead.
- Server URL must be a dotted-IP address (lwIP gethostbyname is not
  shipped in our build).

## Troubleshooting

- `Storage not ready`: check `storage=` in `CONFIG.TXT`. For USB, use a FAT32
  drive with an MBR partition table and plug it in before booting the ELF. For
  internal HDD, use Config → `TRIANGLE` twice to format APA/PFS, then relaunch
  with `storage=hdd` or `storage=auto`. USB mode probes `mass:`, `mass0:`,
  `mass1:`, `mass2:`, and `mass3:` and creates the OPL folders on the detected
  root.
- `Config not found`: edit the generated `mc0:/3DSSYNC/CONFIG.TXT` and
  replace the sample IP/API key with your server values.
- `Network not ready (ip=no-link/no-dhcp/bad-static)`: the catalog fetch is
  blocked before HTTP; check the ethernet adapter, cable, and `CONFIG.TXT`
  static IP values.
- `Catalog fetch failed (HTTP 401, ...)`: the PS2 reached the server but
  `api_key` does not match `SYNC_API_KEY`.

## Status

- **Phase 1: ROM installer** — catalog browse, queue, resumable HTTP/1.0
  downloads to USB `DVD/`/`CD/` folders, plus classic APA/HDLoader partition
  installs on PS2 fat internal HDDs.
- **Phase 2: MCP2 save sync** — not yet implemented.  Will use the
  GameID broadcast protocol to switch the MCP2 channel, then read the
  per-channel virtual memcard via libmc and POST it as a `.bin` save
  bundle to the server.
- **Phase 3: APA/HDLoader HDD installs** — implemented for OPL-compatible
  partition naming, APA partition creation, local HDL scan/delete, and direct
  HTTP streaming into HDD sectors.

## File map

```
include/
  common.h        shared paths + SyncState
  config.h        config loader
  network.h       ps2ip + NetMan + static IP / DHCP
  http.h          BSD-socket HTTP/1.0 client
  roms.h          catalog model + path resolution
  downloads.h     pause/resume manager
  hdl.h           APA/HDLoader installer
  ui.h            libdebug screen wrapper
  sha256.h        hash helper
source/
  main.c          IRX bootstrap + menu loop
  config.c
  network.c
  http.c
  roms.c
  downloads.c
  hdl.c
  ui.c
  config.c
  sha256.c
tools/
  gen_irx_mods.sh embeds IRX blobs into the ELF at build time
```
