#include "config.h"
#include <sys/stat.h>

// Generate a random 16-char hex ID using 3DS random number generator
static void generate_console_id(char *out) {
    u8 random[8];
    // Use PS service for random data
    PS_GenerateRandomBytes(random, 8);

    for (int i = 0; i < 8; i++) {
        snprintf(out + (i * 2), 3, "%02X", random[i]);
    }
    out[16] = '\0';
}

// Load or generate console ID
static void load_or_generate_console_id(AppConfig *config) {
    FILE *f = fopen(CONSOLE_ID_PATH, "r");
    if (f) {
        // Read existing ID
        if (fgets(config->console_id, sizeof(config->console_id), f)) {
            // Trim any whitespace
            char *end = config->console_id + strlen(config->console_id) - 1;
            while (end > config->console_id && (*end == '\r' || *end == '\n' || *end == ' '))
                *end-- = '\0';
        }
        fclose(f);

        // Validate it's 16 hex chars
        if (strlen(config->console_id) == 16) {
            return;  // Valid ID loaded
        }
    }

    // Generate new ID
    generate_console_id(config->console_id);

    // Ensure directory exists
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/3dssync", 0777);

    // Save it
    f = fopen(CONSOLE_ID_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", config->console_id);
        fclose(f);
    }
}

// Trim leading/trailing whitespace in-place
static void trim(char *str) {
    // Leading
    char *start = str;
    while (*start == ' ' || *start == '\t') start++;
    if (start != str) memmove(str, start, strlen(start) + 1);

    // Trailing
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
}

// Skip UTF-8 BOM if present at start of file
static void skip_bom(char *str) {
    if ((unsigned char)str[0] == 0xEF &&
        (unsigned char)str[1] == 0xBB &&
        (unsigned char)str[2] == 0xBF) {
        memmove(str, str + 3, strlen(str + 3) + 1);
    }
}

bool config_load(AppConfig *config, char *error_out, int error_size) {
    memset(config, 0, sizeof(AppConfig));

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        snprintf(error_out, error_size,
            "Could not open config file:\n  %s\n\n"
            "Make sure the file exists on your SD card.",
            CONFIG_PATH);
        return false;
    }

    char line[512];
    bool first_line = true;
    while (fgets(line, sizeof(line), f)) {
        if (first_line) {
            skip_bom(line);
            first_line = false;
        }

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(key, "server_url") == 0) {
            strncpy(config->server_url, val, MAX_URL_LEN - 1);
        } else if (strcmp(key, "api_key") == 0) {
            strncpy(config->api_key, val, MAX_API_KEY_LEN - 1);
        }
    }

    fclose(f);

    // Validate required fields with specific error
    if (config->server_url[0] == '\0' && config->api_key[0] == '\0') {
        snprintf(error_out, error_size,
            "Config file found but no valid keys.\n\n"
            "Expected format:\n"
            "  server_url=http://<ip>:8000\n"
            "  api_key=<your-key>");
        return false;
    }
    if (config->server_url[0] == '\0') {
        snprintf(error_out, error_size, "Config missing 'server_url' field.");
        return false;
    }
    if (config->api_key[0] == '\0') {
        snprintf(error_out, error_size, "Config missing 'api_key' field.");
        return false;
    }

    // Load or generate console ID
    load_or_generate_console_id(config);

    return true;
}

bool config_save(const AppConfig *config) {
    // Ensure directory exists
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/3dssync", 0777);

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return false;

    fprintf(f, "# 3DS Save Sync Configuration\n");
    fprintf(f, "server_url=%s\n", config->server_url);
    fprintf(f, "api_key=%s\n", config->api_key);

    fclose(f);
    return true;
}

bool config_edit_field(const char *hint, char *buffer, int max_len) {
    // Use static to keep large SwkbdState off the stack (avoids VFP alignment issues)
    static SwkbdState swkbd;
    static char temp[512];

    // Copy current value to temp buffer
    strncpy(temp, buffer, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    // Ensure GPU is idle before launching swkbd applet
    gfxFlushBuffers();
    gspWaitForVBlank();

    // Initialize keyboard
    memset(&swkbd, 0, sizeof(swkbd));
    int keyboard_max = max_len - 1;
    if (keyboard_max > (int)sizeof(temp) - 1) keyboard_max = sizeof(temp) - 1;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, keyboard_max);
    swkbdSetInitialText(&swkbd, temp);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK", true);

    // Show keyboard
    SwkbdButton button = swkbdInputText(&swkbd, temp, sizeof(temp));

    if (button == SWKBD_BUTTON_RIGHT) {
        // User confirmed - copy back
        strncpy(buffer, temp, max_len - 1);
        buffer[max_len - 1] = '\0';
        return true;
    }

    return false;  // Cancelled
}
