/*
 * libpylontech - Pylontech battery (RS485) protocol library
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
 */

#define _GNU_SOURCE

#include "pylontech.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* protocol */
#define PROTOCOL_VERSION 0x20
#define CID1_BATTERY 0x46
#define CID2_GET_ANALOG_VALUE 0x42
#define CID2_GET_ALARM_INFO 0x44
#define CID2_GET_SYSTEM_PARAMETER 0x47
#define CID2_GET_CHARGE_MANAGEMENT 0x92
#define CID2_GET_SERIAL_NUMBER 0x93

#define FRAME_MAX 1024
#define SERIAL_NUMBER_RETRIES 3
#define SOCKET_CONNECT_TIMEOUT_MS 5000

#define RX_TIMEOUT (-1)
#define RX_STOP (-2)
#define RX_ERROR (-3)

struct pylon {
    char *port;
    int baudrate;
    int battery_count; /* 0: auto detect */
    int group;
    double timeout;
    pylon_log_fn log;
    void *log_ctx;
    int log_level;

    /* transport: serial port (8N1, RTS as transmit enable like python-pylontech)
     * or TCP for RS485 to Ethernet adapters ("socket://host:port") */
    int fd;
    bool is_serial;
    uint8_t rx[512];
    size_t rx_pos;
    size_t rx_len;

    /* pylon_abort(): flag plus pipe to wake up poll() */
    volatile sig_atomic_t aborted;
    int abort_pipe[2];

    /* detected batteries */
    bool detected;
    int battcount;
    char serials[PYLON_MAX_BATTERIES][17];

    char frame[FRAME_MAX];
    char payload[FRAME_MAX];

    /* description of the last error */
    char error[256];
};

static void set_error(pylon_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_error(pylon_t *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error, sizeof(p->error), fmt, ap);
    va_end(ap);
}

