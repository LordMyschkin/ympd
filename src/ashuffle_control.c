/* Manage the original ashuffle process so ympd can expose its full CLI. */
#include "ashuffle_control.h"
#include "mpd_client.h"
#include "json_encode.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

struct radio_config {
    int enabled, buffer, only, no_check, window_size;
    char mode[16], group_tags[128], file[512], exclude_from[512], exclude_rules[512];
    char log_file[512], reconnect_timeout[32], suspend_timeout[32];
    int play_on_startup, exit_on_db_update;
};

static struct radio_config config = {
    .enabled = 1, .buffer = 1, .window_size = 7, .mode = "songs",
    .reconnect_timeout = "10s", .suspend_timeout = "0ms", .play_on_startup = 1
};
static pid_t child = -1;
static time_t retry_at;
static char config_path[1024];
static char status_message[128] = "Starting";
static char ashuffle_binary[PATH_MAX];
static int only_finished;
static int restart_requested;
static time_t stop_requested_at;

static int number(const char *value, int min, int max, int *result) {
    char *end;
    long n;
    if (!*value) return 0;
    errno = 0;
    n = strtol(value, &end, 10);
    if (errno || *end || n < min || n > max) return 0;
    *result = (int)n;
    return 1;
}

static int assign(struct radio_config *c, const char *key, const char *value) {
#define FIELD(name) if (!strcmp(key, #name)) { \
    if (strlen(value) >= sizeof(c->name) || strchr(value, '\n') || strchr(value, '\r')) return 0; \
    strcpy(c->name, value); return 1; }
    if (!strcmp(key, "enabled")) return number(value, 0, 1, &c->enabled);
    if (!strcmp(key, "buffer")) return number(value, 0, 100, &c->buffer);
    if (!strcmp(key, "only")) return number(value, 0, 1000000, &c->only);
    if (!strcmp(key, "no_check")) return number(value, 0, 1, &c->no_check);
    if (!strcmp(key, "window_size")) return number(value, 1, 100000, &c->window_size);
    if (!strcmp(key, "play_on_startup")) return number(value, 0, 1, &c->play_on_startup);
    if (!strcmp(key, "exit_on_db_update")) return number(value, 0, 1, &c->exit_on_db_update);
    FIELD(mode)
    FIELD(group_tags)
    FIELD(file)
    FIELD(exclude_from)
    FIELD(exclude_rules)
    FIELD(log_file)
    FIELD(reconnect_timeout)
    FIELD(suspend_timeout)
#undef FIELD
    return 0;
}

static int valid_config(const struct radio_config *c) {
    if (strcmp(c->mode, "songs") && strcmp(c->mode, "albums") && strcmp(c->mode, "groups")) return 0;
    if (!strcmp(c->mode, "groups") && !c->group_tags[0]) return 0;
    if (c->no_check && !c->file[0]) return 0;
    if (c->no_check && strcmp(c->mode, "songs")) return 0;
    if (!strcmp(c->file, "-")) return 0; /* ympd has no URI stream on stdin. */
    if (!strcmp(c->mode, "groups")) {
        int found = 0, in_tag = 0;
        for (const unsigned char *p = (const unsigned char *)c->group_tags; *p; ++p) {
            if (isalnum(*p) || *p == '_') {
                if (!in_tag && ++found > 8) return 0;
                in_tag = 1;
            } else if (*p == ',' || *p == ' ') in_tag = 0;
            else return 0;
        }
        if (!found) return 0;
    }
    if (!c->reconnect_timeout[0] || !c->suspend_timeout[0]) return 0;
    /* Values become argv entries, never shell code. Exclude control characters. */
    const char *fields[] = {c->group_tags, c->file, c->exclude_from, c->exclude_rules, c->log_file,
                            c->reconnect_timeout, c->suspend_timeout};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        for (const unsigned char *p = (const unsigned char *)fields[i]; *p; ++p)
            if (*p < 32 || *p == 127) return 0;
    if (c->exclude_rules[0]) {
        char rules[sizeof(c->exclude_rules)], *rule_save = NULL;
        int rule_count = 0, pair_count = 0;
        strcpy(rules, c->exclude_rules);
        for (char *rule = strtok_r(rules, ";", &rule_save); rule;
             rule = strtok_r(NULL, ";", &rule_save)) {
            char *pair_save = NULL;
            int in_rule = 0;
            if (++rule_count > 4) return 0;
            for (char *pair = strtok_r(rule, ",", &pair_save); pair;
                 pair = strtok_r(NULL, ",", &pair_save)) {
                char *equal = strchr(pair, '=');
                if (!equal || equal == pair || !equal[1] || ++pair_count > 8) return 0;
                for (char *p = pair; p < equal; ++p)
                    if (!isalnum((unsigned char)*p) && *p != '_') return 0;
                ++in_rule;
            }
            if (!in_rule) return 0;
        }
        if (!rule_count) return 0;
    }
    return 1;
}

