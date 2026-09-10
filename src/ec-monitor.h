#ifndef CLEVO_EC_MONITOR_H
#define CLEVO_EC_MONITOR_H

#include <stdint.h>
#include <sys/types.h>

#define EC_SENSOR_MAX_AGE_MS 3000

typedef ssize_t (*EcReadAt)(int, void *, size_t, off_t);

/* The only registers needed by the indicator, not an EC memory dump. */
typedef struct {
    uint8_t cpu_temp;
    uint8_t gpu_temp;
    uint8_t cpu_duty;
    uint8_t gpu_duty;
    uint8_t rpm[4];
} EcSample;

typedef struct {
    int value;
    int valid;
    int64_t updated_ms;
} EcTemperature;

int64_t ec_monotonic_ms(void);
int ec_sample_read(int fd, EcReadAt read_at, EcSample *sample);
void ec_temperature_update(EcTemperature *sensor, int value, int64_t now);
int ec_temperature_value(const EcTemperature *sensor, int64_t now);
/*
 * A timed-out GPU query may be stuck in uninterruptible driver I/O. Retain its
 * PID and do not spawn replacements until it has exited; never block reaping.
 * One owner/thread must use a query object.
 */
typedef struct {
    pid_t child;
} EcGpuQuery;

int ec_gpu_query(EcGpuQuery *query, const char *executable, int timeout_ms);

#endif
