/*
 * libpylontech - Pylontech battery (RS485) protocol library
 *
 * Polls a Pylontech battery stack (US2000, US3000, US5000, ... and
 * compatibles) via the "PYLON low voltage Protocol RS485" over a serial port
 * or an RS485 to Ethernet adapter. No dependencies besides libc and libm.
 *
 * Usage:
 *
 *     pylon_config_t cfg = { .port = "/dev/ttyUSB0" };
 *     pylon_t *p = pylon_new(&cfg);
 *     pylon_data_t data;
 *     if (pylon_update(p, &data) == PYLON_OK)
 *         printf("%.1f %%\n", data.calculated.remain_percent);
 *     else
 *         fprintf(stderr, "%s\n", pylon_error(p));
 *     pylon_free(p);
 *
 * A handle must not be used by several threads at the same time (except
 * pylon_abort()); separate handles are independent.
 *
 * Copyright (c) 2022 Bernd Singer (python-pylontech, MIT License,
 *                    see LICENSE.python-pylontech)
 * Copyright (c) 2026 Clemens Eisserer (C port)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PYLONTECH_H
#define PYLONTECH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define PYLON_API __attribute__((visibility("default")))
#else
#define PYLON_API
#endif

#define PYLON_VERSION_MAJOR 1
#define PYLON_VERSION_MINOR 0
#define PYLON_VERSION_PATCH 0

#define PYLON_MAX_BATTERIES 16
#define PYLON_MAX_CELLS 32
#define PYLON_MAX_TEMPERATURES 16
#define PYLON_MAX_ALARM_STATUS 5

/* return codes */
#define PYLON_OK 0
#define PYLON_ERR_OPEN (-1)    /* could not open the port */
#define PYLON_ERR_DETECT (-2)  /* battery detection (serial numbers) failed */
#define PYLON_ERR_UPDATE (-3)  /* polling the batteries failed */
#define PYLON_ERR_ABORTED (-4) /* pylon_abort() was called */

/* log levels (same values as Python's logging module) */
#define PYLON_LOG_DEBUG 10
#define PYLON_LOG_INFO 20
#define PYLON_LOG_WARNING 30
#define PYLON_LOG_ERROR 40

typedef struct pylon pylon_t;

typedef void (*pylon_log_fn)(void *ctx, int level, const char *msg);

/* zero means default for all members except port */
typedef struct {
    const char *port;    /* serial device, e.g. "/dev/ttyUSB0", or "socket://host:port" */
    int baudrate;        /* default 9600 */
    int battery_count;   /* number of batteries, default 0 = auto detect */
    int group;           /* battery group 0..7 */
    double timeout;      /* seconds to wait for a response, default 1 */
    pylon_log_fn log;    /* optional log callback */
    void *log_ctx;       /* passed to log */
    int log_level;       /* minimum level passed to log, default 0 = all */
} pylon_config_t;

typedef enum {
    PYLON_ALARM_OK = 0,
    PYLON_ALARM_BELOW_LIMIT = 1,
    PYLON_ALARM_ABOVE_LIMIT = 2,
    PYLON_ALARM_OTHER_ERROR = 3,
} pylon_alarm_t;

/* Units: V, A, Ah, W and °C. Currents and power are positive while charging. */

typedef struct {
    int adr;
    int info_flag;
    int command_value;
    int cell_count;
    double cell_voltages[PYLON_MAX_CELLS];
    int temperature_count;
    double temperatures[PYLON_MAX_TEMPERATURES];
    double current;
    double voltage;
    double remain_capacity;
    bool large_capacity; /* CapDetect ">65Ah" */
    double total_capacity;
    int cycle_number;
} pylon_analog_t;

typedef struct {
    int adr;
    int command_value;
    double charge_voltage;
    double discharge_voltage;
    double charge_current;
    double discharge_current;
    bool charge_enable;
    bool discharge_enable;
    bool charge_immediately1;
    bool charge_immediately2;
    bool full_charge_required;
} pylon_charge_management_t;

typedef struct {
    int adr;
    int info_flag;
    int command_value;
    int cell_count;
    pylon_alarm_t cell[PYLON_MAX_CELLS];
    int temperature_count;
    pylon_alarm_t temperature[PYLON_MAX_TEMPERATURES];
    pylon_alarm_t charge_current;
    pylon_alarm_t module_voltage;
    pylon_alarm_t discharge_current;
    int status_count; /* status bytes, only sent by newer firmware */
    int status[PYLON_MAX_ALARM_STATUS];
} pylon_alarm_info_t;

