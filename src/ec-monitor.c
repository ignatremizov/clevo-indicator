#define _GNU_SOURCE
#include "ec-monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
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

int ec_sample_read(int fd, EcReadAt read_at, EcSample *sample, unsigned fields)
{
    EcSample next = *sample;
    uint8_t duty[2];
    if (fields & ~EC_READ_ALL)
        return -1;
    if ((fields & EC_READ_CPU) &&
        read_exact(fd, read_at, &next.cpu_temp, 1, EC_REG_CPU_TEMP) != 0)
        return -1;
    if ((fields & EC_READ_GPU) &&
        read_exact(fd, read_at, &next.gpu_temp, 1, EC_REG_GPU_TEMP) != 0)
        return -1;
    if (fields & EC_READ_DUTY) {
        if (read_exact(fd, read_at, duty, sizeof(duty), EC_REG_CPU_FAN_DUTY) != 0)
            return -1;
        next.cpu_duty = duty[0];
        next.gpu_duty = duty[1];
    }
    if ((fields & EC_READ_RPM) &&
        read_exact(fd, read_at, next.rpm, sizeof(next.rpm),
                   EC_REG_CPU_FAN_RPMS_HI) != 0)
        return -1;
    *sample = next; /* Never publish a partial sample. */
    return 0;
}

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n;
    do {
        n = read(fd, buf, size - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0 || (size_t)n >= size - 1)
        return -1;
    buf[n] = '\0';
    return 0;
}

int ec_coretemp_read(const char *root)
{
    char pattern[PATH_MAX], path[PATH_MAX], text[128];
    if (snprintf(pattern, sizeof(pattern), "%s/hwmon*/name", root) >=
        (int)sizeof(pattern))
        return -1;
    glob_t devices = {0};
    if (glob(pattern, 0, NULL, &devices) != 0) {
        globfree(&devices);
        return -1;
    }
    int hottest = -1, failed = 0;
    for (size_t i = 0; i < devices.gl_pathc; i++) {
        if (read_text(devices.gl_pathv[i], text, sizeof(text)) != 0 ||
            (strcmp(text, "coretemp\n") && strcmp(text, "coretemp")))
            continue;
        size_t prefix = strlen(devices.gl_pathv[i]) - strlen("name");
        if (snprintf(pattern, sizeof(pattern), "%.*stemp*_label",
                     (int)prefix, devices.gl_pathv[i]) >= (int)sizeof(pattern)) {
            failed = 1;
            continue;
        }
        glob_t labels = {0};
        if (glob(pattern, 0, NULL, &labels) != 0) {
            failed = 1;
            globfree(&labels);
            continue;
        }
        int packages = 0;
        for (size_t j = 0; j < labels.gl_pathc; j++) {
            if (read_text(labels.gl_pathv[j], text, sizeof(text)) != 0) {
                failed = 1;
                continue;
            }
            unsigned id;
            char extra;
            if (sscanf(text, "Package id %u %c", &id, &extra) != 1)
                continue;
            packages++;
            size_t base = strlen(labels.gl_pathv[j]) - strlen("label");
            if (snprintf(path, sizeof(path), "%.*sinput", (int)base,
                         labels.gl_pathv[j]) >= (int)sizeof(path) ||
                read_text(path, text, sizeof(text)) != 0) {
                failed = 1;
                continue;
            }
            char *end;
            errno = 0;
            long milli = strtol(text, &end, 10);
            if (errno || end == text || (*end && strcmp(end, "\n")) ||
                milli < 15000 || milli > 125000) {
                failed = 1;
                continue;
            }
            int value = (int)((milli + 999) / 1000);
            if (value > hottest)
                hottest = value;
        }
        if (!packages)
            failed = 1;
        globfree(&labels);
    }
    globfree(&devices);
    return failed ? -1 : hottest;
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
