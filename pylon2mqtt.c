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
 * Dependencies: libmosquitto, cJSON
 *
 * Usage: pylon2mqtt [config_file]   (default: /etc/pylon2mqtt/pylon.json)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <mosquitto.h>

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

/* protocol */
#define PROTOCOL_VERSION 0x20
#define CID1_BATTERY 0x46
#define CID2_GET_ANALOG_VALUE 0x42
#define CID2_GET_ALARM_INFO 0x44
#define CID2_GET_SYSTEM_PARAMETER 0x47
#define CID2_GET_CHARGE_MANAGEMENT 0x92
#define CID2_GET_SERIAL_NUMBER 0x93

#define FRAME_MAX 1024
#define MAX_BATTERIES 16
#define SERIAL_NUMBER_RETRIES 3
#define SOCKET_CONNECT_TIMEOUT_MS 5000

/* topic paths */
#define IDX_WILDCARD (-1)
#define MAX_SEG_INDEXES 4
#define MAX_WILDCARDS 8

static volatile sig_atomic_t g_stop = 0;

/* description of the last protocol error, for the caller to log */
static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

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
 * Numbers with Python semantics: the data tree holds ints and floats like
 * the Python library, floats are formatted like Python's repr() (e.g. "6.0").
 */

/* equivalent of Python's repr(float) for finite values */
static void format_py_float(double v, char *out, size_t sz)
{
    char buf[40];
    int prec;

    if (!isfinite(v)) {
        snprintf(out, sz, "%s", isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf"));
        return;
    }
    if (v == 0.0) {
        snprintf(out, sz, "%s", signbit(v) ? "-0.0" : "0.0");
        return;
    }

    /* shortest representation that round-trips */
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, v);
        if (strtod(buf, NULL) == v)
            break;
    }

    /* split "-d.ddde+XX" into sign, digits and exponent */
    const char *p = buf;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    }
    char digits[24];
    int nd = 0;
    for (; *p && *p != 'e'; p++)
        if (*p != '.')
            digits[nd++] = *p;
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    digits[nd] = '\0';
    int exp10 = atoi(p + 1);
    int decpt = exp10 + 1; /* value = 0.digits * 10^decpt */

    char res[64];
    int pos = 0;
    if (neg)
        res[pos++] = '-';
    if (decpt <= -4 || decpt > 16) {
        res[pos++] = digits[0];
        if (nd > 1) {
            res[pos++] = '.';
            memcpy(res + pos, digits + 1, (size_t)nd - 1);
            pos += nd - 1;
        }
        pos += snprintf(res + pos, sizeof(res) - (size_t)pos, "e%c%02d", exp10 < 0 ? '-' : '+',
                        abs(exp10));
    } else if (decpt <= 0) {
        res[pos++] = '0';
        res[pos++] = '.';
        for (int i = 0; i < -decpt; i++)
            res[pos++] = '0';
        memcpy(res + pos, digits, (size_t)nd);
        pos += nd;
    } else if (decpt >= nd) {
        memcpy(res + pos, digits, (size_t)nd);
        pos += nd;
        for (int i = nd; i < decpt; i++)
            res[pos++] = '0';
        res[pos++] = '.';
        res[pos++] = '0';
    } else {
        memcpy(res + pos, digits, (size_t)decpt);
        pos += decpt;
        res[pos++] = '.';
        memcpy(res + pos, digits + decpt, (size_t)(nd - decpt));
        pos += nd - decpt;
    }
    res[pos] = '\0';
    snprintf(out, sz, "%s", res);
}

/* equivalent of Python's round(v, ndigits) (correctly rounded, half to even) */
static double py_round(double v, int ndigits)
{
    char buf[400];

    if (!isfinite(v))
        return v;
    snprintf(buf, sizeof(buf), "%.*f", ndigits, v);
    return strtod(buf, NULL);
}

/* floats are stored as raw JSON (Python formatting), ints as cJSON numbers */
static cJSON *float_item(double v)
{
    char buf[40];
    format_py_float(v, buf, sizeof(buf));
    return cJSON_CreateRaw(buf);
}

