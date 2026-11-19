#include "talk.h"

#include "log.h"

#include <string.h>

static int host_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    return c == '.' || c == '-';
}

static uint64_t read_seq(const uint8_t *buf)
{
    uint64_t seq;
    int i;

    seq = 0;
    for (i = 0; i < 8; i++) {
        seq = (seq << 8) | buf[1 + i];
    }
    return seq;
}

int talk_parse(const uint8_t *buf, int len, struct talk_cmd *cmd)
{
    int hlen;
    int i;

    if (buf == NULL || cmd == NULL || len < 9) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = buf[0];
    cmd->seq = read_seq(buf);
    if (cmd->cmd != TALK_START && cmd->cmd != TALK_STOP) {
        return 1;
    }
    if (cmd->cmd == TALK_STOP) {
        return len == 9 ? 0 : -1;
    }
    if (len < 12) {
        return -1;
    }
    cmd->port = (buf[9] << 8) | buf[10];
    hlen = buf[11];
    if (cmd->port <= 0 || cmd->port > 65535 || hlen <= 0 || hlen > TALK_HOST_MAX ||
        len != 12 + hlen) {
        return -1;
    }
    for (i = 0; i < hlen; i++) {
        if (!host_char((char)buf[12 + i])) {
            return -1;
        }
        cmd->host[i] = (char)buf[12 + i];
    }
    cmd->host[hlen] = '\0';
    return 0;
}

void talk_init(struct talk *talk, struct voice *voice)
{
    if (talk == NULL) {
        return;
    }
    memset(talk, 0, sizeof(*talk));
    talk->voice = voice;
}

#ifndef _WIN32

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define TALK_PT     96
#define TALK_FRAME  640
#define TALK_IDLE   15000
#define TALK_SSRC   0x48474432u

static int send_ack(int (*publish)(void *, const uint8_t *, int), void *ctx,
                    uint64_t seq, uint8_t origin, uint8_t code)
{
    uint8_t out[16];
    int n;

    n = voice_ack(out, (int)sizeof(out), seq, origin, code);
    if (n < 0 || publish(ctx, out, n) != 0) {
        return -1;
    }
    return 0;
}

static int set_nonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void silence_child(int keep_fd)
{
    int nul;

    nul = open("/dev/null", O_RDWR);
    if (nul < 0) {
        return;
    }
    if (keep_fd != 0) {
        dup2(nul, 0);
    }
    if (keep_fd != 1) {
        dup2(nul, 1);
    }
    dup2(nul, 2);
    if (nul > 2) {
        close(nul);
    }
}