static int unescape(char *value) {
    char *write = value;
    for (char *read = value; *read; ++read) {
        if (*read == '%') {
            int hi, lo;
            if (!isxdigit((unsigned char)read[1]) || !isxdigit((unsigned char)read[2])) return 0;
            hi = isdigit((unsigned char)read[1]) ? read[1] - '0' : tolower((unsigned char)read[1]) - 'a' + 10;
            lo = isdigit((unsigned char)read[2]) ? read[2] - '0' : tolower((unsigned char)read[2]) - 'a' + 10;
            *write++ = (char)(hi * 16 + lo);
            read += 2;
        } else if (*read == '+') *write++ = ' ';
        else *write++ = *read;
    }
    *write = 0;
    return 1;
}

static int save_config(void) {
    char temporary[sizeof(config_path) + 8];
    FILE *f;
    if (!config_path[0]) return 0;
    snprintf(temporary, sizeof(temporary), "%s.tmp", config_path);
    f = fopen(temporary, "w");
    if (!f) return 0;
    chmod(temporary, 0600);
#define SAVE_INT(name) fprintf(f, #name "=%d\n", config.name)
#define SAVE_STR(name) fprintf(f, #name "=%s\n", config.name)
    SAVE_INT(enabled); SAVE_INT(buffer); SAVE_INT(only); SAVE_INT(no_check);
    SAVE_INT(window_size); SAVE_INT(play_on_startup); SAVE_INT(exit_on_db_update);
    SAVE_STR(mode); SAVE_STR(group_tags); SAVE_STR(file); SAVE_STR(exclude_from); SAVE_STR(exclude_rules);
    SAVE_STR(log_file); SAVE_STR(reconnect_timeout); SAVE_STR(suspend_timeout);
#undef SAVE_INT
#undef SAVE_STR
    if (fclose(f) != 0) return 0;
    return rename(temporary, config_path) == 0;
}

static void locate_ashuffle(const char *ympd_executable) {
    char executable[PATH_MAX] = "";
#ifdef __APPLE__
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0)
        executable[0] = 0;
#elif defined(__linux__)
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size > 0) executable[size] = 0;
#endif
    if (!executable[0] && ympd_executable && strchr(ympd_executable, '/'))
        snprintf(executable, sizeof(executable), "%s", ympd_executable);
    if (!executable[0]) return;

    char resolved[PATH_MAX];
    if (!realpath(executable, resolved)) return;
    char *slash = strrchr(resolved, '/');
    if (!slash) return;
    *slash = 0;
    snprintf(ashuffle_binary, sizeof(ashuffle_binary), "%s/ashuffle", resolved);
}

void ashuffle_init(const char *ympd_executable) {
    locate_ashuffle(ympd_executable);
    const char *override = getenv("YMPD_ASHUFFLE_CONFIG_DIR");
    const char *base = getenv("XDG_CONFIG_HOME");
    FILE *f;
    char directory[sizeof(config_path)];
    if (override && *override) {
        snprintf(config_path, sizeof(config_path), "%s/ashuffle.conf", override);
        goto load;
    }
    if (!base || !*base) {
        base = getenv("HOME");
        if (!base || !*base) return;
        snprintf(directory, sizeof(directory), "%s/.config", base);
        mkdir(directory, 0700);
        base = directory;
    }
    char radio_dir[sizeof(config_path)];
    snprintf(radio_dir, sizeof(radio_dir), "%s/ympd", base);
    mkdir(radio_dir, 0700);
    snprintf(config_path, sizeof(config_path), "%s/ashuffle.conf", radio_dir);
load:
    f = fopen(config_path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *equal = strchr(line, '=');
        if (!equal) continue;
        *equal++ = 0;
        equal[strcspn(equal, "\r\n")] = 0;
        assign(&config, line, equal);
    }
    fclose(f);
    if (!valid_config(&config)) config.enabled = 0;
}

