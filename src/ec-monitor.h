#ifndef CLEVO_EC_MONITOR_H
#define CLEVO_EC_MONITOR_H

#include <stdint.h>
#include <sys/types.h>

/* Inherited mapping formerly preceded by an unused "#define P775DM3".
   That label never selected registers or detected hardware. These addresses
   have been used on this fork's development board, DMI name X170KM-G;
   this does not establish a universal mapping for either model's firmware.
   Duties are readback registers; fan writes use a separate command protocol. */
#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_CPU_FAN_DUTY 0xCE
#define EC_REG_GPU_FAN_DUTY 0xCF
#define EC_REG_CPU_FAN_RPMS_HI 0xD0
#define EC_REG_CPU_FAN_RPMS_LO 0xD1
#define EC_REG_GPU_FAN_RPMS_HI 0xD2
#define EC_REG_GPU_FAN_RPMS_LO 0xD3

#define EC_SENSOR_MAX_AGE_MS 3000
#define EC_COMMAND_RETRY_MS 2000
#define EC_POLL_MS 1000
#define EC_DUTY_VERIFY_MS 30000
#define EC_READ_CPU 1u
#define EC_READ_GPU 2u
#define EC_READ_DUTY 4u
#define EC_READ_RPM 8u
#define EC_READ_ALL 15u

typedef ssize_t (*EcReadAt)(int, void *, size_t, off_t);

/* Selectively populated telemetry, not an EC memory dump. */
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

typedef struct {
    int target;
    int attempted;
    int confirmed;
    int64_t attempted_ms;
} EcFanCommand;

int64_t ec_monotonic_ms(void);
/* Only requested fields are updated; failure leaves the sample untouched. */
int ec_sample_read(int fd, EcReadAt read_at, EcSample *sample, unsigned fields);
/* Read the hottest coretemp package. Root is fixed to /sys/class/hwmon in
   production; the argument permits disposable filesystem fixtures in tests. */
int ec_coretemp_read(const char *root);
void ec_temperature_update(EcTemperature *sensor, int value, int64_t now);
int ec_temperature_value(const EcTemperature *sensor, int64_t now);
/* Returns 1 when a write is due, 0 otherwise. Confirmation requires readback. */
int ec_fan_command_due(EcFanCommand *command, int target, int actual,
                       int64_t now);

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
