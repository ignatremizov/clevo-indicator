#ifndef CLEVO_EC_MONITOR_H
#define CLEVO_EC_MONITOR_H

#include <stdint.h>
#include <sys/types.h>

typedef ssize_t (*EcReadAt)(int, void *, size_t, off_t);

/* The only registers needed by the indicator, not an EC memory dump. */
typedef struct {
    uint8_t cpu_temp;
    uint8_t gpu_temp;
    uint8_t cpu_duty;
    uint8_t gpu_duty;
    uint8_t rpm[4];
} EcSample;

int ec_sample_read(int fd, EcReadAt read_at, EcSample *sample);

#endif