void ashuffle_restart(void) {
    if (child > 0 && !restart_requested) {
        restart_requested = 1;
        stop_requested_at = time(NULL);
        kill(child, SIGTERM);
    }
    retry_at = 0;
    only_finished = 0;
    snprintf(status_message, sizeof(status_message), "%s",
             config.enabled ? "Restarting Ashuffle" : "Stopping Ashuffle");
}

static void start_child(void) {
    char port[16], buffer[16], only[16], window[48], startup[48];
    char db_update[48], reconnect[64], suspend[64], host[640];
    char *args[64], *tags = NULL, *rules = NULL, *save = NULL;
    int n = 0;
    snprintf(port, sizeof(port), "%d", mpd.port);
    snprintf(buffer, sizeof(buffer), "%d", config.buffer);
    snprintf(only, sizeof(only), "%d", config.only);
    snprintf(window, sizeof(window), "window-size=%d", config.window_size);
    snprintf(startup, sizeof(startup), "play-on-startup=%s", config.play_on_startup ? "yes" : "no");
    snprintf(db_update, sizeof(db_update), "exit-on-db-update=%s", config.exit_on_db_update ? "yes" : "no");
    snprintf(reconnect, sizeof(reconnect), "reconnect-timeout=%s", config.reconnect_timeout);
    snprintf(suspend, sizeof(suspend), "suspend-timeout=%s", config.suspend_timeout);
    if (mpd.password) snprintf(host, sizeof(host), "%s@%s", mpd.password, mpd.host);
    else snprintf(host, sizeof(host), "%s", mpd.host);

    args[n++] = "ashuffle";
    args[n++] = "--host"; args[n++] = host;
    args[n++] = "--port"; args[n++] = port;
    args[n++] = "--queue-buffer"; args[n++] = buffer;
    if (!strcmp(config.mode, "albums")) args[n++] = "--by-album";
    if (!strcmp(config.mode, "groups")) {
        tags = strdup(config.group_tags);
        if (!tags) return;
        args[n++] = "--group-by";
        for (char *tag = strtok_r(tags, ", ", &save); tag && n < 32;
             tag = strtok_r(NULL, ", ", &save)) args[n++] = tag;
    }
    if (config.exclude_rules[0]) {
        char *rule_save = NULL;
        rules = strdup(config.exclude_rules);
        if (!rules) { free(tags); return; }
        for (char *rule = strtok_r(rules, ";", &rule_save); rule;
             rule = strtok_r(NULL, ";", &rule_save)) {
            char *pair_save = NULL;
            args[n++] = "--exclude";
            for (char *pair = strtok_r(rule, ",", &pair_save); pair;
                 pair = strtok_r(NULL, ",", &pair_save)) {
                char *equal = strchr(pair, '=');
                *equal++ = 0;
                args[n++] = pair;
                args[n++] = equal;
            }
        }
    }
    if (config.file[0]) { args[n++] = "--file"; args[n++] = config.file; }
    if (config.exclude_from[0]) { args[n++] = "--exclude-from"; args[n++] = config.exclude_from; }
    if (config.no_check) args[n++] = "--no-check";
    if (config.only) { args[n++] = "--only"; args[n++] = only; }
    if (config.log_file[0]) { args[n++] = "--log-file"; args[n++] = config.log_file; }
    args[n++] = "--tweak"; args[n++] = window;
    args[n++] = "--tweak"; args[n++] = startup;
    args[n++] = "--tweak"; args[n++] = db_update;
    args[n++] = "--tweak"; args[n++] = reconnect;
    args[n++] = "--tweak"; args[n++] = suspend;
    args[n] = NULL;
    if (!ashuffle_binary[0] || access(ashuffle_binary, X_OK) != 0) {
        snprintf(status_message, sizeof(status_message), "Bundled ashuffle executable not found");
        free(tags);
        free(rules);
        retry_at = time(NULL) + 10;
        return;
    }
    fprintf(stderr, "Starting bundled Ashuffle: %s\n", ashuffle_binary);
    child = fork();
    if (child == 0) {
        execv(ashuffle_binary, args);
        _exit(127);
    }
    free(tags);
    free(rules);
    if (child < 0) snprintf(status_message, sizeof(status_message), "Could not start ashuffle");
    else snprintf(status_message, sizeof(status_message), "Running");
    retry_at = time(NULL) + 10;
}

