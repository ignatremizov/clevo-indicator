/*
 * Exercise the real worker and port protocol without privileges or hardware.
 * System calls that can touch the machine are replaced before including it.
 * No GTK initialization, indicator launch, NVIDIA call, or EC access occurs.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>
#include "../src/ec-monitor.h"

static int test_setuid(uid_t uid);
static int test_system(const char *command);
static int test_open(const char *name, int flags, ...);
static int test_close(int fd);
static ssize_t test_pread(int fd, void *buf, size_t len, off_t offset);
static int test_usleep(useconds_t delay);
static unsigned char test_inb(unsigned short port);
static void test_outb(unsigned char value, unsigned short port);
static int64_t test_now(void);

#define main unused_indicator_main
#define setuid test_setuid
#define system test_system
#define open test_open
#define close test_close
#define pread test_pread
#define usleep test_usleep
#define inb test_inb
#define outb test_outb
#define ec_monotonic_ms test_now
#include "../src/clevo-indicator.c"
#undef main
#undef setuid
#undef system
#undef open
#undef close
#undef pread
#undef usleep
#undef inb
#undef outb
#undef ec_monotonic_ms

static __typeof__(*share_info) shared;
static int loops, limit, fail_sample_after, bytes_read, write_count;
static int busy_after_writes, status_byte, readback, port_sleeps;
static int gpu_cache_test, change_to_manual, recover_after;
static unsigned char cpu_temperature, gpu_temperature, cpu_duty, gpu_duty;
static unsigned char written[512];

static void reset_worker(int iterations)
{
    memset(&shared, 0, sizeof(shared));
    share_info = &shared;
    memset(auto_fan_states, 0, sizeof(auto_fan_states));
    memset(&g_gpu_temperature, 0, sizeof(g_gpu_temperature));
    parent_pid = 0;
    use_gpu_temp_smi = 0;
    use_hwmon_interface = 0;
    loops = bytes_read = write_count = port_sleeps = 0;
    limit = iterations;
    fail_sample_after = busy_after_writes = -1;
    recover_after = -1;
    status_byte = readback = gpu_cache_test = change_to_manual = 0;
    cpu_temperature = 60;
    gpu_temperature = 55;
    cpu_duty = gpu_duty = 128;
}

static int test_setuid(uid_t uid)
{
    assert(uid == 0);
    return 0;
}

static int test_system(const char *command)
{
    assert(strcmp(command, "modprobe ec_sys") == 0);
    return 0;
}

static int test_open(const char *name, int flags, ...)
{
    assert(strcmp(name, "/sys/kernel/debug/ec/ec0/io") == 0);
    assert((flags & O_ACCMODE) == O_RDONLY);
    return 42;
}

static int test_close(int fd)
{
    assert(fd == 42);
    return 0;
}

static ssize_t test_pread(int fd, void *buf, size_t len, off_t offset)
{
    assert(fd == 42);
    if (gpu_cache_test) {
        /* Enabled after thread creation point: no real GPU thread is started. */
        use_gpu_temp_smi = 1;
        if (!loops)
            ec_temperature_update(&g_gpu_temperature, 80, test_now());
    }
    if (fail_sample_after >= 0 && loops >= fail_sample_after &&
        (recover_after < 0 || loops < recover_after)) {
        errno = EIO;
        return -1;
    }
    if (offset == 0x07) {
        assert(len == 1);
        *(unsigned char *)buf = cpu_temperature;
    } else {
        assert(offset == 0xCD && len == 7);
        unsigned char block[7] = {
            gpu_temperature, cpu_duty, gpu_duty, 0x02, 0x10, 0x02, 0x20
        };
        memcpy(buf, block, sizeof(block));
    }
    bytes_read += len;
    return len;
}

static int64_t test_now(void)
{
    return 1000 + loops * 200;
}

static int test_usleep(useconds_t delay)
{
    if (delay == 1000) {
        port_sleeps++;
        return 0;
    }
    assert(delay == 200000);
    if (readback && write_count >= 3) {
        assert(written[write_count - 3] == 0x99);
        if (written[write_count - 2] == 1)
            cpu_duty = written[write_count - 1];
        else
            gpu_duty = written[write_count - 1];
    }
    if (change_to_manual && loops == 1) {
        share_info->auto_cpu_duty = 0;
        share_info->manual_next_cpu_fan_duty = 70;
    }
    if (++loops >= limit)
        share_info->exit = 1;
    return 0;
}

static unsigned char test_inb(unsigned short port)
{
    assert(port == EC_SC || port == EC_DATA);
    if (port == EC_DATA)
        return 80;
    if (busy_after_writes >= 0 && write_count >= busy_after_writes)
        return 2; /* IBF remains busy. */
    return status_byte;
}