static void add_float(cJSON *obj, const char *name, double v)
{
    cJSON_AddItemToObject(obj, name, float_item(v));
}

static void add_int(cJSON *obj, const char *name, long v)
{
    cJSON_AddNumberToObject(obj, name, (double)v);
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
        !cfg_int(r, file, "battery_count", 0, 0, MAX_BATTERIES, &cfg->battery_count) ||
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
 * Transport: serial port (8N1, RTS as transmit enable like python-pylontech)
 * or TCP for RS485 to Ethernet adapters ("socket://host:port")
 */

typedef struct {
    int fd;
    bool is_serial;
    uint8_t rx[512];
    size_t rx_pos;
    size_t rx_len;
} transport_t;

#define RX_TIMEOUT (-1)
#define RX_STOP (-2)
#define RX_ERROR (-3)

static double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

static int serial_open(const char *port, int baudrate)
{
    speed_t speed = baud_to_speed(baudrate);
    if (!speed) {
        set_error("unsupported baudrate %d", baudrate);
        return -1;
    }

    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        set_error("%s", strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        set_error("could not configure port: %s", strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CMSPAR | CRTSCTS);
    tio.c_cflag |= CS8;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        set_error("could not configure port: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* DTR on, RTS (transmit enable) off = receive; errors ignored, e.g. on ptys */
    int bits = TIOCM_DTR;
    ioctl(fd, TIOCMBIS, &bits);
    bits = TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &bits);

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int socket_open(const char *hostport)
{
    char host[256];
    const char *colon = strrchr(hostport, ':');
    size_t n = colon ? (size_t)(colon - hostport) : 0;

    if (n == 0 || n >= sizeof(host) || !colon[1]) {
        set_error("invalid socket address (expected socket://host:port)");
        return -1;
    }
    memcpy(host, hostport, n);
    host[n] = '\0';
    if (host[0] == '[' && host[n - 1] == ']') {
        memmove(host, host + 1, n - 2);
        host[n - 2] = '\0';
    }

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res, *ai;
    int rc = getaddrinfo(host, colon + 1, &hints, &res);
    if (rc != 0) {
        set_error("could not resolve host: %s", gai_strerror(rc));
        return -1;
    }

    int fd = -1, err = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    ai->ai_protocol);
        if (fd < 0) {
            err = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        err = errno;
        if (err == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, SOCKET_CONNECT_TIMEOUT_MS) == 1) {
                socklen_t l = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0)
                    err = errno;
                if (err == 0)
                    break;
            } else {
                err = ETIMEDOUT;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        set_error("could not connect: %s", strerror(err));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static bool transport_open(transport_t *tp, const config_t *cfg)
{
    const char *addr = strip_prefix(cfg->port, "socket://");

    tp->is_serial = addr == cfg->port;
    tp->rx_pos = tp->rx_len = 0;
    tp->fd = tp->is_serial ? serial_open(cfg->port, cfg->baudrate) : socket_open(addr);
    if (tp->fd < 0)
        return false;
    log_info("Opened %s", cfg->port);
    return true;
}

static void transport_close(transport_t *tp)
{
    if (tp->fd >= 0)
        close(tp->fd);
    tp->fd = -1;
}

/* discard pending input */
static void transport_flush(transport_t *tp)
{
    uint8_t buf[256];

    if (tp->is_serial)
        tcflush(tp->fd, TCIFLUSH);
    while (read(tp->fd, buf, sizeof(buf)) > 0)
        ;
    tp->rx_pos = tp->rx_len = 0;
}

static bool transport_write(transport_t *tp, const char *data, size_t len)
{
    int rts = TIOCM_RTS;
    size_t off = 0;
    bool ok = true;

    if (tp->is_serial)
        ioctl(tp->fd, TIOCMBIS, &rts);
    while (off < len) {
        ssize_t n = write(tp->fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pfd = { .fd = tp->fd, .events = POLLOUT };
                if (poll(&pfd, 1, 1000) == 0) {
                    set_error("write timeout");
                    ok = false;
                    break;
                }
                continue;
            }
            set_error("write failed: %s", strerror(errno));
            ok = false;
            break;
        }
        off += (size_t)n;
    }
    if (tp->is_serial) {
        tcdrain(tp->fd);
        ioctl(tp->fd, TIOCMBIC, &rts);
    }
    return ok;
}

static int transport_read_byte(transport_t *tp, double deadline)
{
    if (tp->rx_pos < tp->rx_len)
        return tp->rx[tp->rx_pos++];

    for (;;) {
        if (g_stop)
            return RX_STOP;
        double left = deadline - now_monotonic();
        if (left <= 0)
            return RX_TIMEOUT;

        struct pollfd pfd = { .fd = tp->fd, .events = POLLIN };
        int r = poll(&pfd, 1, (int)ceil(left * 1000.0));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            set_error("poll failed: %s", strerror(errno));
            return RX_ERROR;
        }
        if (r == 0)
            return RX_TIMEOUT;

        ssize_t n = read(tp->fd, tp->rx, sizeof(tp->rx));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            set_error("read failed: %s", strerror(errno));
            return RX_ERROR;
        }
        if (n == 0) {
            set_error("connection closed (device disconnected?)");
            return RX_ERROR;
        }
        tp->rx_pos = 1;
        tp->rx_len = (size_t)n;
        return tp->rx[0];
    }
}