void ashuffle_tick(void) {
    if (child > 0) {
        int status;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) {
            int expected_stop = restart_requested || !config.enabled;
            child = -1;
            restart_requested = 0;
            if (!expected_stop && result > 0 &&
                (config.only || config.exit_on_db_update) &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0)
                only_finished = 1;
            if (expected_stop)
                snprintf(status_message, sizeof(status_message), "%s",
                         config.enabled ? "Restarting Ashuffle" : "Ashuffle stopped");
            else
                snprintf(status_message, sizeof(status_message),
                         "Ashuffle exited (status %d); see server log",
                         result > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        } else if (restart_requested && time(NULL) - stop_requested_at >= 3) {
            /* Do not let an unresponsive old process block the new settings. */
            kill(child, SIGKILL);
        }
    }
    if (!config.enabled) {
        if (child > 0 && !restart_requested) ashuffle_restart();
        return;
    }
    if (child < 0 && !only_finished && time(NULL) >= retry_at && mpd.conn_state == MPD_CONNECTED)
        start_child();
}

void ashuffle_shutdown(void) {
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        child = -1;
    }
}

int ashuffle_enabled(void) { return config.enabled; }

int ashuffle_apply_query(const char *query, char *error, size_t error_size) {
    struct radio_config updated = config;
    struct radio_config previous = config;
    char *copy = strdup(query), *save = NULL;
    if (!copy) return 0;
    for (char *part = strtok_r(copy, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
        char *value = strchr(part, '=');
        if (!value) goto invalid;
        *value++ = 0;
        if (!unescape(value) || !assign(&updated, part, value)) goto invalid;
    }
    if (!valid_config(&updated)) goto invalid;
    config = updated;
    if (!save_config()) {
        config = previous;
        snprintf(error, error_size, "Could not save Ashuffle settings");
        free(copy);
        return 0;
    }
    ashuffle_restart();
    free(copy);
    return 1;
invalid:
    snprintf(error, error_size, "Invalid Ashuffle settings");
    free(copy);
    return 0;
}

size_t ashuffle_status_json(char *out, size_t size) {
    char *cur = out;
    size_t left = size;
#define RAW(s) do { int written = snprintf(cur, left, "%s", s); cur += written; left -= written; } while (0)
#define INT(name) do { int written = snprintf(cur, left, "\"" #name "\":%d,", config.name); cur += written; left -= written; } while (0)
#define STR(name) do { RAW("\"" #name "\":"); int written = json_emit_quoted_str(cur, (int)left, config.name); cur += written; left -= written; RAW(","); } while (0)
    RAW("{\"type\":\"ashuffle\",\"data\":{");
    INT(enabled); INT(buffer); INT(only); INT(no_check); INT(window_size);
    INT(play_on_startup); INT(exit_on_db_update);
    STR(mode); STR(group_tags); STR(file); STR(exclude_from); STR(exclude_rules); STR(log_file);
    STR(reconnect_timeout); STR(suspend_timeout);
    RAW("\"running\":"); RAW(child > 0 && !restart_requested ? "true" : "false"); RAW(",\"status\":");
    cur += json_emit_quoted_str(cur, (int)left, status_message);
    left = size - (size_t)(cur - out);
    RAW("}}");
#undef RAW
#undef INT
#undef STR
    return (size_t)(cur - out);
}