static void plog(pylon_t *p, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void plog(pylon_t *p, int level, const char *fmt, ...)
{
    if (!p->log || level < p->log_level)
        return;

    char *msg = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    p->log(p->log_ctx, level, msg);
    free(msg);
}

static const char *strip_prefix(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? s + n : s;
}

/*
 * Numbers with Python semantics: floats are formatted like Python's repr()
 * (e.g. "6.0"), rounding like Python's round().
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

/*
 * Transport
 */

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

static int serial_open(pylon_t *p)
{
    speed_t speed = baud_to_speed(p->baudrate);
    if (!speed) {
        set_error(p, "unsupported baudrate %d", p->baudrate);
        return -1;
    }

    int fd = open(p->port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        set_error(p, "%s", strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        set_error(p, "could not configure port: %s", strerror(errno));
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
        set_error(p, "could not configure port: %s", strerror(errno));
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

static int socket_open(pylon_t *p, const char *hostport)
{
    char host[256];
    const char *colon = strrchr(hostport, ':');
    size_t n = colon ? (size_t)(colon - hostport) : 0;

    if (n == 0 || n >= sizeof(host) || !colon[1]) {
        set_error(p, "invalid socket address (expected socket://host:port)");
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
        set_error(p, "could not resolve host: %s", gai_strerror(rc));
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
            struct pollfd pfd[2] = { { .fd = fd, .events = POLLOUT },
                                     { .fd = p->abort_pipe[0], .events = POLLIN } };
            int r = poll(pfd, 2, SOCKET_CONNECT_TIMEOUT_MS);
            if (p->aborted) {
                err = ECANCELED;
            } else if (r > 0 && pfd[0].revents) {
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
        if (p->aborted)
            break;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        set_error(p, "could not connect: %s", strerror(err));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void transport_close(pylon_t *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}

/* discard pending input */
static void transport_flush(pylon_t *p)
{
    uint8_t buf[256];

    if (p->is_serial)
        tcflush(p->fd, TCIFLUSH);
    while (read(p->fd, buf, sizeof(buf)) > 0)
        ;
    p->rx_pos = p->rx_len = 0;
}

static bool transport_write(pylon_t *p, const char *data, size_t len)
{
    int rts = TIOCM_RTS;
    size_t off = 0;
    bool ok = true;

    if (p->is_serial)
        ioctl(p->fd, TIOCMBIS, &rts);
    while (off < len) {
        ssize_t n = write(p->fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pfd = { .fd = p->fd, .events = POLLOUT };
                if (poll(&pfd, 1, 1000) == 0) {
                    set_error(p, "write timeout");
                    ok = false;
                    break;
                }
                continue;
            }
            set_error(p, "write failed: %s", strerror(errno));
            ok = false;
            break;
        }
        off += (size_t)n;
    }
    if (p->is_serial) {
        tcdrain(p->fd);
        ioctl(p->fd, TIOCMBIC, &rts);
    }
    return ok;
}

static int transport_read_byte(pylon_t *p, double deadline)
{
    if (p->rx_pos < p->rx_len)
        return p->rx[p->rx_pos++];

    for (;;) {
        if (p->aborted)
            return RX_STOP;
        double left = deadline - now_monotonic();
        if (left <= 0)
            return RX_TIMEOUT;

        struct pollfd pfd[2] = { { .fd = p->fd, .events = POLLIN },
                                 { .fd = p->abort_pipe[0], .events = POLLIN } };
        int r = poll(pfd, 2, (int)ceil(left * 1000.0));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            set_error(p, "poll failed: %s", strerror(errno));
            return RX_ERROR;
        }
        if (r == 0)
            return RX_TIMEOUT;
        if (!pfd[0].revents)
            continue; /* abort */

        ssize_t n = read(p->fd, p->rx, sizeof(p->rx));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            set_error(p, "read failed: %s", strerror(errno));
            return RX_ERROR;
        }
        if (n == 0) {
            set_error(p, "connection closed (device disconnected?)");
            return RX_ERROR;
        }
        p->rx_pos = 1;
        p->rx_len = (size_t)n;
        return p->rx[0];
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
static int receive_frame(pylon_t *p, size_t *len)
{
    double deadline = now_monotonic() + p->timeout;
    size_t n = 0;
    int c;

    do {
        c = transport_read_byte(p, deadline);
        if (c < 0)
            return c;
    } while (c != '~');

    deadline = now_monotonic() + p->timeout;
    for (;;) {
        c = transport_read_byte(p, deadline);
        if (c < 0)
            return c;
        if (c == '\r')
            break;
        if (c == '~') {
            n = 0; /* start over at the new start byte */
            continue;
        }
        if (n + 1 >= sizeof(p->frame)) {
            set_error(p, "response too long");
            return RX_ERROR;
        }
        p->frame[n++] = (char)c;
    }
    p->frame[n] = '\0';
    *len = n;
    return 0;
}

/* check the frame and copy its INFO to p->payload */
static bool parse_response(pylon_t *p, size_t len, int *adr, size_t *plen)
{
    const char *f = p->frame;

    if (len < 16) {
        set_error(p, "response too short (%zu characters)", len);
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)f[i])) {
            set_error(p, "invalid character in response");
            return false;
        }
    }
    unsigned chksum = (unsigned)hex_value(f + len - 4, 4);
    unsigned calc = frame_checksum(f, len - 4);
    if (chksum != calc) {
        set_error(p, "checksum error: expected %04X, received %04X", calc, chksum);
        return false;
    }

    unsigned cid1 = (unsigned)hex_value(f + 4, 2);
    unsigned rtn = (unsigned)hex_value(f + 6, 2);
    size_t lenid = hex_value(f + 8, 4) & 0x0fff;
    if (cid1 != CID1_BATTERY) {
        set_error(p, "unexpected CID1 0x%02X in response", cid1);
        return false;
    }
    if (rtn != 0) {
        set_error(p, "battery returned error 0x%02X (%s)", rtn, rtn_name(rtn));
        return false;
    }
    if (12 + lenid > len - 4 || lenid >= sizeof(p->payload)) {
        set_error(p, "response truncated (LENGTH %zu, received %zu)", lenid, len - 16);
        return false;
    }
    *adr = (int)hex_value(f + 2, 2);
    memcpy(p->payload, f + 12, lenid);
    p->payload[lenid] = '\0';
    *plen = lenid;
    return true;
}

/* send a command and receive the response payload (INFO) into p->payload */
static bool pylon_request(pylon_t *p, int adr, int cid2, const char *info, int *resp_adr,
                          size_t *plen)
{
    char body[128], cmd[160];
    size_t len;

    int blen = snprintf(body, sizeof(body), "%02X%02X%02X%02X%04X%s", PROTOCOL_VERSION, adr,
                        CID1_BATTERY, cid2, length_field((unsigned)strlen(info)), info);
    int clen = snprintf(cmd, sizeof(cmd), "~%s%04X\r", body, frame_checksum(body, (size_t)blen));

    transport_flush(p);
    plog(p, PYLON_LOG_DEBUG, "-> %.*s", clen - 1, cmd);
    if (!transport_write(p, cmd, (size_t)clen))
        return false;

    for (;;) {
        int rc = receive_frame(p, &len);
        if (rc == RX_TIMEOUT)
            set_error(p, "no response from address %d (timeout)", adr);
        if (rc == RX_STOP)
            set_error(p, "stopped");
        if (rc != 0)
            return false;
        plog(p, PYLON_LOG_DEBUG, "<- ~%s", p->frame);
        /* adapters without echo suppression */
        if (len == (size_t)clen - 2 && memcmp(p->frame, cmd + 1, len) == 0)
            continue;
        break;
    }
    return parse_response(p, len, resp_adr, plen);
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

static pylon_alarm_t alarm_value(unsigned long v)
{
    return v <= PYLON_ALARM_ABOVE_LIMIT ? (pylon_alarm_t)v : PYLON_ALARM_OTHER_ERROR;
}

/* number of cells or temperatures, limited to the size of the arrays */
static bool hr_count(pylon_t *p, hexrd_t *r, int max, const char *what, int *out)
{
    *out = (int)hr_uint(r, 1);
    if (*out > max) {
        set_error(p, "too many %s (%d, at most %d supported)", what, *out, max);
        return false;
    }
    return true;
}

static bool decode_analog_value(pylon_t *p, size_t len, int adr, pylon_analog_t *a)
{
    hexrd_t r = { p->payload, len, 0, false };

    a->adr = adr;
    a->info_flag = (int)hr_uint(&r, 1);
    a->command_value = (int)hr_uint(&r, 1);
    if (!hr_count(p, &r, PYLON_MAX_CELLS, "cells", &a->cell_count))
        return false;
    for (int i = 0; i < a->cell_count; i++)
        a->cell_voltages[i] = cell_voltage(hr_int16(&r));
    if (!hr_count(p, &r, PYLON_MAX_TEMPERATURES, "temperatures", &a->temperature_count))
        return false;
    for (int i = 0; i < a->temperature_count; i++)
        a->temperatures[i] = temperature(hr_int16(&r));
    a->current = module_current(hr_int16(&r));
    a->voltage = module_voltage(hr_uint(&r, 2));
    a->remain_capacity = (double)hr_uint(&r, 2) / 1000.0;
    a->large_capacity = hr_uint(&r, 1) == 4; /* user defined item count: 4 = 3 byte capacities */
    a->total_capacity = (double)hr_uint(&r, 2) / 1000.0;
    a->cycle_number = (int)hr_uint(&r, 2);
    if (a->large_capacity) {
        a->remain_capacity = (double)hr_uint(&r, 3) / 1000.0;
        a->total_capacity = (double)hr_uint(&r, 3) / 1000.0;
    }
    if (r.err) {
        set_error(p, "analog value response too short (%zu characters)", len);
        return false;
    }
    return true;
}

static bool decode_charge_management(pylon_t *p, size_t len, int adr,
                                     pylon_charge_management_t *c)
{
    hexrd_t r = { p->payload, len, 0, false };

    if (len < 20) {
        set_error(p, "charge/discharge management response too short (%zu characters)", len);
        return false;
    }
    c->adr = adr;
    c->command_value = (int)hr_uint(&r, 1);
    c->charge_voltage = module_voltage(hr_uint(&r, 2));
    c->discharge_voltage = module_voltage(hr_uint(&r, 2));
    c->charge_current = module_current(hr_int16(&r));
    c->discharge_current = module_current(hr_int16(&r));
    unsigned long status = hr_uint(&r, 1);
    c->charge_enable = status & 0x80;
    c->discharge_enable = status & 0x40;
    c->charge_immediately1 = status & 0x20;
    c->charge_immediately2 = status & 0x10;
    c->full_charge_required = status & 0x08;
    return true;
}

static bool decode_alarm_info(pylon_t *p, size_t len, int adr, pylon_alarm_info_t *a)
{
    hexrd_t r = { p->payload, len, 0, false };

    a->adr = adr;
    a->info_flag = (int)hr_uint(&r, 1);
    a->command_value = (int)hr_uint(&r, 1);
    if (!hr_count(p, &r, PYLON_MAX_CELLS, "cells", &a->cell_count))
        return false;
    for (int i = 0; i < a->cell_count; i++)
        a->cell[i] = alarm_value(hr_uint(&r, 1));
    if (!hr_count(p, &r, PYLON_MAX_TEMPERATURES, "temperatures", &a->temperature_count))
        return false;
    for (int i = 0; i < a->temperature_count; i++)
        a->temperature[i] = alarm_value(hr_uint(&r, 1));
    a->charge_current = alarm_value(hr_uint(&r, 1));
    a->module_voltage = alarm_value(hr_uint(&r, 1));
    a->discharge_current = alarm_value(hr_uint(&r, 1));
    if (r.err) {
        set_error(p, "alarm info response too short (%zu characters)", len);
        return false;
    }
    /* the status bytes are optional, only present in newer firmware */
    a->status_count = 0;
    while (a->status_count < PYLON_MAX_ALARM_STATUS && hr_available(&r, 1))
        a->status[a->status_count++] = (int)hr_uint(&r, 1);
    return true;
}

/*
 * Decoded as in the protocol specification. python-pylontech is off by one
 * byte here (it misses the INFOFLAG), so its values are garbage.
 */
static bool decode_system_parameter(pylon_t *p, size_t len, int adr, pylon_system_parameter_t *s)
{
    hexrd_t r = { p->payload, len, 0, false };

    if (len < 50) {
        set_error(p, "system parameter response too short (%zu characters)", len);
        return false;
    }
    s->adr = adr;
    s->info_flag = (int)hr_uint(&r, 1);
    s->cell_high_voltage_limit = cell_voltage(hr_int16(&r));
    s->cell_low_voltage_limit = cell_voltage(hr_int16(&r));
    s->cell_under_voltage_limit = cell_voltage(hr_int16(&r));
    s->charge_high_temperature_limit = temperature(hr_int16(&r));
    s->charge_low_temperature_limit = temperature(hr_int16(&r));
    s->charge_current_limit = module_current(hr_int16(&r));
    s->module_high_voltage_limit = module_voltage(hr_uint(&r, 2));
    s->module_low_voltage_limit = module_voltage(hr_uint(&r, 2));
    s->module_under_voltage_limit = module_voltage(hr_uint(&r, 2));
    s->discharge_high_temperature_limit = temperature(hr_int16(&r));
    s->discharge_low_temperature_limit = temperature(hr_int16(&r));
    s->discharge_current_limit = module_current(hr_int16(&r));
    return true;
}

/*
 * Battery stack polling (like PylontechStack)
 */

static int battery_address(const pylon_t *p, int batt)
{
    return 2 + batt + (p->group << 4);
}

static bool battery_request(pylon_t *p, int batt, int cid2, int *resp_adr, size_t *plen)
{
    int adr = battery_address(p, batt);
    char info[16];
    snprintf(info, sizeof(info), "%02X%02X", adr & 0xff, (batt + 1) & 0xff);
    return pylon_request(p, adr, cid2, info, resp_adr, plen);
}

static bool poll_serial_number(pylon_t *p, int batt, char *out, size_t size)
{
    size_t plen;
    int adr;

    if (!battery_request(p, batt, CID2_GET_SERIAL_NUMBER, &adr, &plen))
        return false;
    if (plen < 34) {
        set_error(p, "serial number response too short (%zu characters)", plen);
        return false;
    }
    hexrd_t r = { p->payload, plen, 2, false };
    hr_ascii(&r, 16, out, size);
    return true;
}

/* poll the serial numbers to find the batteries */
static bool stack_detect(pylon_t *p)
{
    int limit = p->battery_count ? p->battery_count : PYLON_MAX_BATTERIES;
    int count = 0;

    for (int b = 0; b < limit; b++) {
        bool ok = false;
        for (int retry = 0; retry < SERIAL_NUMBER_RETRIES && !ok && !p->aborted; retry++) {
            ok = poll_serial_number(p, b, p->serials[b], sizeof(p->serials[b]));
            if (!ok)
                plog(p, PYLON_LOG_DEBUG, "Battery %d: serial number request failed: %s", b,
                     p->error);
        }
        if (p->aborted)
            return false;
        if (!ok) {
            if (p->battery_count == 0 && b > 0)
                break; /* auto detection: end of stack */
            char msg[sizeof(p->error)];
            snprintf(msg, sizeof(msg), "%s", p->error);
            set_error(p, "battery %d: serial number request failed: %s", b, msg);
            return false;
        }
        count++;
        plog(p, PYLON_LOG_INFO, "Battery %d: serial number %s", b, p->serials[b]);
    }

    p->battcount = count;
    p->detected = true;
    plog(p, PYLON_LOG_INFO, "number of batteries found: %d", count);
    return true;
}

/* poll all batteries */
static bool stack_update(pylon_t *p, pylon_data_t *d)
{
    size_t plen;
    int adr;
    double total = 0, remain = 0, power = 0, cell_min = 0, cell_max = 0;
    int cells = 0;

    memset(d, 0, sizeof(*d));
    d->battery_count = p->battcount;

    for (int b = 0; b < p->battcount; b++) {
        pylon_battery_t *bat = &d->batteries[b];
        memcpy(bat->serial_number, p->serials[b], sizeof(bat->serial_number));

        if (!battery_request(p, b, CID2_GET_ANALOG_VALUE, &adr, &plen) ||
            !decode_analog_value(p, plen, adr, &bat->analog))
            return false;
        const pylon_analog_t *a = &bat->analog;
        remain += a->remain_capacity;
        total += a->total_capacity;
        power += a->voltage * a->current;
        for (int i = 0; i < a->cell_count; i++) {
            double v = a->cell_voltages[i];
            if (cells == 0 || v < cell_min)
                cell_min = v;
            if (cells == 0 || v > cell_max)
                cell_max = v;
            cells++;
        }

        if (!battery_request(p, b, CID2_GET_CHARGE_MANAGEMENT, &adr, &plen) ||
            !decode_charge_management(p, plen, adr, &bat->charge_management))
            return false;

        if (!battery_request(p, b, CID2_GET_ALARM_INFO, &adr, &plen) ||
            !decode_alarm_info(p, plen, adr, &bat->alarm_info))
            return false;
    }

    if (!pylon_request(p, battery_address(p, 0), CID2_GET_SYSTEM_PARAMETER, "", &adr, &plen) ||
        !decode_system_parameter(p, plen, adr, &d->system_parameter))
        return false;

    pylon_calculated_t *c = &d->calculated;
    c->total_capacity = total;
    c->remain_capacity = remain;
    c->remain_percent = total > 0 ? py_round(remain / total * 100, 0) : NAN;
    c->power = py_round(power, 3);
    if (c->power > 0) {
        c->charge_power = c->power;
        c->discharge_power = 0;
    } else {
        c->charge_power = 0;
        c->discharge_power = -1.0 * c->power;
    }
    if (cells > 0) {
        c->cell_voltage_min = cell_min;
        c->cell_voltage_max = cell_max;
        c->cell_voltage_delta = py_round(cell_max - cell_min, 3);
    } else {
        c->cell_voltage_min = c->cell_voltage_max = c->cell_voltage_delta = NAN;
    }
    return true;
}

/*
 * API
 */

pylon_t *pylon_new(const pylon_config_t *cfg)
{
    if (!cfg || !cfg->port || cfg->baudrate < 0 || cfg->battery_count < 0 ||
        cfg->battery_count > PYLON_MAX_BATTERIES || cfg->group < 0 || cfg->group > 7 ||
        !(cfg->timeout >= 0)) {
        errno = EINVAL;
        return NULL;
    }

    pylon_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->port = strdup(cfg->port);
    if (!p->port || pipe2(p->abort_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        free(p->port);
        free(p);
        return NULL;
    }
    p->baudrate = cfg->baudrate ? cfg->baudrate : 9600;
    p->battery_count = cfg->battery_count;
    p->group = cfg->group;
    p->timeout = cfg->timeout > 0 ? cfg->timeout : 1.0;
    p->log = cfg->log;
    p->log_ctx = cfg->log_ctx;
    p->log_level = cfg->log_level;
    p->fd = -1;
    return p;
}

void pylon_free(pylon_t *p)
{
    if (!p)
        return;
    transport_close(p);
    close(p->abort_pipe[0]);
    close(p->abort_pipe[1]);
    free(p->port);
    free(p);
}

int pylon_connect(pylon_t *p)
{
    if (p->fd >= 0)
        return PYLON_OK;

    const char *addr = strip_prefix(p->port, "socket://");
    p->is_serial = addr == p->port;
    p->rx_pos = p->rx_len = 0;
    p->fd = p->is_serial ? serial_open(p) : socket_open(p, addr);
    if (p->fd < 0)
        return PYLON_ERR_OPEN;
    plog(p, PYLON_LOG_INFO, "Opened %s", p->port);
    return PYLON_OK;
}

int pylon_update(pylon_t *p, pylon_data_t *data)
{
    int rc = PYLON_OK;

    if (p->aborted)
        rc = PYLON_ERR_ABORTED;
    else if (pylon_connect(p) != PYLON_OK)
        rc = PYLON_ERR_OPEN;
    else if (!p->detected && !stack_detect(p))
        rc = PYLON_ERR_DETECT;
    else if (!stack_update(p, data))
        rc = PYLON_ERR_UPDATE;

    if (rc != PYLON_OK) {
        if (p->aborted) {
            rc = PYLON_ERR_ABORTED;
            set_error(p, "aborted");
        }
        /* reconnect and detect the batteries again, like the Python version */
        transport_close(p);
        p->detected = false;
        p->battcount = 0;
    }
    return rc;
}

void pylon_abort(pylon_t *p)
{
    int saved = errno;
    p->aborted = 1;
    ssize_t n = write(p->abort_pipe[1], "", 1); /* fails only if the pipe is full: woken up already */
    (void)n;
    errno = saved;
}

const char *pylon_error(const pylon_t *p)
{
    return p->error;
}

const char *pylon_strerror(int rc)
{
    switch (rc) {
    case PYLON_OK: return "Success";
    case PYLON_ERR_OPEN: return "Could not open port";
    case PYLON_ERR_DETECT: return "Pylontech detection error";
    case PYLON_ERR_UPDATE: return "Pylontech update error";
    case PYLON_ERR_ABORTED: return "Aborted";
    default: return "Unknown error";
    }
}

const char *pylon_alarm_name(pylon_alarm_t alarm)
{
    switch (alarm) {
    case PYLON_ALARM_OK: return "Ok";
    case PYLON_ALARM_BELOW_LIMIT: return "BelowLimit";
    case PYLON_ALARM_ABOVE_LIMIT: return "AboveLimit";
    default: return "OtherError";
    }
}

/*
 * Data tree (names, order and int/float types like python-pylontech)
 */

typedef struct {
    pylon_walk_fn fn;
    void *ctx;
} walker_t;

static void w_emit(walker_t *w, pylon_node_t n)
{
    w->fn(w->ctx, &n);
}

static void w_begin(walker_t *w, const char *key, bool array)
{
    w_emit(w, (pylon_node_t){ .type = array ? PYLON_NODE_ARRAY : PYLON_NODE_OBJECT, .key = key });
}

static void w_end(walker_t *w)
{
    w_emit(w, (pylon_node_t){ .type = PYLON_NODE_END });
}

static void w_int(walker_t *w, const char *key, long v)
{
    char text[24];
    snprintf(text, sizeof(text), "%ld", v);
    w_emit(w, (pylon_node_t){ .type = PYLON_NODE_INT, .key = key, .i = v, .text = text });
}

/* non-finite values (absent values) as null */
static void w_float(walker_t *w, const char *key, double v)
{
    char text[40];
    if (!isfinite(v)) {
        w_emit(w, (pylon_node_t){ .type = PYLON_NODE_NULL, .key = key });
        return;
    }
    format_py_float(v, text, sizeof(text));
    w_emit(w, (pylon_node_t){ .type = PYLON_NODE_FLOAT, .key = key, .f = v, .text = text });
}

static void w_bool(walker_t *w, const char *key, bool v)
{
    w_emit(w, (pylon_node_t){ .type = PYLON_NODE_BOOL, .key = key, .b = v });
}

static void w_string(walker_t *w, const char *key, const char *v)
{
    w_emit(w, (pylon_node_t){ .type = PYLON_NODE_STRING, .key = key, .s = v });
}

static void w_floats(walker_t *w, const char *key, const double *v, int n)
{
    w_begin(w, key, true);
    for (int i = 0; i < n; i++)
        w_float(w, NULL, v[i]);
    w_end(w);
}

static void w_alarms(walker_t *w, const char *key, const pylon_alarm_t *v, int n)
{
    w_begin(w, key, true);
    for (int i = 0; i < n; i++)
        w_string(w, NULL, pylon_alarm_name(v[i]));
    w_end(w);
}

static void walk_analog(walker_t *w, const pylon_analog_t *a)
{
    w_begin(w, NULL, false);
    w_int(w, "ADR", a->adr);
    w_int(w, "InfoFlag", a->info_flag);
    w_int(w, "CommandValue", a->command_value);
    w_int(w, "CellCount", a->cell_count);
    w_floats(w, "CellVoltages", a->cell_voltages, a->cell_count);
    w_int(w, "TemperatureCount", a->temperature_count);
    w_floats(w, "Temperatures", a->temperatures, a->temperature_count);
    w_float(w, "Current", a->current);
    w_float(w, "Voltage", a->voltage);
    w_float(w, "RemainCapacity", a->remain_capacity);
    w_string(w, "CapDetect", a->large_capacity ? ">65Ah" : "<=65Ah");
    w_float(w, "ModuleTotalCapacity", a->total_capacity);
    w_int(w, "CycleNumber", a->cycle_number);
    w_end(w);
}

static void walk_charge_management(walker_t *w, const pylon_charge_management_t *c)
{
    w_begin(w, NULL, false);
    w_int(w, "ADR", c->adr);
    w_int(w, "CommandValue", c->command_value);
    w_float(w, "ChargeVoltage", c->charge_voltage);
    w_float(w, "DischargeVoltage", c->discharge_voltage);
    w_float(w, "ChargeCurrent", c->charge_current);
    w_float(w, "DischargeCurrent", c->discharge_current);
    w_bool(w, "StatusChargeEnable", c->charge_enable);
    w_bool(w, "StatusDischargeEnable", c->discharge_enable);
    w_bool(w, "StatusChargeImmediately1", c->charge_immediately1);
    w_bool(w, "StatusChargeImmediately2", c->charge_immediately2);
    w_bool(w, "StatusFullChargeRequired", c->full_charge_required);
    w_end(w);
}

static void walk_alarm_info(walker_t *w, const pylon_alarm_info_t *a)
{
    w_begin(w, NULL, false);
    w_int(w, "ADR", a->adr);
    w_int(w, "InfoFlag", a->info_flag);
    w_int(w, "CommandValue", a->command_value);
    w_int(w, "CellCount", a->cell_count);
    w_alarms(w, "CellAlarm", a->cell, a->cell_count);
    w_int(w, "TemperatureCount", a->temperature_count);
    w_alarms(w, "TemperatureAlarm", a->temperature, a->temperature_count);
    w_string(w, "ChargeCurrentAlarm", pylon_alarm_name(a->charge_current));
    w_string(w, "ModuleVoltageAlarm", pylon_alarm_name(a->module_voltage));
    w_string(w, "DischargeCurrentAlarm", pylon_alarm_name(a->discharge_current));
    for (int i = 0; i < a->status_count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Status%d", i + 1);
        w_int(w, name, a->status[i]);
    }
    w_end(w);
}

static void walk_system_parameter(walker_t *w, const pylon_system_parameter_t *s)
{
    w_begin(w, "SystemParameter", false);
    w_int(w, "ADR", s->adr);
    w_int(w, "InfoFlag", s->info_flag);
    w_float(w, "CellHighVoltageLimit", s->cell_high_voltage_limit);
    w_float(w, "CellLowVoltageLimit", s->cell_low_voltage_limit);
    w_float(w, "CellUnderVoltageLimit", s->cell_under_voltage_limit);
    w_float(w, "ChargeHighTemperatureLimit", s->charge_high_temperature_limit);
    w_float(w, "ChargeLowTemperatureLimit", s->charge_low_temperature_limit);
    w_float(w, "ChargeCurrentLimit", s->charge_current_limit);
    w_float(w, "ModuleHighVoltageLimit", s->module_high_voltage_limit);
    w_float(w, "ModuleLowVoltageLimit", s->module_low_voltage_limit);
    w_float(w, "ModuleUnderVoltageLimit", s->module_under_voltage_limit);
    w_float(w, "DischargeHighTemperatureLimit", s->discharge_high_temperature_limit);
    w_float(w, "DischargeLowTemperatureLimit", s->discharge_low_temperature_limit);
    w_float(w, "DischargeCurrentLimit", s->discharge_current_limit);
    w_end(w);
}

static void walk_calculated(walker_t *w, const pylon_calculated_t *c)
{
    w_begin(w, "Calculated", false);
    w_float(w, "TotalCapacity_Ah", c->total_capacity);
    w_float(w, "RemainCapacity_Ah", c->remain_capacity);
    w_float(w, "Remain_Percent", c->remain_percent);
    w_float(w, "Power_W", c->power);
    /* the zero of the inactive direction is an int in python-pylontech */
    if (c->power > 0) {
        w_float(w, "ChargePower_W", c->charge_power);
        w_int(w, "DischargePower_W", 0);
    } else {
        w_int(w, "ChargePower_W", 0);
        w_float(w, "DischargePower_W", c->discharge_power);
    }
    if (isfinite(c->cell_voltage_min)) {
        w_float(w, "CellVoltageMin", c->cell_voltage_min);
        w_float(w, "CellVoltageMax", c->cell_voltage_max);
        w_float(w, "CellVoltageDelta", c->cell_voltage_delta);
    }
    w_end(w);
}

void pylon_data_walk(const pylon_data_t *d, pylon_walk_fn fn, void *ctx)
{
    walker_t w = { fn, ctx };
    int n = d->battery_count;

    w_begin(&w, NULL, false);
    w_begin(&w, "SerialNumbers", true);
    for (int b = 0; b < n; b++)
        w_string(&w, NULL, d->batteries[b].serial_number);
    w_end(&w);
    w_begin(&w, "Analog", true);
    for (int b = 0; b < n; b++)
        walk_analog(&w, &d->batteries[b].analog);
    w_end(&w);
    w_begin(&w, "ChargeDischargeManagement", true);
    for (int b = 0; b < n; b++)
        walk_charge_management(&w, &d->batteries[b].charge_management);
    w_end(&w);
    w_begin(&w, "AlarmInfo", true);
    for (int b = 0; b < n; b++)
        walk_alarm_info(&w, &d->batteries[b].alarm_info);
    w_end(&w);
    walk_system_parameter(&w, &d->system_parameter);
    walk_calculated(&w, &d->calculated);
    w_end(&w);
}

/*
 * JSON
 */

#define JSON_MAX_DEPTH 16

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
    int depth;
    bool array[JSON_MAX_DEPTH];
    bool first[JSON_MAX_DEPTH];
} json_t;

static void js_putn(json_t *j, const char *s, size_t n)
{
    if (j->oom)
        return;
    if (j->len + n + 1 > j->cap) {
        size_t cap = j->cap ? j->cap : 4096;
        while (cap < j->len + n + 1)
            cap *= 2;
        char *tmp = realloc(j->buf, cap);
        if (!tmp) {
            j->oom = true;
            return;
        }
        j->buf = tmp;
        j->cap = cap;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

static void js_puts(json_t *j, const char *s)
{
    js_putn(j, s, strlen(s));
}

static void js_string(json_t *j, const char *s)
{
    js_putn(j, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            js_putn(j, esc, 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            js_puts(j, esc);
        } else {
            js_putn(j, (const char *)&c, 1);
        }
    }
    js_putn(j, "\"", 1);
}

static void json_node(void *ctx, const pylon_node_t *n)
{
    json_t *j = ctx;

    if (n->type == PYLON_NODE_END) {
        if (j->depth > 0)
            js_puts(j, j->array[--j->depth] ? "]" : "}");
        return;
    }
    if (j->depth > 0) {
        if (!j->first[j->depth - 1])
            js_putn(j, ",", 1);
        j->first[j->depth - 1] = false;
    }
    if (n->key) {
        js_string(j, n->key);
        js_putn(j, ":", 1);
    }
    switch (n->type) {
    case PYLON_NODE_OBJECT:
    case PYLON_NODE_ARRAY:
        if (j->depth == JSON_MAX_DEPTH) {
            j->oom = true;
            return;
        }
        js_puts(j, n->type == PYLON_NODE_ARRAY ? "[" : "{");
        j->array[j->depth] = n->type == PYLON_NODE_ARRAY;
        j->first[j->depth++] = true;
        break;
    case PYLON_NODE_INT:
    case PYLON_NODE_FLOAT: js_puts(j, n->text); break;
    case PYLON_NODE_BOOL: js_puts(j, n->b ? "true" : "false"); break;
    case PYLON_NODE_STRING: js_string(j, n->s); break;
    default: js_puts(j, "null"); break;
    }
}

char *pylon_data_json(const pylon_data_t *data)
{
    json_t j = { 0 };

    pylon_data_walk(data, json_node, &j);
    if (j.oom) {
        free(j.buf);
        return NULL;
    }
    return j.buf;
}

void pylon_json_free(char *json)
{
    free(json);
}
