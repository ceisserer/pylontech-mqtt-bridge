/*
 * pylon2mqtt - Pylontech battery to MQTT bridge
 *
 * Polls a Pylontech battery stack (US2000, US3000, US5000, ... and
 * compatibles) via the "PYLON low voltage Protocol RS485" and publishes the
 * values to an MQTT broker. Which values are published under which topics is
 * configurable: each topic maps to a path into the data tree (see README.md).
 *
 * Protocol handling and the data tree are ported from the Python library
 * python-pylontech (https://github.com/Tomcat42/pylontech-python) by
 * Bernd Singer, stack abstraction (PylontechStack) by Daniel Schramm.
 *
 * Copyright (c) 2022 Bernd Singer (python-pylontech, MIT License,
 *                    see LICENSE.python-pylontech)
 * Copyright (c) 2026 Clemens Eisserer (C port)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The battery communication is in libpylontech (pylontech.c), which is
 * compiled into this program.
 *
 * Dependencies: libmosquitto, cJSON
 *
 * Usage: pylon2mqtt [config_file]   (default: /etc/pylon2mqtt/pylon.json)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <mosquitto.h>

#include "pylontech.h"

#define DEFAULT_CONFIG_FILE "/etc/pylon2mqtt/pylon.json"

#define LOG_MAX_BYTES (1024 * 1024 * 2)

/* exit codes as in kaifareader */
#define EXIT_CONFIG 10
#define EXIT_LOGGING 20
#define EXIT_MQTT 40

/* Python logging levels */
#define LVL_DEBUG 10
#define LVL_INFO 20
#define LVL_WARNING 30
#define LVL_ERROR 40
#define LVL_CRITICAL 50

/* topic paths */
#define IDX_WILDCARD (-1)
#define MAX_SEG_INDEXES 4
#define MAX_WILDCARDS 8

static volatile sig_atomic_t g_stop = 0;

/*
 * Logging (format and rotation like Python's RotatingFileHandler,
 * maxBytes=2MiB, backupCount=1); without logfile to stderr
 */

static struct {
    FILE *fp;
    const char *path;
    int level;
    pthread_mutex_t mu;
} g_log = { NULL, NULL, LVL_INFO, PTHREAD_MUTEX_INITIALIZER };

static const char *level_name(int level)
{
    switch (level) {
    case LVL_DEBUG: return "DEBUG";
    case LVL_INFO: return "INFO";
    case LVL_WARNING: return "WARNING";
    case LVL_ERROR: return "ERROR";
    default: return "CRITICAL";
    }
}

