#define _GNU_SOURCE
#include "ec-monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int64_t ec_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int read_exact(int fd, EcReadAt read_at, void *buf, size_t len,
                      off_t offset)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read_at(fd, (char *)buf + done, len - done, offset + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || (size_t)n > len - done)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

int ec_sample_read(int fd, EcReadAt read_at, EcSample *sample)
{
    EcSample next;
    uint8_t block[7];
    if (read_exact(fd, read_at, &next.cpu_temp, 1, 0x07) != 0 ||
        read_exact(fd, read_at, block, sizeof(block), 0xCD) != 0)
        return -1;
    next.gpu_temp = block[0];
    next.cpu_duty = block[1];
    next.gpu_duty = block[2];
    memcpy(next.rpm, block + 3, sizeof(next.rpm));
    *sample = next; /* Never publish a partial sample. */
    return 0;
}

void ec_temperature_update(EcTemperature *sensor, int value, int64_t now)
{
    /* Reject zero/disconnected readings and obvious EC sentinel values. */
    sensor->valid = value >= 15 && value <= 125;
    if (sensor->valid) {
        sensor->value = value;
        sensor->updated_ms = now;
    }
}

int ec_temperature_value(const EcTemperature *sensor, int64_t now)
{
    if (!sensor->valid || now < sensor->updated_ms ||
        now - sensor->updated_ms >= EC_SENSOR_MAX_AGE_MS)
        return -1;
    return sensor->value;
}

int ec_fan_command_due(EcFanCommand *command, int target, int actual,
                       int64_t now)
{
    command->confirmed = 0;
    if (target < 1 || target > 100)
        return 0;
    int changed = command->target != target;
    command->target = target;
    command->confirmed = actual >= 0 &&
        abs(actual - target) <= (target == 100 ? 0 : 1);
    if (command->confirmed)
        return 0;
    /* Emergency escalation bypasses the retry interval, but repeated failures
       (including emergency writes) remain bounded to one attempt per 2 s. */
    if (!command->attempted || (changed && target == 100) ||
        now - command->attempted_ms >= EC_COMMAND_RETRY_MS) {
        command->attempted = 1;
        command->attempted_ms = now;
        return 1;
    }
    return 0;
}

static int reap_query(EcGpuQuery *query, int *result)
{
    pid_t ret = waitpid(query->child, result, WNOHANG);
    if (ret == query->child || (ret < 0 && errno == ECHILD)) {
        query->child = 0;
        return ret > 0;
    }
    return 0;
}

int ec_gpu_query(EcGpuQuery *query, const char *executable, int timeout_ms)
{
    int result = 0;
    if (query->child) {
        reap_query(query, &result);
        if (query->child)
            return -1;
    }
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0)
        return -1;
    pid_t child = fork();
    if (child == 0) {
        /* Only async-signal-safe operations after fork from the worker thread. */
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        char *const argv[] = {
            (char *)executable, "--query-gpu=temperature.gpu",
            "--format=csv,noheader,nounits", NULL
        };
        char *const env[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
        execve(executable, argv, env);
        _exit(127);
    }
    close(pipes[1]);
    if (child < 0) {
        close(pipes[0]);
        return -1;
    }
    query->child = child;
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    int64_t deadline = ec_monotonic_ms() + timeout_ms;
    char output[128];
    size_t used = 0;
    int eof = 0, exited = 0, failed = 0;
    while (ec_monotonic_ms() < deadline) {
        if (!exited)
            exited = reap_query(query, &result);
        if (eof && exited)
            break;
        struct pollfd pfd = {.fd = eof ? -1 : pipes[0], .events = POLLIN};
        int64_t remaining = deadline - ec_monotonic_ms();
        if (remaining <= 0)
            break;
        int wait_ms = remaining < 20 ? (int)remaining : 20;
        int ready = poll(&pfd, 1, wait_ms);
        if (ready < 0 && errno != EINTR) {
            failed = 1;
            break;
        }
        if (!eof && ready > 0) {
            ssize_t n = read(pipes[0], output + used, sizeof(output) - 1 - used);
            if (n > 0) {
                used += (size_t)n;
                if (used == sizeof(output) - 1) {
                    failed = 1;
                    break;
                }
            } else if (n == 0) {
                eof = 1;
            } else if (errno != EAGAIN && errno != EINTR) {
                failed = 1;
                break;
            }
        }
    }
    close(pipes[0]);
    if (query->child) {
        /* This PID is our unreaped child, never an unrelated GPU process. */
        kill(query->child, SIGKILL);
        reap_query(query, &result);
        return -1;
    }
    if (failed || !eof || !exited || !WIFEXITED(result) ||
        WEXITSTATUS(result) != 0)
        return -1;
    output[used] = '\0';
    int hottest = -1;
    char *cursor = output;
    while (*cursor) {
        char *end;
        errno = 0;
        long temp = strtol(cursor, &end, 10);
        if (end == cursor || errno || temp < 15 || temp > 125)
            return -1;
        if (temp > hottest)
            hottest = (int)temp;
        while (*end == ' ' || *end == '\t' || *end == '\r')
            end++;
        if (*end && *end != '\n')
            return -1;
        cursor = *end ? end + 1 : end;
    }
    return hottest;
}
