#include "admin.h"

#include "http_broker.h"
#include "log.h"
#include "voice.h"

#include <stdio.h>
#include <string.h>

static struct admin *current;

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

static void put_u64(uint8_t *out, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[7 - i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
    }
}

int admin_is_busy(void)
{
    return current != NULL && current->busy;
}

int admin_parse(const uint8_t *buf, int len, struct admin_cmd *cmd)
{
    int n;
    int i;

    if (buf == NULL || cmd == NULL || len < 9) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = buf[0];
    cmd->seq = read_seq(buf);
    if (cmd->cmd != ADMIN_UPGRADE && cmd->cmd != ADMIN_REBOOT &&
        cmd->cmd != ADMIN_RESET && cmd->cmd != ADMIN_VERSION) {
        return 1;
    }
    if (cmd->cmd != ADMIN_UPGRADE) {
        return len == 9 ? 0 : -1;
    }
    if (len < 28) {
        return -1;
    }
    memcpy(cmd->md5, buf + 9, 16);
    n = (buf[25] << 8) | buf[26];
    if (n <= 0 || n > ADMIN_URL_MAX || len != 27 + n) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        cmd->url[i] = (char)buf[27 + i];
    }
    cmd->url[n] = '\0';
    if (strncmp(cmd->url, "http://", 7) != 0) {
        return -1;
    }
    return 0;
}

int admin_version(uint8_t *out, int cap, uint64_t seq, const char *firmware)
{
    int n;
    int i;

    if (out == NULL || firmware == NULL) {
        return -1;
    }
    n = (int)strlen(firmware);
    if (n <= 0 || n > 63 || cap < 11 + n) {
        return -1;
    }
    out[0] = ADMIN_VER_REPLY;
    put_u64(out + 1, seq);
    out[9] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        out[10 + i] = (uint8_t)firmware[i];
    }
    return 10 + n;
}

void admin_init(struct admin *admin, const struct app_config *cfg,
                struct voice *voice, struct bus *bus)
{
    if (admin == NULL) {
        return;
    }
    memset(admin, 0, sizeof(*admin));
    admin->voice = voice;
    admin->bus = bus;
    snprintf(admin->firmware, sizeof(admin->firmware), "%s",
             cfg != NULL && cfg->firmware[0] != '\0' ? cfg->firmware : "HY-DG02");
    snprintf(admin->voice_dir, sizeof(admin->voice_dir), "%s",
             cfg != NULL && cfg->voice_dir[0] != '\0' ? cfg->voice_dir : "/etc/hydg02/voice");
    current = admin;
}

static int send_ack(int (*publish)(void *, const uint8_t *, int), void *ctx,
                    uint64_t seq, uint8_t origin, uint8_t code)
{
    uint8_t out[16];
    int n;

    n = voice_ack(out, (int)sizeof(out), seq, origin, code);
    if (n < 0 || publish == NULL || publish(ctx, out, n) != 0) {
        return -1;
    }
    return 0;
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#define UPGRADE_PATH "/tmp/hydg02-upgrade.bin"
#define UPGRADE_MAX  (15 * 1024 * 1024)

static int launch(const char *path, char *const argv[])
{
    pid_t pid;
    int nul;

    if (access(path, X_OK) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2) {
                close(nul);
            }
        }
        execv(path, argv);
        _exit(127);
    }
    return pid;
}