/*
 * Protocol: frames are "~" + ASCII hex + "\r":
 * VER ADR CID1 CID2/RTN LENGTH INFO CHKSUM
 */

static unsigned long hex_value(const char *s, size_t n)
{
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        v = v * 16 + (unsigned long)(isdigit((unsigned char)c) ? c - '0' : (toupper((unsigned char)c) - 'A' + 10));
    }
    return v;
}

static unsigned frame_checksum(const char *s, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (unsigned char)s[i];
    return (~sum + 1) & 0xffff;
}

/* LENGTH field: 12 bit LENID plus 4 bit LCHKSUM */
static unsigned length_field(unsigned lenid)
{
    unsigned cs = (lenid & 0xf) + ((lenid >> 4) & 0xf) + ((lenid >> 8) & 0xf);
    cs = ((~cs) & 0xf) + 1;
    return ((cs << 12) + lenid) & 0xffff;
}

static const char *rtn_name(unsigned rtn)
{
    switch (rtn) {
    case 0x01: return "VER error";
    case 0x02: return "CHKSUM error";
    case 0x03: return "LCHKSUM error";
    case 0x04: return "CID2 invalid";
    case 0x05: return "command format error";
    case 0x06: return "invalid data";
    case 0x90: return "ADR error";
    case 0x91: return "communication error";
    default: return "unknown error";
    }
}

/* receive the next "~...\r" frame, without "~" and "\r" */
static int receive_frame(transport_t *tp, char *buf, size_t size, size_t *len, double timeout)
{
    double deadline = now_monotonic() + timeout;
    size_t n = 0;
    int c;

    do {
        c = transport_read_byte(tp, deadline);
        if (c < 0)
            return c;
    } while (c != '~');

    deadline = now_monotonic() + timeout;
    for (;;) {
        c = transport_read_byte(tp, deadline);
        if (c < 0)
            return c;
        if (c == '\r')
            break;
        if (c == '~') {
            n = 0; /* start over at the new start byte */
            continue;
        }
        if (n + 1 >= size) {
            set_error("response too long");
            return RX_ERROR;
        }
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    *len = n;
    return 0;
}

static bool parse_response(const char *f, size_t len, int *adr, char *payload, size_t size,
                           size_t *plen)
{
    if (len < 16) {
        set_error("response too short (%zu characters)", len);
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)f[i])) {
            set_error("invalid character in response");
            return false;
        }
    }
    unsigned chksum = (unsigned)hex_value(f + len - 4, 4);
    unsigned calc = frame_checksum(f, len - 4);
    if (chksum != calc) {
        set_error("checksum error: expected %04X, received %04X", calc, chksum);
        return false;
    }

    unsigned cid1 = (unsigned)hex_value(f + 4, 2);
    unsigned rtn = (unsigned)hex_value(f + 6, 2);
    size_t lenid = hex_value(f + 8, 4) & 0x0fff;
    if (cid1 != CID1_BATTERY) {
        set_error("unexpected CID1 0x%02X in response", cid1);
        return false;
    }
    if (rtn != 0) {
        set_error("battery returned error 0x%02X (%s)", rtn, rtn_name(rtn));
        return false;
    }
    if (12 + lenid > len - 4 || lenid >= size) {
        set_error("response truncated (LENGTH %zu, received %zu)", lenid, len - 16);
        return false;
    }
    *adr = (int)hex_value(f + 2, 2);
    memcpy(payload, f + 12, lenid);
    payload[lenid] = '\0';
    *plen = lenid;
    return true;
}