/* child_stdio 为 1 时子进程写管道，为 0 时子进程读管道。 */
static pid_t spawn_pipe(char *const argv[], int *parent_fd, int child_stdio)
{
    int sp[2];
    pid_t pid;

    if (pipe(sp) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(sp[0]);
        close(sp[1]);
        return -1;
    }
    if (pid == 0) {
        if (child_stdio == 1) {
            dup2(sp[1], 1);
            silence_child(1);
        } else {
            dup2(sp[0], 0);
            silence_child(0);
        }
        close(sp[0]);
        close(sp[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (child_stdio == 1) {
        *parent_fd = sp[0];
        close(sp[1]);
    } else {
        *parent_fd = sp[1];
        close(sp[0]);
    }
    if (set_nonblock(*parent_fd) != 0) {
        int st;

        kill(pid, SIGTERM);
        waitpid(pid, &st, 0);
        close(*parent_fd);
        *parent_fd = -1;
        return -1;
    }
    return pid;
}

static int udp_connect(const char *host, int port)
{
    char port_text[8];
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *it;
    int fd;

    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port_text, &hints, &res) != 0) {
        return -1;
    }
    fd = -1;
    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0 && set_nonblock(fd) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int rtp_send(int fd, unsigned short *seq, uint32_t *stamp,
                    const uint8_t *pcm, int n)
{
    uint8_t pkt[12 + TALK_FRAME];

    if (n <= 0 || n > TALK_FRAME) {
        return -1;
    }
    pkt[0] = 0x80;
    pkt[1] = TALK_PT;
    pkt[2] = (uint8_t)(*seq >> 8);
    pkt[3] = (uint8_t)*seq;
    pkt[4] = (uint8_t)(*stamp >> 24);
    pkt[5] = (uint8_t)(*stamp >> 16);
    pkt[6] = (uint8_t)(*stamp >> 8);
    pkt[7] = (uint8_t)*stamp;
    pkt[8] = (uint8_t)(TALK_SSRC >> 24);
    pkt[9] = (uint8_t)(TALK_SSRC >> 16);
    pkt[10] = (uint8_t)(TALK_SSRC >> 8);
    pkt[11] = (uint8_t)TALK_SSRC;
    memcpy(pkt + 12, pcm, (size_t)n);
    if (send(fd, pkt, (size_t)(12 + n), 0) != 12 + n) {
        return -1;
    }
    *seq = (unsigned short)(*seq + 1u);
    *stamp += (uint32_t)(n / 2);
    return 0;
}

static int elapsed_ms(const struct timeval *start)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (int)((now.tv_sec - start->tv_sec) * 1000 +
                 (now.tv_usec - start->tv_usec) / 1000);
}

static void kill_child(pid_t pid)
{
    int st;

    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
}

static int play_rtp(const uint8_t *pkt, int n, int play_fd)
{
    int cc;
    int off;
    int left;

    if (n < 12 || (pkt[0] & 0xC0) != 0x80 || (pkt[1] & 0x7F) != TALK_PT) {
        return 0;
    }
    cc = pkt[0] & 0x0F;
    off = 12 + cc * 4;
    if ((pkt[0] & 0x10) != 0 || off >= n) {
        return 0;
    }
    left = n - off;
    while (left > 0) {
        int w = (int)write(play_fd, pkt + off, (size_t)left);

        if (w <= 0) {
            if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
                return 0;
            }
            return w < 0 ? -1 : 0;
        }
        off += w;
        left -= w;
    }
    return 1;
}