typedef struct {
    int adr;
    int info_flag;
    double cell_high_voltage_limit;
    double cell_low_voltage_limit;
    double cell_under_voltage_limit;
    double charge_high_temperature_limit;
    double charge_low_temperature_limit;
    double charge_current_limit;
    double module_high_voltage_limit;
    double module_low_voltage_limit;
    double module_under_voltage_limit;
    double discharge_high_temperature_limit;
    double discharge_low_temperature_limit;
    double discharge_current_limit;
} pylon_system_parameter_t;

/* summed over all batteries */
typedef struct {
    double total_capacity;
    double remain_capacity;
    double remain_percent;   /* NAN if total_capacity is 0 */
    double power;
    double charge_power;     /* power while charging, else 0 */
    double discharge_power;  /* -power while discharging, else 0 */
    double cell_voltage_min; /* over all cells of all batteries; NAN without cells */
    double cell_voltage_max;
    double cell_voltage_delta;
} pylon_calculated_t;

typedef struct {
    char serial_number[17];
    pylon_analog_t analog;
    pylon_charge_management_t charge_management;
    pylon_alarm_info_t alarm_info;
} pylon_battery_t;

typedef struct {
    int battery_count;
    pylon_battery_t batteries[PYLON_MAX_BATTERIES];
    pylon_system_parameter_t system_parameter; /* of the master battery */
    pylon_calculated_t calculated;
} pylon_data_t;

/*
 * Creates a handle; the port is opened by the first pylon_update() (or
 * pylon_connect()). The config is copied. Returns NULL with errno set on
 * invalid config (EINVAL) or out of memory.
 */
PYLON_API pylon_t *pylon_new(const pylon_config_t *cfg);

/* closes the port and frees the handle; NULL is ignored */
PYLON_API void pylon_free(pylon_t *p);

/* opens the port if it is not open yet; returns PYLON_OK or PYLON_ERR_OPEN */
PYLON_API int pylon_connect(pylon_t *p);

/*
 * Opens the port and detects the batteries if needed, then polls all
 * batteries into *data. Returns PYLON_OK or a PYLON_ERR_* code; on error,
 * *data is undefined and the port is closed, so the next call reconnects
 * and detects the batteries again. Blocks for up to a few times the timeout.
 */
PYLON_API int pylon_update(pylon_t *p, pylon_data_t *data);

/*
 * Makes the running and all further pylon_update() calls of this handle
 * return PYLON_ERR_ABORTED as soon as possible. Meant for shutdown; may be
 * called from another thread or a signal handler.
 */
PYLON_API void pylon_abort(pylon_t *p);

/* description of the last error of this handle */
PYLON_API const char *pylon_error(const pylon_t *p);

/* description of a return code */
PYLON_API const char *pylon_strerror(int rc);

/* "Ok", "BelowLimit", "AboveLimit" or "OtherError" */
PYLON_API const char *pylon_alarm_name(pylon_alarm_t alarm);

/*
 * Data tree: the data as JSON-like tree with the names of python-pylontech,
 * see README.md. pylon_data_walk() reports the nodes in order: OBJECT and
 * ARRAY open a container that is closed by an END node.
 */
typedef enum {
    PYLON_NODE_OBJECT,
    PYLON_NODE_ARRAY,
    PYLON_NODE_END,
    PYLON_NODE_INT,
    PYLON_NODE_FLOAT,
    PYLON_NODE_BOOL,
    PYLON_NODE_STRING,
    PYLON_NODE_NULL,
} pylon_node_type_t;

typedef struct {
    pylon_node_type_t type;
    const char *key;  /* member name inside objects, NULL inside arrays and for END */
    long i;           /* INT */
    double f;         /* FLOAT */
    bool b;           /* BOOL */
    const char *s;    /* STRING */
    const char *text; /* INT and FLOAT formatted like Python, e.g. "6.0"; else NULL */
} pylon_node_t;

typedef void (*pylon_walk_fn)(void *ctx, const pylon_node_t *node);

PYLON_API void pylon_data_walk(const pylon_data_t *data, pylon_walk_fn fn, void *ctx);

/* the data tree as compact JSON; release with pylon_json_free(); NULL if out of memory */
PYLON_API char *pylon_data_json(const pylon_data_t *data);

PYLON_API void pylon_json_free(char *json);

#ifdef __cplusplus
}
#endif

#endif /* PYLONTECH_H */