/* send a command and receive the response payload (INFO); on error see g_error */
static bool pylon_request(transport_t *tp, double timeout, int adr, int cid2, const char *info,
                          int *resp_adr, char *payload, size_t size, size_t *plen)
{
    char body[128], cmd[160];
    static char frame[FRAME_MAX];
    size_t len;

    int blen = snprintf(body, sizeof(body), "%02X%02X%02X%02X%04X%s", PROTOCOL_VERSION, adr,
                        CID1_BATTERY, cid2, length_field((unsigned)strlen(info)), info);
    int clen = snprintf(cmd, sizeof(cmd), "~%s%04X\r", body, frame_checksum(body, (size_t)blen));

    transport_flush(tp);
    log_debug("-> %.*s", clen - 1, cmd);
    if (!transport_write(tp, cmd, (size_t)clen))
        return false;

    for (;;) {
        int rc = receive_frame(tp, frame, sizeof(frame), &len, timeout);
        if (rc == RX_TIMEOUT)
            set_error("no response from address %d (timeout)", adr);
        if (rc == RX_STOP)
            set_error("stopped");
        if (rc != 0)
            return false;
        log_debug("<- ~%s", frame);
        /* adapters without echo suppression */
        if (len == (size_t)clen - 2 && memcmp(frame, cmd + 1, len) == 0)
            continue;
        break;
    }
    return parse_response(frame, len, resp_adr, payload, size, plen);
}

/*
 * Payload decoding (field names and scaling like python-pylontech)
 */

typedef struct {
    const char *p;
    size_t len;
    size_t pos;
    bool err;
} hexrd_t;

static unsigned long hr_uint(hexrd_t *r, int nbytes)
{
    size_t n = (size_t)nbytes * 2;
    if (r->pos + n > r->len) {
        r->err = true;
        r->pos = r->len;
        return 0;
    }
    unsigned long v = hex_value(r->p + r->pos, n);
    r->pos += n;
    return v;
}

static long hr_int16(hexrd_t *r)
{
    unsigned long v = hr_uint(r, 2);
    return v & 0x8000 ? (long)v - 0x10000 : (long)v;
}

static bool hr_available(const hexrd_t *r, int nbytes)
{
    return r->pos + (size_t)nbytes * 2 <= r->len;
}

/* ASCII string, trailing NULs stripped, non-printable characters replaced */
static void hr_ascii(hexrd_t *r, int nbytes, char *out, size_t size)
{
    size_t n = 0;
    for (int i = 0; i < nbytes; i++) {
        unsigned long c = hr_uint(r, 1);
        if (n + 1 < size)
            out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == '\0')
        n--;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] >= 0x7f)
            out[i] = '?';
    out[n] = '\0';
}

static double cell_voltage(long v) { return (double)v / 1000.0; }
static double module_voltage(unsigned long v) { return (double)v / 1000.0; }
static double module_current(long v) { return (double)v / 10.0; }
static double temperature(long v) { return (double)(v - 2731) / 10.0; }

