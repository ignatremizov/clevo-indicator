#include "ec-monitor.h"

#include <errno.h>
#include <string.h>

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