static bool log_init(const char *path, int level)
{
    g_log.path = path;
    g_log.level = level;
    if (!path) {
        g_log.fp = stderr;
        return true;
    }
    g_log.fp = fopen(path, "a");
    if (!g_log.fp) {
        fprintf(stderr, "Could not initialize logging system: %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static void log_rollover_if_needed(size_t msg_len)
{
    struct stat st;

    if (!g_log.path || fstat(fileno(g_log.fp), &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if ((size_t)st.st_size + msg_len < LOG_MAX_BYTES)
        return;

    char backup[4096];
    snprintf(backup, sizeof(backup), "%s.1", g_log.path);
    fclose(g_log.fp);
    unlink(backup);
    rename(g_log.path, backup);
    g_log.fp = fopen(g_log.path, "a");
}

static void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_msg(int level, const char *fmt, ...)
{
    if (level < g_log.level || !g_log.fp)
        return;

    char *msg = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    struct timespec ts;
    struct tm tm;
    char tbuf[32];
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

    pthread_mutex_lock(&g_log.mu);
    /* "%(asctime)s [%(levelname)s]:  %(message)s" */
    char head[64];
    int hlen = snprintf(head, sizeof(head), "%s,%03ld [%s]:  ", tbuf, ts.tv_nsec / 1000000L,
                        level_name(level));
    log_rollover_if_needed((size_t)hlen + (size_t)n + 1);
    if (g_log.fp) {
        fprintf(g_log.fp, "%s%s\n", head, msg);
        fflush(g_log.fp);
    }
    pthread_mutex_unlock(&g_log.mu);
    free(msg);
}

#define log_debug(...) log_msg(LVL_DEBUG, __VA_ARGS__)
#define log_info(...) log_msg(LVL_INFO, __VA_ARGS__)
#define log_warning(...) log_msg(LVL_WARNING, __VA_ARGS__)
#define log_error(...) log_msg(LVL_ERROR, __VA_ARGS__)

/*
 * Growable string buffer
 */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sbuf_t;

static void sb_putn(sbuf_t *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 256;
        while (cap < sb->len + n + 1)
            cap *= 2;
        char *tmp = realloc(sb->buf, cap);
        if (!tmp) {
            log_error("Out of memory");
            exit(1);
        }
        sb->buf = tmp;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(sbuf_t *sb, const char *s)
{
    sb_putn(sb, s, strlen(s));
}

static void sb_putc(sbuf_t *sb, char c)
{
    sb_putn(sb, &c, 1);
}

static void sb_printf(sbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_printf(sbuf_t *sb, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_putn(sb, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void sb_clear(sbuf_t *sb)
{
    sb->len = 0;
    if (sb->buf)
        sb->buf[0] = '\0';
}

/*
 * Payload formatting
 */

static void json_string(sbuf_t *sb, const char *s)
{
    sb_putc(sb, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        case '\b': sb_puts(sb, "\\b"); break;
        case '\f': sb_puts(sb, "\\f"); break;
        default:
            if (c < 0x20)
                sb_printf(sb, "\\u%04x", c);
            else
                sb_putc(sb, (char)c);
        }
    }
    sb_putc(sb, '"');
}

/* JSON; py: separators like Python's json.dumps() (", " and ": ") */
static void json_write(sbuf_t *sb, const cJSON *n, bool py)
{
    if (cJSON_IsRaw(n)) {
        sb_puts(sb, n->valuestring);
    } else if (cJSON_IsNumber(n)) {
        sb_printf(sb, "%.0f", n->valuedouble);
    } else if (cJSON_IsTrue(n)) {
        sb_puts(sb, "true");
    } else if (cJSON_IsFalse(n)) {
        sb_puts(sb, "false");
    } else if (cJSON_IsString(n)) {
        json_string(sb, n->valuestring);
    } else if (cJSON_IsArray(n) || cJSON_IsObject(n)) {
        bool obj = cJSON_IsObject(n);
        bool first = true;
        const cJSON *c;
        sb_putc(sb, obj ? '{' : '[');
        cJSON_ArrayForEach(c, n) {
            if (!first)
                sb_puts(sb, py ? ", " : ",");
            first = false;
            if (obj) {
                json_string(sb, c->string);
                sb_puts(sb, py ? ": " : ":");
            }
            json_write(sb, c, py);
        }
        sb_putc(sb, obj ? '}' : ']');
    } else {
        sb_puts(sb, "null");
    }
}

/*
 * MQTT payload of a node: single values plain, arrays and objects as JSON.
 * py: like the Python version (str() of single values, json.dumps() otherwise)
 */
static void payload_format(sbuf_t *sb, const cJSON *n, bool py)
{
    if (cJSON_IsString(n))
        sb_puts(sb, n->valuestring);
    else if (py && cJSON_IsBool(n))
        sb_puts(sb, cJSON_IsTrue(n) ? "True" : "False");
    else if (py && cJSON_IsNull(n))
        ; /* paho publishes None as empty payload */
    else
        json_write(sb, n, py);
}

/*
 * Config
 */

typedef struct {
    char *name; /* object member, "*" for all members */
    int nidx;
    int idx[MAX_SEG_INDEXES]; /* array index or IDX_WILDCARD */
} path_seg_t;

typedef struct {
    const char *topic; /* relative to basetopic, may contain {N} placeholders */
    const char *path;
    path_seg_t *segs;
    int nsegs;
    int nwild;
    bool warned;
} mqtt_topic_t;

typedef struct {
    cJSON *root;
    int loglevel;
    const char *logfile;
    const char *port;
    int baudrate;
    int battery_count; /* 0: auto detect */
    int group;
    double interval;
    double timeout;
    const char *export_file_abspath;
    const char *mqtt_server;
    int mqtt_port;
    const char *mqtt_client_id;
    const char *mqtt_user;
    const char *mqtt_password;
    const char *mqtt_basetopic;
    int mqtt_qos;
    bool mqtt_retain;
    bool mqtt_on_change;
    bool mqtt_python_format;
    const char *mqtt_status_topic;
    mqtt_topic_t *mqtt_topics;
    size_t mqtt_topic_count;
} config_t;

/* published by pylon.py, used without "export_mqtt_topics" */
static const struct {
    const char *topic;
    const char *path;
} DEFAULT_MQTT_TOPICS[] = {
    { "PowerW", "Calculated.Power_W" },
    { "RemCapAh", "Calculated.RemainCapacity_Ah" },
    { "RemPerc", "Calculated.Remain_Percent" },
    { "FullChargeReq", "ChargeDischargeManagement[0].StatusFullChargeRequired" },
    { "Voltage", "Analog[0].Voltage" },
    { "CellVoltages", "Analog[0].CellVoltages" },
};

/* key names of the Python library */
static const struct {
    const char *alias;
    const char *name;
} KEY_ALIASES[] = {
    { "AnaloglList", "Analog" },
    { "ChargeDischargeManagementList", "ChargeDischargeManagement" },
    { "AlarmInfoList", "AlarmInfo" },
    { "ChargeCurentAlarm", "ChargeCurrentAlarm" },
};

/* "Analog[*].CellVoltages[3]" -> segments; false on syntax error */
static bool path_compile(mqtt_topic_t *t, const char *path)
{
    const char *p = path;

    t->path = path;
    if (!*p)
        return true;

    for (;;) {
        path_seg_t *segs = realloc(t->segs, ((size_t)t->nsegs + 1) * sizeof(*segs));
        if (!segs)
            return false;
        t->segs = segs;
        path_seg_t *s = &t->segs[t->nsegs++];
        memset(s, 0, sizeof(*s));

        size_t n = strcspn(p, ".[]");
        if (n == 0)
            return false;
        s->name = strndup(p, n);
        for (size_t i = 0; i < sizeof(KEY_ALIASES) / sizeof(KEY_ALIASES[0]); i++) {
            if (strcmp(s->name, KEY_ALIASES[i].alias) == 0) {
                free(s->name);
                s->name = strdup(KEY_ALIASES[i].name);
            }
        }
        if (strcmp(s->name, "*") == 0)
            t->nwild++;
        p += n;

        while (*p == '[') {
            p++;
            if (s->nidx == MAX_SEG_INDEXES)
                return false;
            if (*p == '*') {
                s->idx[s->nidx++] = IDX_WILDCARD;
                t->nwild++;
                p++;
            } else if (isdigit((unsigned char)*p)) {
                char *end;
                long v = strtol(p, &end, 10);
                if (v > 65535)
                    return false;
                s->idx[s->nidx++] = (int)v;
                p = end;
            } else {
                return false;
            }
            if (*p != ']')
                return false;
            p++;
        }

        if (*p == '\0')
            break;
        if (*p != '.')
            return false;
        p++;
    }
    return t->nwild <= MAX_WILDCARDS;
}

/* parse "{N}", "{N+M}" or "{N-M}" at p; returns the length or 0 if there is none */
static size_t placeholder_parse(const char *p, int *n, int *offset)
{
    const char *q = p + 1;
    char *end;

    if (!isdigit((unsigned char)*q))
        return 0;
    long v = strtol(q, &end, 10);
    q = end;
    long off = 0;
    if (*q == '+' || *q == '-') {
        char sign = *q++;
        if (!isdigit((unsigned char)*q))
            return 0;
        off = strtol(q, &end, 10);
        q = end;
        if (sign == '-')
            off = -off;
    }
    if (*q != '}' || v > MAX_WILDCARDS || off > 65535 || off < -65535)
        return 0;
    *n = (int)v;
    *offset = (int)off;
    return (size_t)(q + 1 - p);
}

static bool topic_validate(const mqtt_topic_t *t)
{
    const char *p = t->topic;

    while (*p) {
        if (*p == '{') {
            int n, off;
            size_t len = placeholder_parse(p, &n, &off);
            if (!len || n >= t->nwild)
                return false;
            p += len;
        } else if (*p == '+' || *p == '#') {
            return false; /* MQTT wildcards are not allowed in published topics */
        } else {
            p++;
        }
    }
    return true;
}

static bool add_topic(config_t *cfg, const char *file, const char *topic, const char *path)
{
    mqtt_topic_t *topics = realloc(cfg->mqtt_topics, (cfg->mqtt_topic_count + 1) * sizeof(*topics));
    if (!topics)
        return false;
    cfg->mqtt_topics = topics;
    mqtt_topic_t *t = &cfg->mqtt_topics[cfg->mqtt_topic_count++];
    memset(t, 0, sizeof(*t));
    t->topic = topic;

    if (!*topic && !*cfg->mqtt_basetopic) {
        fprintf(stderr, "Config file %s: empty MQTT topic for path \"%s\"\n", file, path);
        return false;
    }
    if (!path_compile(t, path)) {
        fprintf(stderr, "Config file %s: invalid path \"%s\" for MQTT topic \"%s\"\n", file, path,
                topic);
        return false;
    }
    if (!topic_validate(t)) {
        if (t->nwild > 0)
            fprintf(stderr,
                    "Config file %s: invalid MQTT topic \"%s\" (no + or #; placeholders "
                    "{0}..{%d} for the wildcards of path \"%s\")\n",
                    file, topic, t->nwild - 1, path);
        else
            fprintf(stderr,
                    "Config file %s: invalid MQTT topic \"%s\" (no + or #; no placeholders, "
                    "path \"%s\" has no wildcards)\n",
                    file, topic, path);
        return false;
    }
    return true;
}

static const char *strip_prefix(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? s + n : s;
}

static bool parse_loglevel(const cJSON *item, int *out)
{
    static const struct { const char *name; int level; } levels[] = {
        { "DEBUG", LVL_DEBUG }, { "INFO", LVL_INFO }, { "WARNING", LVL_WARNING },
        { "WARN", LVL_WARNING }, { "ERROR", LVL_ERROR }, { "CRITICAL", LVL_CRITICAL },
        { "FATAL", LVL_CRITICAL },
    };
    if (cJSON_IsNumber(item)) {
        *out = item->valueint;
        return true;
    }
    if (!cJSON_IsString(item))
        return false;
    const char *s = strip_prefix(item->valuestring, "logging.");
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(s, levels[i].name) == 0) {
            *out = levels[i].level;
            return true;
        }
    }
    return false;
}

static bool cfg_string(const cJSON *r, const char *file, const char *name, const char *def,
                       const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(r, name);
    if (!item || cJSON_IsNull(item)) {
        *out = def;
        return true;
    }
    if (!cJSON_IsString(item)) {
        fprintf(stderr, "Config file %s: \"%s\" must be a string\n", file, name);
        return false;
    }
    *out = item->valuestring;
    return true;
}

static bool cfg_number(const cJSON *r, const char *file, const char *name, double def, double min,
                       double max, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(r, name);
    if (!item) {
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) {
        fprintf(stderr, "Config file %s: invalid value for \"%s\" (expected %g..%g)\n", file, name,
                min, max);
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static bool cfg_int(const cJSON *r, const char *file, const char *name, int def, int min, int max,
                    int *out)
{
    double v;
    if (!cfg_number(r, file, name, def, min, max, &v))
        return false;
    *out = (int)v;
    return true;
}

static bool cfg_bool(const cJSON *r, const char *file, const char *name, bool def, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(r, name);
    if (!item) {
        *out = def;
        return true;
    }
    if (!cJSON_IsBool(item)) {
        fprintf(stderr, "Config file %s: \"%s\" must be true or false\n", file, name);
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static bool config_load(config_t *cfg, const char *file)
{
    memset(cfg, 0, sizeof(*cfg));

    FILE *f = fopen(file, "r");
    if (!f) {
        fprintf(stderr, "Error loading config file %s: %s\n", file, strerror(errno));
        return false;
    }
    char *data = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *tmp = realloc(data, cap);
            if (!tmp) {
                free(data);
                fclose(f);
                return false;
            }
            data = tmp;
        }
        size_t n = fread(data + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0)
            break;
    }
    fclose(f);
    data[len] = '\0';
    cfg->root = cJSON_Parse(data);
    free(data);
    if (!cfg->root) {
        fprintf(stderr, "Error loading config file %s: invalid JSON\n", file);
        return false;
    }

    const cJSON *r = cfg->root;
    const cJSON *item;

    cfg->loglevel = LVL_INFO;
    item = cJSON_GetObjectItemCaseSensitive(r, "loglevel");
    if (item && !parse_loglevel(item, &cfg->loglevel)) {
        fprintf(stderr, "Config file %s: invalid value for \"loglevel\"\n", file);
        return false;
    }

    if (!cfg_string(r, file, "logfile", NULL, &cfg->logfile) ||
        !cfg_string(r, file, "port", NULL, &cfg->port) ||
        !cfg_int(r, file, "baudrate", 9600, 300, 921600, &cfg->baudrate) ||
        !cfg_int(r, file, "battery_count", 0, 0, PYLON_MAX_BATTERIES, &cfg->battery_count) ||
        !cfg_int(r, file, "group", 0, 0, 7, &cfg->group) ||
        !cfg_number(r, file, "interval", 2, 0, 86400, &cfg->interval) ||
        !cfg_number(r, file, "timeout", 1, 0.05, 60, &cfg->timeout) ||
        !cfg_string(r, file, "export_file_abspath", NULL, &cfg->export_file_abspath) ||
        !cfg_string(r, file, "export_mqtt_server", NULL, &cfg->mqtt_server) ||
        !cfg_int(r, file, "export_mqtt_port", 1883, 1, 65535, &cfg->mqtt_port) ||
        !cfg_string(r, file, "export_mqtt_client_id", "pylon2mqtt", &cfg->mqtt_client_id) ||
        !cfg_string(r, file, "export_mqtt_user", NULL, &cfg->mqtt_user) ||
        !cfg_string(r, file, "export_mqtt_password", NULL, &cfg->mqtt_password) ||
        !cfg_string(r, file, "export_mqtt_basetopic", "Pylon", &cfg->mqtt_basetopic) ||
        !cfg_int(r, file, "export_mqtt_qos", 0, 0, 2, &cfg->mqtt_qos) ||
        !cfg_bool(r, file, "export_mqtt_retain", false, &cfg->mqtt_retain) ||
        !cfg_bool(r, file, "export_mqtt_on_change", false, &cfg->mqtt_on_change) ||
        !cfg_bool(r, file, "export_mqtt_python_format", false, &cfg->mqtt_python_format) ||
        !cfg_string(r, file, "export_mqtt_status_topic", NULL, &cfg->mqtt_status_topic))
        return false;

    if (!cfg->port) {
        fprintf(stderr, "Config file %s: missing key \"port\"\n", file);
        return false;
    }
    /* empty user means anonymous, like paho's username_pw_set("", "") */
    if (cfg->mqtt_user && !*cfg->mqtt_user) {
        cfg->mqtt_user = NULL;
        cfg->mqtt_password = NULL;
    }

    item = cJSON_GetObjectItemCaseSensitive(r, "export_mqtt_topics");
    if (item) {
        const cJSON *entry;
        if (!cJSON_IsObject(item)) {
            fprintf(stderr, "Config file %s: \"export_mqtt_topics\" must be an object\n", file);
            return false;
        }
        cJSON_ArrayForEach(entry, item) {
            if (!cJSON_IsString(entry)) {
                fprintf(stderr, "Config file %s: path for MQTT topic \"%s\" must be a string\n",
                        file, entry->string);
                return false;
            }
            if (!add_topic(cfg, file, entry->string, entry->valuestring))
                return false;
        }
    } else {
        for (size_t i = 0; i < sizeof(DEFAULT_MQTT_TOPICS) / sizeof(DEFAULT_MQTT_TOPICS[0]); i++)
            if (!add_topic(cfg, file, DEFAULT_MQTT_TOPICS[i].topic, DEFAULT_MQTT_TOPICS[i].path))
                return false;
    }

    return true;
}

static void config_free(config_t *cfg)
{
    for (size_t i = 0; i < cfg->mqtt_topic_count; i++) {
        for (int s = 0; s < cfg->mqtt_topics[i].nsegs; s++)
            free(cfg->mqtt_topics[i].segs[s].name);
        free(cfg->mqtt_topics[i].segs);
    }
    free(cfg->mqtt_topics);
    cJSON_Delete(cfg->root);
}

/*
 * MQTT
 */

typedef struct {
    int index;       /* array wildcard */
    const char *key; /* object member wildcard */
} wild_t;

typedef struct {
    char *topic;
    char *payload;
} last_value_t;

/* last published payload per topic, for export_mqtt_on_change */
static struct {
    last_value_t *v;
    size_t n;
} g_last;

static last_value_t *last_value_get(const char *topic)
{
    for (size_t i = 0; i < g_last.n; i++)
        if (strcmp(g_last.v[i].topic, topic) == 0)
            return &g_last.v[i];
    last_value_t *v = realloc(g_last.v, (g_last.n + 1) * sizeof(*v));
    if (!v)
        return NULL;
    g_last.v = v;
    g_last.v[g_last.n] = (last_value_t){ strdup(topic), NULL };
    return &g_last.v[g_last.n++];
}

typedef struct {
    const config_t *cfg;
    struct mosquitto *mosq;
    const mqtt_topic_t *topic;
    wild_t wild[MAX_WILDCARDS];
    int found;
    sbuf_t topic_buf;
    sbuf_t payload_buf;
} publish_ctx_t;

/* basetopic + "/" + topic */
static void topic_full(sbuf_t *sb, const config_t *cfg, const char *topic)
{
    const char *base = cfg->mqtt_basetopic;
    sb_puts(sb, base);
    if (*base && *topic)
        sb_putc(sb, '/');
    sb_puts(sb, topic);
}

static void topic_expand(sbuf_t *sb, const config_t *cfg, const char *tpl, const wild_t *wild)
{
    sbuf_t rel = { 0 };
    for (const char *p = tpl; *p;) {
        int n, off;
        size_t len = *p == '{' ? placeholder_parse(p, &n, &off) : 0;
        if (len) {
            if (wild[n].key)
                sb_puts(&rel, wild[n].key);
            else
                sb_printf(&rel, "%d", wild[n].index + off);
            p += len;
        } else {
            sb_putc(&rel, *p++);
        }
    }
    topic_full(sb, cfg, rel.buf ? rel.buf : "");
    free(rel.buf);
}

static void publish_node(publish_ctx_t *ctx, const cJSON *node)
{
    const config_t *cfg = ctx->cfg;

    ctx->found++;
    sb_clear(&ctx->topic_buf);
    topic_expand(&ctx->topic_buf, cfg, ctx->topic->topic, ctx->wild);
    sb_clear(&ctx->payload_buf);
    payload_format(&ctx->payload_buf, node, cfg->mqtt_python_format);
    const char *topic = ctx->topic_buf.buf;
    const char *payload = ctx->payload_buf.buf ? ctx->payload_buf.buf : "";

    last_value_t *last = NULL;
    if (cfg->mqtt_on_change) {
        last = last_value_get(topic);
        if (last && last->payload && strcmp(last->payload, payload) == 0)
            return;
    }
    int rc = mosquitto_publish(ctx->mosq, NULL, topic, (int)strlen(payload), payload,
                               cfg->mqtt_qos, cfg->mqtt_retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_debug("MQTT: publish %s failed: %s", topic, mosquitto_strerror(rc));
        return;
    }
    if (last) {
        free(last->payload);
        last->payload = strdup(payload);
    }
}

/* follow the path from segment si, index step ii (0: member name) */
static void path_walk(publish_ctx_t *ctx, const cJSON *node, int si, int ii, int nw)
{
    const mqtt_topic_t *t = ctx->topic;
    const cJSON *child;

    if (si == t->nsegs) {
        publish_node(ctx, node);
        return;
    }
    const path_seg_t *s = &t->segs[si];

    if (ii == 0) {
        if (!cJSON_IsObject(node))
            return;
        if (strcmp(s->name, "*") == 0) {
            cJSON_ArrayForEach(child, node) {
                ctx->wild[nw] = (wild_t){ 0, child->string };
                path_walk(ctx, child, si, 1, nw + 1);
            }
            return;
        }
        child = cJSON_GetObjectItemCaseSensitive(node, s->name);
        if (child)
            path_walk(ctx, child, si, 1, nw);
        return;
    }

    if (ii - 1 == s->nidx) {
        path_walk(ctx, node, si + 1, 0, nw);
        return;
    }
    if (!cJSON_IsArray(node))
        return;
    int idx = s->idx[ii - 1];
    if (idx == IDX_WILDCARD) {
        int i = 0;
        cJSON_ArrayForEach(child, node) {
            ctx->wild[nw] = (wild_t){ i++, NULL };
            path_walk(ctx, child, si, ii + 1, nw + 1);
        }
        return;
    }
    child = cJSON_GetArrayItem(node, idx);
    if (child)
        path_walk(ctx, child, si, ii + 1, nw);
}

static void export_mqtt(struct mosquitto *mosq, config_t *cfg, const cJSON *tree)
{
    publish_ctx_t ctx = { .cfg = cfg, .mosq = mosq };

    for (size_t i = 0; i < cfg->mqtt_topic_count; i++) {
        mqtt_topic_t *t = &cfg->mqtt_topics[i];
        ctx.topic = t;
        ctx.found = 0;
        path_walk(&ctx, tree, 0, 0, 0);
        if (ctx.found == 0 && !t->warned) {
            log_warning("MQTT: topic \"%s\": no value at path \"%s\"", t->topic, t->path);
            t->warned = true;
        }
    }
    free(ctx.topic_buf.buf);
    free(ctx.payload_buf.buf);
}

static void mqtt_publish_status(struct mosquitto *mosq, const config_t *cfg, const char *status)
{
    sbuf_t topic = { 0 };
    topic_full(&topic, cfg, cfg->mqtt_status_topic);
    mosquitto_publish(mosq, NULL, topic.buf, (int)strlen(status), status, 1, true);
    free(topic.buf);
}

static void mqtt_on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    const config_t *cfg = obj;
    if (rc == 0) {
        log_info("MQTT: Client connected; rc=%d", rc);
        if (cfg->mqtt_status_topic)
            mqtt_publish_status(mosq, cfg, "online");
    } else {
        log_error("MQTT: Client bad RC; rc=%d", rc);
    }
}

static void mqtt_on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    log_info("MQTT: Client disconnected; rc=%d", rc);
    if (rc != 0)
        log_info("MQTT: Trying auto-reconnect; rc=%d", rc);
}

static struct mosquitto *mqtt_start(config_t *cfg)
{
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new(cfg->mqtt_client_id, true, cfg);
    if (!mosq) {
        fprintf(stderr, "Failed to connect: could not create MQTT client\n");
        return NULL;
    }
    mosquitto_connect_callback_set(mosq, mqtt_on_connect);
    mosquitto_disconnect_callback_set(mosq, mqtt_on_disconnect);
    mosquitto_reconnect_delay_set(mosq, 1, 120, true);

    int rc = mosquitto_username_pw_set(mosq, cfg->mqtt_user, cfg->mqtt_password);
    if (rc == MOSQ_ERR_SUCCESS && cfg->mqtt_status_topic) {
        sbuf_t topic = { 0 };
        topic_full(&topic, cfg, cfg->mqtt_status_topic);
        rc = mosquitto_will_set(mosq, topic.buf, 7, "offline", 1, true);
        free(topic.buf);
    }
    if (rc == MOSQ_ERR_SUCCESS)
        rc = mosquitto_connect(mosq, cfg->mqtt_server, cfg->mqtt_port, 60);
    if (rc == MOSQ_ERR_SUCCESS)
        rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        /* libmosquitto clobbers errno, so MOSQ_ERR_ERRNO has no usable detail */
        fprintf(stderr, "Failed to connect to %s:%d: %s\n", cfg->mqtt_server, cfg->mqtt_port,
                rc == MOSQ_ERR_ERRNO ? "connection failed" : mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        return NULL;
    }
    return mosq;
}

/*
 * File export: the whole data tree as JSON, replaced atomically
 */

static void export_file(const config_t *cfg, const cJSON *tree)
{
    char tmp[4096];
    char *json = cJSON_Print(tree);
    if (!json)
        return;

    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->export_file_abspath);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_error("Error writing to file %s: %s", tmp, strerror(errno));
        free(json);
        return;
    }
    fprintf(f, "%s\n", json);
    free(json);
    if (fclose(f) != 0 || rename(tmp, cfg->export_file_abspath) != 0) {
        log_error("Error writing to file %s: %s", cfg->export_file_abspath, strerror(errno));
        unlink(tmp);
    }
}

/*
 * Data tree as cJSON, for the MQTT paths and the export file
 */

#define TREE_MAX_DEPTH 8

typedef struct {
    cJSON *root;
    cJSON *stack[TREE_MAX_DEPTH];
    int depth;
} tree_builder_t;

static void tree_node(void *ctx, const pylon_node_t *n)
{
    tree_builder_t *tb = ctx;
    cJSON *item;

    switch (n->type) {
    case PYLON_NODE_END:
        tb->depth--;
        return;
    case PYLON_NODE_OBJECT: item = cJSON_CreateObject(); break;
    case PYLON_NODE_ARRAY: item = cJSON_CreateArray(); break;
    case PYLON_NODE_INT: item = cJSON_CreateNumber((double)n->i); break;
    /* floats are stored as raw JSON to keep the Python formatting ("6.0") */
    case PYLON_NODE_FLOAT: item = cJSON_CreateRaw(n->text); break;
    case PYLON_NODE_BOOL: item = cJSON_CreateBool(n->b); break;
    case PYLON_NODE_STRING: item = cJSON_CreateString(n->s); break;
    default: item = cJSON_CreateNull(); break;
    }

    if (tb->depth == 0)
        tb->root = item;
    else if (n->key)
        cJSON_AddItemToObject(tb->stack[tb->depth - 1], n->key, item);
    else
        cJSON_AddItemToArray(tb->stack[tb->depth - 1], item);

    if (n->type == PYLON_NODE_OBJECT || n->type == PYLON_NODE_ARRAY) {
        if (tb->depth == TREE_MAX_DEPTH) {
            log_error("Data tree too deep");
            exit(1);
        }
        tb->stack[tb->depth++] = item;
    }
}

static cJSON *data_tree(const pylon_data_t *data)
{
    tree_builder_t tb = { 0 };
    pylon_data_walk(data, tree_node, &tb);
    return tb.root;
}

/*
 * Main
 */

static pylon_t *volatile g_pylon;

static void signal_handler(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_pylon)
        pylon_abort(g_pylon); /* interrupts a running poll */
}

static void pylon_log(void *ctx, int level, const char *msg)
{
    (void)ctx;
    log_msg(level, "%s", msg);
}

static double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_until(double deadline)
{
    while (!g_stop) {
        double left = deadline - now_monotonic();
        if (left <= 0)
            break;
        struct timespec ts = { (time_t)left, (long)((left - floor(left)) * 1e9) };
        nanosleep(&ts, NULL); /* interrupted by signals */
    }
}

int main(int argc, char **argv)
{
    const char *config_file = argc > 1 ? argv[1] : DEFAULT_CONFIG_FILE;
    config_t cfg;

    if (argc > 2 || (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))) {
        fprintf(stderr, "Usage: %s [config_file]   (default: %s)\n", argv[0], DEFAULT_CONFIG_FILE);
        return argc > 2 ? 1 : 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (!config_load(&cfg, config_file)) {
        fprintf(stderr, "Could not load config file\n");
        config_free(&cfg);
        return EXIT_CONFIG;
    }

    if (!log_init(cfg.logfile, cfg.loglevel)) {
        config_free(&cfg);
        return EXIT_LOGGING;
    }

    pylon_config_t pcfg = {
        .port = cfg.port,
        .baudrate = cfg.baudrate,
        .battery_count = cfg.battery_count,
        .group = cfg.group,
        .timeout = cfg.timeout,
        .log = pylon_log,
        .log_level = cfg.loglevel,
    };
    pylon_t *pylon = pylon_new(&pcfg);
    pylon_data_t *data = malloc(sizeof(*data));
    if (!pylon || !data) {
        fprintf(stderr, "Could not initialize: %s\n", strerror(errno));
        pylon_free(pylon);
        config_free(&cfg);
        return 1;
    }
    g_pylon = pylon;
    if (g_stop)
        pylon_abort(pylon);

    if (pylon_connect(pylon) != PYLON_OK) {
        log_error("Could not open %s: %s", cfg.port, pylon_error(pylon));
        if (g_log.fp != stderr)
            fprintf(stderr, "Could not open %s: %s\n", cfg.port, pylon_error(pylon));
        g_pylon = NULL;
        pylon_free(pylon);
        free(data);
        config_free(&cfg);
        return 1;
    }

    /* connect to mqtt broker */
    struct mosquitto *mosq = NULL;
    if (cfg.mqtt_server) {
        /* keep signals away from the mosquitto thread, so they interrupt the main loop */
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &block, &old);
        mosq = mqtt_start(&cfg);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (!mosq) {
            g_pylon = NULL;
            pylon_free(pylon);
            free(data);
            config_free(&cfg);
            return EXIT_MQTT;
        }
    }

    /* errors are logged once, until the communication works again */
    int failures = 0;

    while (!g_stop) {
        double start = now_monotonic();

        /* on errors, the next update reconnects and detects the batteries again */
        int rc = pylon_update(pylon, data);
        if (g_stop)
            break;

        if (rc == PYLON_OK) {
            if (failures > 0)
                log_info("Communication with battery restored after %d failed attempts", failures);
            failures = 0;

            cJSON *tree = data_tree(data);
            if (g_log.level <= LVL_DEBUG) {
                char *json = cJSON_PrintUnformatted(tree);
                log_debug("data: %s", json ? json : "");
                free(json);
            }
            if (cfg.export_file_abspath)
                export_file(&cfg, tree);
            if (mosq)
                export_mqtt(mosq, &cfg, tree);
            cJSON_Delete(tree);
        } else {
            if (failures++ == 0)
                log_error("%s: %s", pylon_strerror(rc), pylon_error(pylon));
            else
                log_debug("%s: %s", pylon_strerror(rc), pylon_error(pylon));
        }

        sleep_until(start + cfg.interval);
    }

    if (g_stop)
        printf("Aborted by signal!\n");

    if (mosq) {
        if (cfg.mqtt_status_topic)
            mqtt_publish_status(mosq, &cfg, "offline");
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
    }
    g_pylon = NULL;
    pylon_free(pylon);
    free(data);
    for (size_t i = 0; i < g_last.n; i++) {
        free(g_last.v[i].topic);
        free(g_last.v[i].payload);
    }
    free(g_last.v);
    config_free(&cfg);
    return 0;
}