static const char *alarm_name(unsigned long v)
{
    switch (v) {
    case 0: return "Ok";
    case 1: return "BelowLimit";
    case 2: return "AboveLimit";
    default: return "OtherError";
    }
}

typedef struct {
    double voltage;
    double current;
    double remain;
    double total;
    double cell_min;
    double cell_max;
    int cells;
} analog_summary_t;

static cJSON *decode_analog_value(const char *payload, size_t len, int adr, analog_summary_t *sum)
{
    hexrd_t r = { payload, len, 0, false };
    cJSON *o = cJSON_CreateObject();

    add_int(o, "ADR", adr);
    add_int(o, "InfoFlag", (long)hr_uint(&r, 1));
    add_int(o, "CommandValue", (long)hr_uint(&r, 1));
    int cells = (int)hr_uint(&r, 1);
    add_int(o, "CellCount", cells);
    cJSON *arr = cJSON_AddArrayToObject(o, "CellVoltages");
    sum->cells = 0;
    for (int i = 0; i < cells && !r.err; i++) {
        double v = cell_voltage(hr_int16(&r));
        cJSON_AddItemToArray(arr, float_item(v));
        if (sum->cells++ == 0 || v < sum->cell_min)
            sum->cell_min = v;
        if (sum->cells == 1 || v > sum->cell_max)
            sum->cell_max = v;
    }
    int temps = (int)hr_uint(&r, 1);
    add_int(o, "TemperatureCount", temps);
    arr = cJSON_AddArrayToObject(o, "Temperatures");
    for (int i = 0; i < temps && !r.err; i++)
        cJSON_AddItemToArray(arr, float_item(temperature(hr_int16(&r))));
    sum->current = module_current(hr_int16(&r));
    sum->voltage = module_voltage(hr_uint(&r, 2));
    sum->remain = (double)hr_uint(&r, 2) / 1000.0;
    bool large = hr_uint(&r, 1) == 4; /* user defined item count: 4 = 3 byte capacities */
    sum->total = (double)hr_uint(&r, 2) / 1000.0;
    long cycles = (long)hr_uint(&r, 2);
    if (large) {
        sum->remain = (double)hr_uint(&r, 3) / 1000.0;
        sum->total = (double)hr_uint(&r, 3) / 1000.0;
    }
    if (r.err) {
        set_error("analog value response too short (%zu characters)", len);
        cJSON_Delete(o);
        return NULL;
    }
    add_float(o, "Current", sum->current);
    add_float(o, "Voltage", sum->voltage);
    add_float(o, "RemainCapacity", sum->remain);
    cJSON_AddStringToObject(o, "CapDetect", large ? ">65Ah" : "<=65Ah");
    add_float(o, "ModuleTotalCapacity", sum->total);
    add_int(o, "CycleNumber", cycles);
    return o;
}