static int run_session(struct talk *talk, const struct talk_cmd *cmd,
                       int (*publish)(void *, const uint8_t *, int),
                       int (*pump)(void *, int), void *ctx)
{
    char *rec[] = { "arecord", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1", "-t", "raw", "-", NULL };
    char *play[] = { "aplay", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1", "-t", "raw", "-", NULL };
    uint8_t mic[TALK_FRAME];
    uint8_t silence[TALK_FRAME];
    uint8_t pkt[1500];
    struct timeval activity;
    struct timeval ping_at;
    unsigned short seq;
    uint32_t stamp;
    int mic_n;
    int udp;
    int rec_fd;
    int play_fd;
    pid_t rec_pid;
    pid_t play_pid;
    uint8_t end_code;

    udp = udp_connect(cmd->host, cmd->port);
    if (udp < 0) {
        log_error("对讲连不上 %s:%d", cmd->host, cmd->port);
        return send_ack(publish, ctx, cmd->seq, TALK_START, VOICE_FAIL);
    }
    rec_fd = -1;
    play_fd = -1;
    rec_pid = spawn_pipe(rec, &rec_fd, 1);
    play_pid = spawn_pipe(play, &play_fd, 0);
    if (rec_pid <= 0 || play_pid <= 0 || voice_amp(talk->voice, 1) != 0) {
        kill_child(rec_pid);
        kill_child(play_pid);
        if (rec_fd >= 0) {
            close(rec_fd);
        }
        if (play_fd >= 0) {
            close(play_fd);
        }
        close(udp);
        voice_amp(talk->voice, 0);
        log_error("对讲打不开录音、播放或功放");
        return send_ack(publish, ctx, cmd->seq, TALK_START, VOICE_FAIL);
    }

    talk->active = 1;
    talk->stop_req = 0;
    talk->start_seq = cmd->seq;
    talk->voice->busy = 1;
    if (send_ack(publish, ctx, cmd->seq, TALK_START, VOICE_STARTED) != 0) {
        end_code = 0;
        goto finish;
    }

    memset(silence, 0, sizeof(silence));
    seq = 1;
    stamp = 0;
    mic_n = 0;
    end_code = TALK_BROKEN;
    if (rtp_send(udp, &seq, &stamp, silence, TALK_FRAME) != 0) {
        goto finish;
    }
    gettimeofday(&activity, NULL);
    ping_at = activity;
    log_info("对讲开始，RTP 发往 %s:%d", cmd->host, cmd->port);

    while (!talk->stop_req) {
        struct pollfd pfd[2];
        int ready;
        int n;

        pfd[0].fd = udp;
        pfd[0].events = POLLIN;
        pfd[1].fd = rec_fd;
        pfd[1].events = POLLIN;
        ready = poll(pfd, 2, 200);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (pump != NULL && pump(ctx, 0) != 0) {
            end_code = TALK_BROKEN;
            goto finish;
        }
        if (talk->stop_req) {
            break;
        }
        if (ready > 0 && (pfd[0].revents & POLLIN)) {
            for (;;) {
                int played;

                n = (int)recv(udp, pkt, sizeof(pkt), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR) {
                        break;
                    }
                    goto finish;
                }
                if (n == 0) {
                    break;
                }
                played = play_rtp(pkt, n, play_fd);
                if (played < 0) {
                    goto finish;
                }
                if (played > 0) {
                    gettimeofday(&activity, NULL);
                }
            }
        }
        if (ready > 0 && (pfd[1].revents & POLLIN)) {
            n = (int)read(rec_fd, mic + mic_n, (size_t)(TALK_FRAME - mic_n));
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                goto finish;
            }
            if (n == 0) {
                goto finish;
            }
            if (n > 0) {
                mic_n += n;
            }
            if (mic_n == TALK_FRAME) {
                if (rtp_send(udp, &seq, &stamp, mic, TALK_FRAME) != 0) {
                    goto finish;
                }
                mic_n = 0;
                gettimeofday(&activity, NULL);
            }
        }
        if (elapsed_ms(&activity) >= TALK_IDLE) {
            end_code = TALK_TIMEOUT;
            log_error("对讲 15 秒没有声音");
            goto finish;
        }
        if (elapsed_ms(&ping_at) >= 10000) {
            if (pump != NULL && pump(ctx, -1) != 0) {
                goto finish;
            }
            gettimeofday(&ping_at, NULL);
        }
    }
    end_code = talk->stop_req ? VOICE_OK : end_code;

finish:
    talk->active = 0;
    talk->stop_req = 0;
    if (talk->voice != NULL) {
        talk->voice->busy = 0;
        voice_amp(talk->voice, 0);
    }
    kill_child(rec_pid);
    kill_child(play_pid);
    if (rec_fd >= 0) {
        close(rec_fd);
    }
    if (play_fd >= 0) {
        close(play_fd);
    }
    close(udp);
    if (end_code == 0) {
        return -1;
    }
    log_info("对讲结束，结果 %02X", end_code);
    return send_ack(publish, ctx, cmd->seq, TALK_START, end_code);
}

#endif

int talk_on_packet(struct talk *talk, const uint8_t *buf, int len,
                   int (*publish)(void *ctx, const uint8_t *data, int n),
                   int (*pump)(void *ctx, int idle_ms), void *ctx)
{
#ifdef _WIN32
    (void)talk;
    (void)buf;
    (void)len;
    (void)publish;
    (void)pump;
    (void)ctx;
    return -1;
#else
    struct talk_cmd cmd;
    int rc;

    if (talk == NULL || talk->voice == NULL || publish == NULL) {
        return -1;
    }
    rc = talk_parse(buf, len, &cmd);
    if (rc > 0) {
        return 1;
    }
    if (rc < 0) {
        if (buf != NULL && len >= 9 && (buf[0] == TALK_START || buf[0] == TALK_STOP)) {
            return send_ack(publish, ctx, read_seq(buf), buf[0], VOICE_FAIL);
        }
        return 0;
    }
    if (cmd.cmd == TALK_STOP) {
        if (!talk->active) {
            return send_ack(publish, ctx, cmd.seq, TALK_STOP, VOICE_FAIL);
        }
        talk->stop_req = 1;
        log_info("对讲收到结束");
        return send_ack(publish, ctx, cmd.seq, TALK_STOP, VOICE_OK);
    }
    if (talk->active || talk->voice->busy) {
        return send_ack(publish, ctx, cmd.seq, TALK_START, VOICE_FAIL);
    }
    return run_session(talk, &cmd, publish, pump, ctx);
#endif
}