static void test_outb(unsigned char value, unsigned short port)
{
    assert(port == EC_SC || port == EC_DATA);
    assert(write_count < (int)sizeof(written));
    written[write_count++] = value;
}

static void test_port_protocol(void)
{
    for (int stage = 0; stage < 4; stage++) {
        reset_worker(1);
        busy_after_writes = stage;
        assert(ec_io_do(0x99, 1, 200) == EXIT_FAILURE);
        assert(write_count == stage);
        assert(port_sleeps == 100);
    }
    reset_worker(1);
    assert(ec_io_do(0x99, 1, 200) == EXIT_SUCCESS);
    assert(write_count == 3);
    reset_worker(1);
    busy_after_writes = 0;
    assert(ec_io_read(7) == -1 && write_count == 0);
    reset_worker(1);
    busy_after_writes = 1;
    assert(ec_io_read(7) == -1 && write_count == 1);
    reset_worker(1);
    assert(ec_io_read(7) == -1 && write_count == 2); /* OBF never ready. */
    reset_worker(1);
    status_byte = 1;
    assert(ec_io_read(7) == 80 && write_count == 2);
    assert(calculate_fan_duty(-1) == -1);
    assert(calculate_fan_rpms(-1, 0) == -1);
    puts("PASS port timeouts abort every command/read stage");
}

static void test_worker_cases(void)
{
    reset_worker(5);
    assert(main_ec_worker() == 0);
    assert(bytes_read == 5 * 8 && write_count == 0);
    puts("PASS worker reads eight bytes per cycle and preserves manual fans");

    reset_worker(15);
    shared.manual_next_cpu_fan_duty = 70;
    busy_after_writes = 0;
    assert(main_ec_worker() == 0);
    assert(write_count == 0 && port_sleeps == 200);
    assert(shared.manual_prev_cpu_fan_duty == 50);
    puts("PASS failed manual writes retry without false acknowledgement");

    reset_worker(15);
    shared.manual_next_cpu_fan_duty = 70;
    readback = 1;
    assert(main_ec_worker() == 0);
    assert(write_count == 3 && shared.manual_prev_cpu_fan_duty == 70);
    puts("PASS manual command is acknowledged only after readback");

    reset_worker(25);
    cpu_temperature = 95;
    shared.auto_cpu_duty = 1;
    assert(main_ec_worker() == 0);
    assert(write_count == 9); /* 0, 2, and 4 seconds; hardware never confirms. */
    assert(written[2] == 255 && written[5] == 255 && written[8] == 255);
    assert(shared.auto_cpu_duty_val == 0);
    puts("PASS emergency target retries even when the target remains unchanged");

    reset_worker(20);
    cpu_temperature = gpu_temperature = 60;
    cpu_duty = 133; /* Existing curve's 52% target. */
    shared.auto_cpu_duty = 1;
    fail_sample_after = 1;
    assert(main_ec_worker() == 0);
    assert(shared.cpu_temp == -1 && shared.gpu_temp == -1);
    assert(write_count == 3 && written[1] == 1 && written[2] == 255);
    puts("PASS stale sensors escalate only AUTO fan, leaving manual GPU untouched");

    reset_worker(35);
    cpu_temperature = gpu_temperature = 60;
    cpu_duty = 133;
    shared.auto_cpu_duty = 1;
    fail_sample_after = 1;
    recover_after = 20;
    readback = 1;
    assert(main_ec_worker() == 0);
    assert(shared.cpu_temp == 60 && shared.gpu_temp == 60);
    assert(write_count == 6 && written[2] == 255 && written[5] == 133);
    assert(shared.auto_cpu_duty_val == 52);
    puts("PASS recovered sensors resume the unchanged normal fan curve");

    reset_worker(2);
    cpu_temperature = 0;
    shared.auto_gpu_duty = 1;
    assert(main_ec_worker() == 0);
    assert(write_count == 3 && written[1] == 2 && written[2] == 255);
    puts("PASS invalid sensor triggers AUTO failsafe");

    reset_worker(20);
    gpu_cache_test = 1;
    assert(main_ec_worker() == 0);
    assert(shared.gpu_temp == 55 && write_count == 0);
    puts("PASS expired NVIDIA cache falls back to fresh EC temperature");

    reset_worker(20);
    cpu_temperature = 95;
    shared.auto_cpu_duty = 1;
    change_to_manual = 1;
    readback = 1;
    assert(main_ec_worker() == 0);
    assert(write_count == 6 && written[5] == 179);
    assert(shared.manual_prev_cpu_fan_duty == 70);
    puts("PASS manual selection cancels previous AUTO target");
}

int main(void)
{
    test_port_protocol();
    test_worker_cases();
    return 0;
}