static cJSON *decode_charge_management(const char *payload, size_t len, int adr)
{
    hexrd_t r = { payload, len, 0, false };

    if (len < 20) {
        set_error("charge/discharge management response too short (%zu characters)", len);
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    add_int(o, "ADR", adr);
    add_int(o, "CommandValue", (long)hr_uint(&r, 1));
    add_float(o, "ChargeVoltage", module_voltage(hr_uint(&r, 2)));
    add_float(o, "DischargeVoltage", module_voltage(hr_uint(&r, 2)));
    add_float(o, "ChargeCurrent", module_current(hr_int16(&r)));
    add_float(o, "DischargeCurrent", module_current(hr_int16(&r)));
    unsigned long status = hr_uint(&r, 1);
    cJSON_AddBoolToObject(o, "StatusChargeEnable", status & 0x80);
    cJSON_AddBoolToObject(o, "StatusDischargeEnable", status & 0x40);
    cJSON_AddBoolToObject(o, "StatusChargeImmediately1", status & 0x20);
    cJSON_AddBoolToObject(o, "StatusChargeImmediately2", status & 0x10);
    cJSON_AddBoolToObject(o, "StatusFullChargeRequired", status & 0x08);
    return o;
}

static cJSON *decode_alarm_info(const char *payload, size_t len, int adr)
{
    hexrd_t r = { payload, len, 0, false };
    cJSON *o = cJSON_CreateObject();

    add_int(o, "ADR", adr);
    add_int(o, "InfoFlag", (long)hr_uint(&r, 1));
    add_int(o, "CommandValue", (long)hr_uint(&r, 1));
    int cells = (int)hr_uint(&r, 1);
    add_int(o, "CellCount", cells);
    cJSON *arr = cJSON_AddArrayToObject(o, "CellAlarm");
    for (int i = 0; i < cells && !r.err; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(alarm_name(hr_uint(&r, 1))));
    int temps = (int)hr_uint(&r, 1);
    add_int(o, "TemperatureCount", temps);
    arr = cJSON_AddArrayToObject(o, "TemperatureAlarm");
    for (int i = 0; i < temps && !r.err; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(alarm_name(hr_uint(&r, 1))));
    cJSON_AddStringToObject(o, "ChargeCurrentAlarm", alarm_name(hr_uint(&r, 1)));
    cJSON_AddStringToObject(o, "ModuleVoltageAlarm", alarm_name(hr_uint(&r, 1)));
    cJSON_AddStringToObject(o, "DischargeCurrentAlarm", alarm_name(hr_uint(&r, 1)));
    if (r.err) {
        set_error("alarm info response too short (%zu characters)", len);
        cJSON_Delete(o);
        return NULL;
    }
    /* the status bytes are optional, only present in newer firmware */
    for (int i = 1; i <= 5 && hr_available(&r, 1); i++) {
        char name[16];
        snprintf(name, sizeof(name), "Status%d", i);
        add_int(o, name, (long)hr_uint(&r, 1));
    }
    return o;
}

/*
 * Decoded as in the protocol specification. python-pylontech is off by one
 * byte here (it misses the INFOFLAG), so its values are garbage.
 */
static cJSON *decode_system_parameter(const char *payload, size_t len, int adr)
{
    hexrd_t r = { payload, len, 0, false };

    if (len < 50) {
        set_error("system parameter response too short (%zu characters)", len);
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    add_int(o, "ADR", adr);
    add_int(o, "InfoFlag", (long)hr_uint(&r, 1));
    add_float(o, "CellHighVoltageLimit", cell_voltage(hr_int16(&r)));
    add_float(o, "CellLowVoltageLimit", cell_voltage(hr_int16(&r)));
    add_float(o, "CellUnderVoltageLimit", cell_voltage(hr_int16(&r)));
    add_float(o, "ChargeHighTemperatureLimit", temperature(hr_int16(&r)));
    add_float(o, "ChargeLowTemperatureLimit", temperature(hr_int16(&r)));
    add_float(o, "ChargeCurrentLimit", module_current(hr_int16(&r)));
    add_float(o, "ModuleHighVoltageLimit", module_voltage(hr_uint(&r, 2)));
    add_float(o, "ModuleLowVoltageLimit", module_voltage(hr_uint(&r, 2)));
    add_float(o, "ModuleUnderVoltageLimit", module_voltage(hr_uint(&r, 2)));
    add_float(o, "DischargeHighTemperatureLimit", temperature(hr_int16(&r)));
    add_float(o, "DischargeLowTemperatureLimit", temperature(hr_int16(&r)));
    add_float(o, "DischargeCurrentLimit", module_current(hr_int16(&r)));
    return o;
}

/*
 * Battery stack polling (like PylontechStack)
 */

typedef struct {
    transport_t tp;
    cJSON *serials; /* NULL: not detected yet */
    int battcount;
} pylon_stack_t;

static int battery_address(const config_t *cfg, int batt)
{
    return 2 + batt + (cfg->group << 4);
}

static bool battery_request(pylon_stack_t *st, const config_t *cfg, int batt, int cid2,
                            int *resp_adr, char *payload, size_t size, size_t *plen)
{
    int adr = battery_address(cfg, batt);
    char info[16];
    snprintf(info, sizeof(info), "%02X%02X", adr & 0xff, (batt + 1) & 0xff);
    return pylon_request(&st->tp, cfg->timeout, adr, cid2, info, resp_adr, payload, size, plen);
}

static bool poll_serial_number(pylon_stack_t *st, const config_t *cfg, int batt, char *out,
                               size_t size)
{
    static char payload[FRAME_MAX];
    size_t plen;
    int adr;

    if (!battery_request(st, cfg, batt, CID2_GET_SERIAL_NUMBER, &adr, payload, sizeof(payload),
                         &plen))
        return false;
    if (plen < 34) {
        set_error("serial number response too short (%zu characters)", plen);
        return false;
    }
    hexrd_t r = { payload, plen, 2, false };
    hr_ascii(&r, 16, out, size);
    return true;
}

/* poll the serial numbers to find the batteries */
static bool stack_detect(pylon_stack_t *st, const config_t *cfg)
{
    int limit = cfg->battery_count ? cfg->battery_count : MAX_BATTERIES;
    cJSON *serials = cJSON_CreateArray();

    for (int b = 0; b < limit; b++) {
        char serial[32];
        bool ok = false;
        for (int retry = 0; retry < SERIAL_NUMBER_RETRIES && !ok && !g_stop; retry++) {
            ok = poll_serial_number(st, cfg, b, serial, sizeof(serial));
            if (!ok)
                log_debug("Battery %d: serial number request failed: %s", b, g_error);
        }
        if (g_stop) {
            cJSON_Delete(serials);
            return false;
        }
        if (!ok) {
            if (cfg->battery_count == 0 && b > 0)
                break; /* auto detection: end of stack */
            char msg[sizeof(g_error)];
            snprintf(msg, sizeof(msg), "%s", g_error);
            set_error("battery %d: serial number request failed: %s", b, msg);
            cJSON_Delete(serials);
            return false;
        }
        cJSON_AddItemToArray(serials, cJSON_CreateString(serial));
        log_info("Battery %d: serial number %s", b, serial);
    }

    st->serials = serials;
    st->battcount = cJSON_GetArraySize(serials);
    log_info("number of batteries found: %d", st->battcount);
    return true;
}

static void stack_forget(pylon_stack_t *st)
{
    cJSON_Delete(st->serials);
    st->serials = NULL;
    st->battcount = 0;
}

/* poll all batteries; returns the data tree or NULL (see g_error) */
static cJSON *stack_update(pylon_stack_t *st, const config_t *cfg)
{
    static char payload[FRAME_MAX];
    size_t plen;
    int adr;
    double total = 0, remain = 0, power = 0, cell_min = 0, cell_max = 0;
    int cells = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "SerialNumbers", cJSON_Duplicate(st->serials, true));
    cJSON *analog = cJSON_AddArrayToObject(root, "Analog");
    cJSON *charge = cJSON_AddArrayToObject(root, "ChargeDischargeManagement");
    cJSON *alarm = cJSON_AddArrayToObject(root, "AlarmInfo");

    for (int b = 0; b < st->battcount; b++) {
        analog_summary_t sum;
        cJSON *o;

        if (!battery_request(st, cfg, b, CID2_GET_ANALOG_VALUE, &adr, payload, sizeof(payload),
                             &plen) ||
            !(o = decode_analog_value(payload, plen, adr, &sum)))
            goto fail;
        cJSON_AddItemToArray(analog, o);
        remain += sum.remain;
        total += sum.total;
        power += sum.voltage * sum.current;
        if (sum.cells > 0) {
            if (cells == 0 || sum.cell_min < cell_min)
                cell_min = sum.cell_min;
            if (cells == 0 || sum.cell_max > cell_max)
                cell_max = sum.cell_max;
            cells += sum.cells;
        }

        if (!battery_request(st, cfg, b, CID2_GET_CHARGE_MANAGEMENT, &adr, payload,
                             sizeof(payload), &plen) ||
            !(o = decode_charge_management(payload, plen, adr)))
            goto fail;
        cJSON_AddItemToArray(charge, o);

        if (!battery_request(st, cfg, b, CID2_GET_ALARM_INFO, &adr, payload, sizeof(payload),
                             &plen) ||
            !(o = decode_alarm_info(payload, plen, adr)))
            goto fail;
        cJSON_AddItemToArray(alarm, o);
    }

    cJSON *o;
    if (!pylon_request(&st->tp, cfg->timeout, battery_address(cfg, 0), CID2_GET_SYSTEM_PARAMETER,
                       "", &adr, payload, sizeof(payload), &plen) ||
        !(o = decode_system_parameter(payload, plen, adr)))
        goto fail;
    cJSON_AddItemToObject(root, "SystemParameter", o);

    cJSON *c = cJSON_AddObjectToObject(root, "Calculated");
    add_float(c, "TotalCapacity_Ah", total);
    add_float(c, "RemainCapacity_Ah", remain);
    if (total > 0)
        add_float(c, "Remain_Percent", py_round(remain / total * 100, 0));
    else
        cJSON_AddNullToObject(c, "Remain_Percent");
    double power_w = py_round(power, 3);
    add_float(c, "Power_W", power_w);
    if (power_w > 0) {
        add_float(c, "ChargePower_W", power_w);
        add_int(c, "DischargePower_W", 0);
    } else {
        add_int(c, "ChargePower_W", 0);
        add_float(c, "DischargePower_W", -1.0 * power_w);
    }
    if (cells > 0) {
        add_float(c, "CellVoltageMin", cell_min);
        add_float(c, "CellVoltageMax", cell_max);
        add_float(c, "CellVoltageDelta", py_round(cell_max - cell_min, 3));
    }
    return root;

fail:
    cJSON_Delete(root);
    return NULL;
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
 * Main
 */

static void signal_handler(int sig)
{
    (void)sig;
    g_stop = 1;
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

    pylon_stack_t st = { .tp = { .fd = -1 } };
    if (!transport_open(&st.tp, &cfg)) {
        log_error("Could not open %s: %s", cfg.port, g_error);
        if (g_log.fp != stderr)
            fprintf(stderr, "Could not open %s: %s\n", cfg.port, g_error);
        config_free(&cfg);
        return 1;
    }

    /* connect to mqtt broker */
    struct mosquitto *mosq = NULL;
    if (cfg.mqtt_server) {
        /* keep signals away from the mosquitto thread, so they interrupt the serial poll */
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &block, &old);
        mosq = mqtt_start(&cfg);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (!mosq) {
            transport_close(&st.tp);
            config_free(&cfg);
            return EXIT_MQTT;
        }
    }

    /* errors are logged once, until the communication works again */
    int failures = 0;

    while (!g_stop) {
        double start = now_monotonic();
        cJSON *tree = NULL;
        const char *what = NULL;

        if (st.tp.fd < 0 && !transport_open(&st.tp, &cfg))
            what = "Could not open port";
        else if (!st.serials && !stack_detect(&st, &cfg))
            what = "Pylontech detection error";
        else if (!(tree = stack_update(&st, &cfg)))
            what = "Pylontech update error";

        if (g_stop) {
            cJSON_Delete(tree);
            break;
        }

        if (tree) {
            if (failures > 0)
                log_info("Communication with battery restored after %d failed attempts", failures);
            failures = 0;

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
                log_error("%s: %s", what, g_error);
            else
                log_debug("%s: %s", what, g_error);
            /* reconnect and detect the batteries again, like the Python version */
            transport_close(&st.tp);
            stack_forget(&st);
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
    transport_close(&st.tp);
    stack_forget(&st);
    for (size_t i = 0; i < g_last.n; i++) {
        free(g_last.v[i].topic);
        free(g_last.v[i].payload);
    }
    free(g_last.v);
    config_free(&cfg);
    return 0;
}
