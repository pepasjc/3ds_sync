#include "network.h"
#include "http.h"
#include "saves.h"
#include <dswifi9.h>
#include <wfc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>

// WiFi connection state
static bool wifi_connected = false;

// WiFi event handler
void wifi_event_handler(int event) {
    // switch (event) {
    //     case WIFI_EVENT_CONNECT:
    //         printf("Connected to WiFi\n");
    //         wifi_connected = true;
    //         break;
    //     case WIFI_EVENT_DISCONNECT:
    //         printf("WiFi disconnected\n");
    //         wifi_connected = false;
    //         break;
    //     case WIFI_EVENT_ERROR:
    //         printf("WiFi error\n");
    //         wifi_connected = false;
    //         break;
    // }
}

int network_init(SyncState *state) {
    iprintf("Connecting WiFi...\n");
    
    // Try DSi/3DS firmware settings first (works on DSi/3DS, not on DS Lite)
    if (Wifi_InitDefault(WFC_CONNECT)) {
        wifi_connected = true;
        iprintf("WFC connected!\n");
        
        struct in_addr ip, gateway, mask, dns1, dns2;
        ip = Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
        iprintf("IP: %s\n", inet_ntoa(ip));
        
        return 0;
    }
    
    // If no firmware settings and SSID is configured, try manual connection
    if (state && state->wifi_ssid[0] != '\0') {
        iprintf("Using manual config\n");
        iprintf("SSID: %s\n", state->wifi_ssid);
        
        // Initialize WiFi hardware without connecting
        if (!Wifi_InitDefault(INIT_ONLY)) {
            iprintf("WiFi init failed\n");
            return -1;
        }
        
        // Set up the WFC scan filter to find our specific AP
        static const WlanBssScanFilter filter = {
            .channel_mask = UINT32_MAX,  // Scan all channels
            .target_bssid = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },  // Any BSSID
        };
        
        // Begin scan
        if (!wfcBeginScan(&filter)) {
            iprintf("Scan failed to start\n");
            return -1;
        }
        
        iprintf("Scanning...\n");
        
        // Wait for scan to complete
        unsigned count = 0;
        WlanBssDesc* aplist = NULL;
        
        for (int timeout = 0; timeout < 600 && !aplist; timeout++) {  // 60 seconds
            aplist = wfcGetScanBssList(&count);
            if (!aplist) swiWaitForVBlank();
        }
        
        if (!aplist || !count) {
            iprintf("No APs found at all!\n");
            iprintf("Check:\n");
            iprintf("- WiFi on channel 1-11\n");
            iprintf("- Broadcasting SSID\n");
            iprintf("Press START to continue\n");
            return -1;
        }
        
        iprintf("Found %u AP(s)\n", count);
        
        // Find our AP by SSID
        WlanBssDesc* target_ap = NULL;
        for (unsigned i = 0; i < count; i++) {
            if (aplist[i].ssid_len == strlen(state->wifi_ssid) &&
                memcmp(aplist[i].ssid, state->wifi_ssid, aplist[i].ssid_len) == 0) {
                target_ap = &aplist[i];
                iprintf("Found '%s' on ch %u\n", 
                    state->wifi_ssid, aplist[i].channel);
                break;
            }
        }
        
        if (!target_ap) {
            iprintf("'%s' not found\n", state->wifi_ssid);
            iprintf("Available APs:\n");
            for (unsigned i = 0; i < count && i < 5; i++) {
                iprintf("  %.*s (ch %u)\n", 
                    aplist[i].ssid_len, aplist[i].ssid,
                    aplist[i].channel);
            }
            if (count > 5) iprintf("  ...and %u more\n", count - 5);
            iprintf("Press START to continue\n");
            return -1;
        }
        
        iprintf("AP found! Connecting...\n");
        
        // Set up auth data
        static WlanAuthData auth;
        memset(&auth, 0, sizeof(auth));
        
        size_t key_len = strlen(state->wifi_wep_key);
        
        if (key_len == 0) {
            // Open network (no encryption)
            iprintf("Open network\n");
            target_ap->auth_type = WlanBssAuthType_Open;
        } else {
            // WEP encrypted network - determine type based on key length
            if (key_len == WLAN_WEP_40_LEN) {
                target_ap->auth_type = WlanBssAuthType_WEP_40;
            } else if (key_len == WLAN_WEP_104_LEN) {
                target_ap->auth_type = WlanBssAuthType_WEP_104;
            } else if (key_len == WLAN_WEP_128_LEN) {
                target_ap->auth_type = WlanBssAuthType_WEP_128;
            } else {
                iprintf("Invalid WEP key length\n");
                iprintf("Need 5, 13, or 16 chars\n");
                iprintf("or leave blank for open\n");
                return -1;
            }
            
            memcpy(auth.wep_key, state->wifi_wep_key, key_len);
        }
        
        // Begin connection
        if (!wfcBeginConnect(target_ap, &auth)) {
            iprintf("Connect failed\n");
            return -1;
        }
        
        // Wait for connection
        bool is_connected = false;
        for (int i = 0; i < 600; i++) {  // 60 seconds max
            int status = Wifi_AssocStatus();
            
            if (status == ASSOCSTATUS_ASSOCIATED) {
                is_connected = true;
                wifi_connected = true;
                break;
            }
            
            if (status == ASSOCSTATUS_DISCONNECTED) {
                iprintf("Connection failed\n");
                return -1;
            }
            
            swiWaitForVBlank();
        }
        
        if (!is_connected) {
            iprintf("Connection timeout\n");
            return -1;
        }
        
        iprintf("Connected!\n");
        
        // Show IP info
        unsigned ip = Wifi_GetIP();
        iprintf("IP: %u.%u.%u.%u\n", 
            ip & 0xff, (ip >> 8) & 0xff, 
            (ip >> 16) & 0xff, (ip >> 24) & 0xff);
        
        return 0;
    }
    
    iprintf("WiFi unavailable\n");
    iprintf("Configure in DS System\n");
    iprintf("or add to config.txt:\n");
    iprintf("wifi_ssid=YourSSID\n");
    iprintf("wifi_wep_key=YourKey\n");
    return -1;
}

