#include "config.h"

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

    return true;
}