static void clear_state(const struct admin *admin)
{
    char path[256];
    static const char *exts[] = { "wav", "mp3", "pcm", "opus" };
    int slot;
    int i;

    unlink("/etc/hydg02/led.bin");
    unlink("/etc/hydg02/trig.bin");
    snprintf(path, sizeof(path), "%s/enabled", admin->voice_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/download.tmp", admin->voice_dir);
    unlink(path);
    for (slot = 1; slot <= 3; slot++) {
        for (i = 0; i < 4; i++) {
            snprintf(path, sizeof(path), "%s/%d.%s", admin->voice_dir, slot, exts[i]);
            unlink(path);
        }
    }
}

static void upgrade_idle(struct admin *admin)
{
    admin->busy = 0;
    if (admin->voice != NULL) {
        admin->voice->busy = 0;
    }
    if (admin->bus != NULL) {
        admin->bus->busy = 0;
    }
}

static int do_reboot(void)
{
    char *argv[] = { "reboot", NULL };
    pid_t pid;
    int st;

    pid = launch("/sbin/reboot", argv);
    if (pid < 0) {
        return -1;
    }
    if (waitpid(pid, &st, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return -1;
    }
    return 0;
}

static int do_upgrade(struct admin *admin, const struct admin_cmd *cmd,
                      int (*publish)(void *, const uint8_t *, int),
                      int (*pump)(void *, int), void *ctx)
{
    uint8_t digest[16];
    char *argv[] = { "sysupgrade", UPGRADE_PATH, NULL };
    pid_t pid;
    int st;
    int i;

    if (admin->busy || (admin->voice != NULL && admin->voice->busy) ||
        (admin->bus != NULL && admin->bus->busy)) {
        return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_FAIL);
    }
    admin->busy = 1;
    if (admin->voice != NULL) {
        admin->voice->busy = 1;
    }
    if (admin->bus != NULL) {
        admin->bus->busy = 1;
    }
    if (send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_STARTED) != 0) {
        upgrade_idle(admin);
        return -1;
    }
    if (http_save(cmd->url, UPGRADE_PATH, digest, UPGRADE_MAX, pump, ctx) != 0) {
        unlink(UPGRADE_PATH);
        log_error("升级包下载失败");
        upgrade_idle(admin);
        return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_FAIL);
    }
    if (memcmp(digest, cmd->md5, 16) != 0) {
        unlink(UPGRADE_PATH);
        log_error("升级包 MD5 不一致，继续用当前版本");
        upgrade_idle(admin);
        return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_BAD_MD5);
    }
    pid = launch("/sbin/sysupgrade", argv);
    if (pid < 0) {
        unlink(UPGRADE_PATH);
        upgrade_idle(admin);
        return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_FAIL);
    }
    for (i = 0; i < 6; i++) {
        pid_t got;

        got = waitpid(pid, &st, WNOHANG);
        if (got == pid) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                log_info("升级包校验通过，开始安装并重启");
                return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_OK);
            }
            unlink(UPGRADE_PATH);
            upgrade_idle(admin);
            log_error("sysupgrade 没有接受升级包");
            return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_FAIL);
        }
        if (got < 0) {
            upgrade_idle(admin);
            return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_FAIL);
        }
        usleep(500000);
    }
    log_info("升级包校验通过，开始安装并重启");
    return send_ack(publish, ctx, cmd->seq, ADMIN_UPGRADE, VOICE_OK);
}

#endif

int admin_on_packet(struct admin *admin, const uint8_t *buf, int len,
                    int (*publish)(void *ctx, const uint8_t *data, int n),
                    void *ctx)
{
#ifdef _WIN32
    (void)admin;
    (void)buf;
    (void)len;
    (void)publish;
    (void)ctx;
    return -1;
#else
    struct admin_cmd cmd;
    uint8_t out[80];
    int n;
    int rc;

    if (admin == NULL || publish == NULL) {
        return -1;
    }
    rc = admin_parse(buf, len, &cmd);
    if (rc > 0 || cmd.cmd == ADMIN_UPGRADE) {
        return 1;
    }
    if (rc < 0) {
        if (buf != NULL && len >= 9 && buf[0] >= ADMIN_REBOOT && buf[0] <= ADMIN_VERSION) {
            return send_ack(publish, ctx, read_seq(buf), buf[0], VOICE_FAIL);
        }
        return 0;
    }
    if (cmd.cmd == ADMIN_VERSION) {
        n = admin_version(out, (int)sizeof(out), cmd.seq, admin->firmware);
        if (n < 0 || publish(ctx, out, n) != 0) {
            return -1;
        }
        return 0;
    }
    if (admin->busy) {
        return send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_FAIL);
    }
    if (cmd.cmd == ADMIN_RESET) {
        clear_state(admin);
        if (admin->voice != NULL) {
            admin->voice->enabled = 1;
        }
        log_info("已清除语音、屏幕和触发参数");
    }
    if (send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_OK) != 0) {
        return -1;
    }
    if (do_reboot() != 0) {
        log_error("reboot 没有执行");
    }
    return 0;
#endif
}

int admin_on_upgrade(struct admin *admin, const uint8_t *buf, int len,
                     int (*publish)(void *ctx, const uint8_t *data, int n),
                     int (*pump)(void *ctx, int idle_ms), void *ctx)
{
#ifdef _WIN32
    (void)admin;
    (void)buf;
    (void)len;
    (void)publish;
    (void)pump;
    (void)ctx;
    return -1;
#else
    struct admin_cmd cmd;
    int rc;

    if (admin == NULL || publish == NULL) {
        return -1;
    }
    rc = admin_parse(buf, len, &cmd);
    if (rc != 0 || cmd.cmd != ADMIN_UPGRADE) {
        if (buf != NULL && len >= 9) {
            return send_ack(publish, ctx, read_seq(buf), buf[0], VOICE_FAIL);
        }
        return 0;
    }
    return do_upgrade(admin, &cmd, publish, pump, ctx);
#endif
}