// Helper: build sync request payload
static uint8_t* build_sync_payload(SyncState *state, size_t *payload_size) {
    // TODO: Create JSON or binary payload with title metadata
    // Format: array of {title_id, local_hash, last_synced_hash, size}
    *payload_size = 0;
    return NULL;
}

int network_sync(SyncState *state) {
    if (!wifi_connected) {
        printf("Not connected to WiFi\n");
        return -1;
    }
    
    // Build sync request
    size_t payload_size = 0;
    uint8_t *payload = build_sync_payload(state, &payload_size);
    
    // Build URL
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v1/sync", state->server_url);
    
    printf("Syncing to %s...\n", url);
    
    // POST to sync endpoint
    HttpResponse response = http_request(url, HTTP_POST, state->api_key, payload, payload_size);
    
    if (!response.success) {
        printf("Sync request failed (HTTP %d)\n", response.status_code);
        http_response_free(&response);
        if (payload) free(payload);
        return -1;
    }
    
    printf("Sync complete\n");
    
    // TODO: Parse response and execute sync plan
    // Response contains list of {title_id, action: UPLOAD|DOWNLOAD|CONFLICT}
    
    http_response_free(&response);
    if (payload) free(payload);
    
    return 0;
}

int network_upload(SyncState *state, int title_idx) {
    if (!wifi_connected) {
        iprintf("Not connected to WiFi\n");
        return -1;
    }
    
    if (title_idx >= state->num_titles) {
        return -1;
    }
    
    Title *title = &state->titles[title_idx];
    
    // Ensure hash is calculated
    if (!title->hash_calculated) {
        iprintf("Calculating hash...\n");
        if (saves_compute_hash(title->save_path, title->hash) != 0) {
            iprintf("Failed to read save!\n");
            return -1;
        }
        title->hash_calculated = true;
    }
    
    // Read save file
    FILE *f = fopen(title->save_path, "rb");
    if (!f) {
        iprintf("Failed to open save file!\n");
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *file_data = (uint8_t*)malloc(file_size);
    if (!file_data) {
        fclose(f);
        iprintf("Out of memory!\n");
        return -1;
    }
    
    size_t read_bytes = fread(file_data, 1, file_size, f);
    fclose(f);
    
    if (read_bytes != file_size) {
        free(file_data);
        iprintf("Failed to read save!\n");
        return -1;
    }
    
    // Build URL - use filename (game name) as identifier
    char url[512];
    snprintf(url, sizeof(url), "%s/api/saves/%s",
        state->server_url,
        title->game_name);
    
    iprintf("=== Upload Debug ===\n");
    iprintf("Server: %s\n", state->server_url);
    iprintf("Game: %s\n", title->game_name);
    iprintf("URL: %s\n", url);
    iprintf("Size: %ld bytes\n", file_size);
    iprintf("API Key: %.10s...\n\n", state->api_key);
    
    iprintf("Sending POST...\n");
    
    // POST save data
    HttpResponse response = http_request(url, HTTP_POST, state->api_key, file_data, file_size);
    
    free(file_data);
    
    if (!response.success) {
        iprintf("HTTP %d\n", response.status_code);
        http_response_free(&response);
        return -1;
    }
    
    http_response_free(&response);
    return 0;
}

int network_download(SyncState *state, int title_idx) {
    if (!wifi_connected) {
        iprintf("Not connected to WiFi\n");
        return -1;
    }
    
    if (title_idx >= state->num_titles) {
        return -1;
    }
    
    Title *title = &state->titles[title_idx];
    
    // Build URL - use filename (game name) as identifier
    char url[512];
    snprintf(url, sizeof(url), "%s/api/saves/%s",
        state->server_url,
        title->game_name);
    
    iprintf("GET %s\n", url);
    
    // GET save data
    HttpResponse response = http_request(url, HTTP_GET, state->api_key, NULL, 0);
    
    if (!response.success) {
        iprintf("HTTP %d\n", response.status_code);
        http_response_free(&response);
        return -1;
    }
    
    // Write to file
    FILE *f = fopen(title->save_path, "wb");
    if (!f) {
        iprintf("Failed to open file!\n");
        http_response_free(&response);
        return -1;
    }
    
    size_t written = fwrite(response.body, 1, response.body_size, f);
    fclose(f);
    
    if (written != response.body_size) {
        iprintf("Write failed!\n");
        http_response_free(&response);
        return -1;
    }
    
    iprintf("Wrote %zu bytes\n", written);
    
    // Recalculate hash for downloaded file
    if (saves_compute_hash(title->save_path, title->hash) == 0) {
        title->hash_calculated = true;
    }
    
    http_response_free(&response);
    return 0;
}

void network_cleanup(void) {
    http_cleanup();
    // Wifi_Disconnect();
    wifi_connected = false;
}
