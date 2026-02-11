# NDS Save Sync Client (POC)

Proof of concept for a Nintendo DS homebrew client that syncs saves with the server, similar to the 3DS client.

## Architecture

The DS client is structured similar to the 3DS client:

- **main.c** - Main loop, input handling, app lifecycle
- **config.c** - Configuration management (server URL, API key)
- **saves.c** - Save file enumeration and I/O
- **network.c** - HTTP communication with server
- **ui.c** - Screen rendering and controls
- **common.h** - Shared structures and constants

## TODO List

### High Priority
- [ ] Bundle format implementation (compress/decompress saves)
- [ ] Sync request payload generation and parsing
- [ ] Save enumeration from inserted cartridge
- [ ] SHA-256 hash computation
- [ ] Cartridge save I/O (EEPROM, SRAM, Flash, FRAM)

### Medium Priority
- [ ] Flashcard support (if applicable)
- [ ] Game name database lookup
- [ ] Conflict detection and resolution
- [ ] Config menu implementation
- [ ] Input handling refinement
- [ ] Response body streaming (for large saves)

### Lower Priority
- [ ] Batch operations
- [ ] Save history/recovery
- [ ] Auto-update mechanism
- [ ] WiFi auto-connection and auto-reconnect

## Hardware Support

### Cartridge Access
- **DS Phat/Lite**: Cartridge slots for DS and GBA games
- **DSi/DSi XL**: Only DS slot (no GBA compatibility)
- **DSi**: Native SD card support (alternative to cartridge)

Save types supported:
- EEPROM (4KB, 64KB)
- SRAM (256KB)
- Flash (256KB, 512KB, 1MB)
- FRAM (256KB)

### WiFi
- DS Phat: Requires DS Wireless Adapter
- DS Lite: Built-in WiFi
- DSi/DSi XL: Built-in WiFi

## Build Requirements

```bash
# devkitARM with DS support
pacman -S nds-dev

# Build
cd ds
make
```

Output: `ndssyncd.nds` (run via flashcard or emulator)

## Current Limitations

1. **No cartridge I/O yet** - Save read/write is stubbed; requires cartridge hardware access
2. **Simple HTTP implementation** - Single-request model, no streaming for large saves
3. **No response body streaming** - Large downloads may need chunked handling
4. **Minimal UI** - POC uses basic text console output

## Implementation Details

### WiFi (`network.c`, `http.c`)
- Uses **dswifi** library for WiFi connectivity
- Event-based WiFi handler for connection state
- Lightweight HTTP/1.0 client using sockets
- Supports GET/POST/PUT methods with X-API-Key header
- Auto-parses HTTP status codes and response bodies

### HTTP Client (`http.c`)
- URL parsing (host, port, path extraction)
- DNS lookup with gethostbyname
- Socket-based connection management
- Simple request/response handling
- Supports optional body data (for POST/PUT)

## Next Steps

1. ~~Integrate dswifi library for WiFi connectivity~~ ✓
2. Implement bundle format (zlib compression, header structure)
3. Parse sync response and determine upload/download actions
4. Create save enumeration from cartridge
5. Implement SHA-256 hashing
6. Add cartridge save I/O
7. Improve UI for dual-screen presentation

## References

- [libnds](https://devkitpro.org/) - DS homebrew library
- [dswifi](https://github.com/devkitPro/dswifi) - WiFi library for DS
- [libfat](https://github.com/devkitPro/libfat) - Filesystem library
