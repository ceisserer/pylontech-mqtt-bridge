/*
 * pylon_dump - libpylontech example: polls a battery stack once and prints
 * a summary and the data tree as JSON
 *
 * Usage: pylon_dump <port> [battery_count]
 *   e.g. pylon_dump /dev/ttyUSB0
 *        pylon_dump socket://192.168.1.50:8899 2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>

#include <pylontech.h>

static void log_cb(void *ctx, int level, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "[%d] %s\n", level, msg);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [battery_count]\n", argv[0]);
        return 1;
    }

    pylon_config_t cfg = {
        .port = argv[1],
        .battery_count = argc > 2 ? atoi(argv[2]) : 0,
        .log = log_cb,
        .log_level = PYLON_LOG_INFO,
    };
    pylon_t *p = pylon_new(&cfg);
    if (!p) {
        perror("pylon_new");
        return 1;
    }

    pylon_data_t *data = malloc(sizeof(*data));
    int rc = data ? pylon_update(p, data) : PYLON_ERR_UPDATE;
    if (rc != PYLON_OK) {
        fprintf(stderr, "%s: %s\n", pylon_strerror(rc), pylon_error(p));
        free(data);
        pylon_free(p);
        return 1;
    }

    for (int b = 0; b < data->battery_count; b++) {
        const pylon_battery_t *bat = &data->batteries[b];
        printf("Battery %d (%s): %.3f V, %.1f A, %d cells\n", b, bat->serial_number,
               bat->analog.voltage, bat->analog.current, bat->analog.cell_count);
    }
    printf("Stack: %.0f %%, %.1f W\n", data->calculated.remain_percent, data->calculated.power);

    char *json = pylon_data_json(data);
    if (json)
        printf("%s\n", json);
    pylon_json_free(json);

    free(data);
    pylon_free(p);
    return 0;
}
