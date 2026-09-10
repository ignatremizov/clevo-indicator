#include "../src/ec-monitor.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int calls, bytes, short_reads, interrupt_read, fail_at;

static ssize_t fake_read(int fd, void *buf, size_t len, off_t offset)
{
    assert(fd == 42);
    calls++;
    assert(offset == 0x07 || (offset >= 0xCD && offset + len <= 0xD4));
    if (interrupt_read) {
        interrupt_read = 0;
        errno = EINTR;
        return -1;
    }
    if (calls == fail_at)
        return 0;
    if (short_reads && len > 2)
        len = 2;
    for (size_t i = 0; i < len; i++)
        ((unsigned char *)buf)[i] = (unsigned char)(offset + i);
    bytes += len;
    return len;
}

static void test_reads(void)
{
    EcSample sample;
    assert(ec_sample_read(42, fake_read, &sample) == 0);
    assert(calls == 2 && bytes == 8);
    assert(sample.cpu_temp == 7 && sample.gpu_temp == 0xCD);
    assert(sample.cpu_duty == 0xCE && sample.gpu_duty == 0xCF);
    assert(sample.rpm[0] == 0xD0 && sample.rpm[3] == 0xD3);
    calls = bytes = 0;
    short_reads = interrupt_read = 1;
    assert(ec_sample_read(42, fake_read, &sample) == 0);
    assert(bytes == 8 && calls == 6);
    EcSample before = sample;
    calls = 0;
    fail_at = 3;
    assert(ec_sample_read(42, fake_read, &sample) == -1);
    assert(memcmp(&before, &sample, sizeof(sample)) == 0);
    puts("PASS targeted reads, short reads, EINTR, partial-sample rejection");
}

static void test_temperatures(void)
{
    EcTemperature sensor = {0};
    assert(ec_temperature_value(&sensor, 100) == -1);
    ec_temperature_update(&sensor, 60, 100);
    assert(ec_temperature_value(&sensor, 3099) == 60);
    assert(ec_temperature_value(&sensor, 3100) == -1);
    assert(ec_temperature_value(&sensor, 99) == -1);
    ec_temperature_update(&sensor, -1, 3200);
    assert(ec_temperature_value(&sensor, 3200) == -1);
    ec_temperature_update(&sensor, 255, 3300);
    assert(ec_temperature_value(&sensor, 3300) == -1);
    ec_temperature_update(&sensor, 0, 3400);
    assert(ec_temperature_value(&sensor, 3400) == -1);
    ec_temperature_update(&sensor, 70, 3500);
    assert(ec_temperature_value(&sensor, 3500) == 70);
    puts("PASS temperature validity, expiry, and recovery");
}

static void test_commands(void)
{
    EcFanCommand cmd = {0};
    assert(ec_fan_command_due(&cmd, 70, 40, 100));
    assert(!cmd.confirmed);
    assert(!ec_fan_command_due(&cmd, 70, 40, 2099));
    assert(ec_fan_command_due(&cmd, 70, 40, 2100));
    assert(!ec_fan_command_due(&cmd, 70, 70, 2200));
    assert(cmd.confirmed);
    assert(!ec_fan_command_due(&cmd, 70, -1, 2300));
    assert(!cmd.confirmed);
    assert(ec_fan_command_due(&cmd, 100, 70, 2400));
    assert(!ec_fan_command_due(&cmd, 100, 70, 2500));
    assert(ec_fan_command_due(&cmd, 100, 70, 4400));
    assert(!ec_fan_command_due(&cmd, 100, 100, 4500));
    assert(cmd.confirmed);
    assert(ec_fan_command_due(&cmd, 100, 70, 6500));
    assert(!ec_fan_command_due(&cmd, 0, -1, 6600));
    assert(!cmd.confirmed);
    assert(ec_fan_command_due(&cmd, 100, 99, 8500));
    assert(!cmd.confirmed);
    puts("PASS readback confirmation, rate-limited retries, emergency escalation/retry");
}

static void settle(EcGpuQuery *query)
{
    if (query->child) {
        assert(waitpid(query->child, NULL, 0) == query->child);
        query->child = 0;
    }
}

static void test_gpu(void)
{
    EcGpuQuery query = {0};
    assert(ec_gpu_query(&query, "./bin/test-gpu-good", 1000) == 63);
    assert(query.child == 0);
    assert(ec_gpu_query(&query, "./bin/test-gpu-invalid", 1000) == -1);
    assert(ec_gpu_query(&query, "./bin/test-gpu-error", 1000) == -1);
    assert(ec_gpu_query(&query, "./bin/test-gpu-overflow", 1000) == -1);
    settle(&query);
    assert(ec_gpu_query(&query, "/nonexistent-clevo-test-program", 1000) == -1);
    int64_t start = ec_monotonic_ms();
    assert(ec_gpu_query(&query, "./bin/test-gpu-hang", 100) == -1);
    assert(ec_monotonic_ms() - start < 1000);
    settle(&query);
    query.child = fork();
    assert(query.child >= 0);
    if (!query.child) {
        pause();
        _exit(0);
    }
    pid_t pending = query.child;
    assert(ec_gpu_query(&query, "./bin/test-gpu-good", 1000) == -1);
    assert(query.child == pending);
    assert(kill(pending, SIGKILL) == 0);
    settle(&query);
    assert(ec_gpu_query(&query, "./bin/test-gpu-good", 1000) == 63);
    puts("PASS GPU timeout, parsing, exit status, no duplicate pending child, recovery");
}

int main(void)
{
    test_reads();
    test_temperatures();
    test_commands();
    test_gpu();
    return 0;
}
