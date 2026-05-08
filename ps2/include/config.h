#ifndef PS2SYNC_CONFIG_H
#define PS2SYNC_CONFIG_H

#include "common.h"

/* Config file at mc0:/3DSSYNC/CONFIG.TXT:
 *
 *   server_url=http://192.168.1.201:8000
 *   api_key=anything
 *   use_static_ip=true
 *   static_ip=192.168.1.95
 *   static_netmask=255.255.255.0
 *   static_gateway=192.168.1.1
 *
 * On first launch a default config is created and those defaults are used.
 */

bool config_load(SyncState *state, char *err_out, size_t err_size);
bool config_save(const SyncState *state);
void config_load_console_id(SyncState *state);

#endif /* PS2SYNC_CONFIG_H */
