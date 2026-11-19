#include "at_port.h"

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_flag(int baud)
{
    if (baud == 9600) {
        return B9600;
    }
    return B115200;
}

int at_open(const char *path, int baud)
{
    int fd;
    struct termios tio;

    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (tcgetattr(fd, &tio) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_flag(baud));
    cfsetospeed(&tio, baud_flag(baud));
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int append_resp(char *resp, int cap, int used, const char *src, int n)
{
    int i;

    for (i = 0; i < n && used < cap - 1; i++) {
        resp[used++] = src[i];
    }
    resp[used] = '\0';
    return used;
}

int at_exchange(int fd, const char *cmd, char *resp, int cap, int timeout_ms)
{
    char tx[160];
    char chunk[128];
    int n;
    int used;
    int left;
    struct pollfd pfd;

    if (fd < 0 || cmd == NULL || resp == NULL || cap < 2) {
        return -1;
    }
    n = snprintf(tx, sizeof(tx), "%s\r", cmd);
    if (n <= 0 || n >= (int)sizeof(tx)) {
        return -1;
    }
    if (write(fd, tx, (size_t)n) != n) {
        return -1;
    }

    resp[0] = '\0';
    used = 0;
    left = timeout_ms;
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (left > 0) {
        int wait = left > 200 ? 200 : left;
        int pr = poll(&pfd, 1, wait);

        left -= wait;
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            continue;
        }
        n = (int)read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            continue;
        }
        used = append_resp(resp, cap, used, chunk, n);
        if (strstr(resp, "ERROR") != NULL) {
            return -1;
        }
        if (strstr(resp, "OK") != NULL) {
            return 0;
        }
    }
    return -1;
}

void at_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

#else

int at_open(const char *path, int baud)
{
    (void)path;
    (void)baud;
    return -1;
}

int at_exchange(int fd, const char *cmd, char *resp, int cap, int timeout_ms)
{
    (void)fd;
    (void)cmd;
    (void)resp;
    (void)cap;
    (void)timeout_ms;
    return -1;
}

void at_close(int fd)
{
    (void)fd;
}

#endif
