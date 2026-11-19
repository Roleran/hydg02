#include "voice.h"

#include "http_broker.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static const char *const exts[] = { "wav", "mp3", "pcm", "opus" };

static void put_u64(uint8_t *out, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[7 - i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
    }
}

static int same_word(const char *text, const char *word)
{
    int i;

    for (i = 0; word[i] != '\0'; i++) {
        char c = text[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != word[i]) {
            return 0;
        }
    }
    return text[i] == '\0';
}

int voice_known_suffix(const char *url)
{
    const char *slash;
    const char *dot;
    int i;

    if (url == NULL) {
        return 0;
    }
    slash = strrchr(url, '/');
    dot = strrchr(url, '.');
    if (dot == NULL || dot == url || (slash != NULL && dot < slash)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (same_word(dot + 1, exts[i])) {
            return 1;
        }
    }
    return 0;
}

int voice_parse(const uint8_t *buf, int len, struct voice_cmd *cmd)
{
    int ulen;
    int i;

    if (buf == NULL || cmd == NULL || len < 9) {
        return -1;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = buf[0];
    for (i = 0; i < 8; i++) {
        cmd->seq = (cmd->seq << 8) | buf[1 + i];
    }
    if (cmd->cmd == VOICE_QUERY || cmd->cmd == VOICE_CLEAR || cmd->cmd == VOICE_PLAY) {
        return len == 9 ? 0 : -1;
    }
    if (cmd->cmd == VOICE_SET) {
        if (len != 10 || (buf[9] != 0 && buf[9] != 1)) {
            return -1;
        }
        cmd->on = buf[9];
        return 0;
    }
    if (cmd->cmd != VOICE_FETCH) {
        return 1;
    }
    if (len < 28) {
        return -1;
    }
    cmd->slot = buf[9];
    if (cmd->slot < 1 || cmd->slot > VOICE_SLOTS) {
        return -1;
    }
    memcpy(cmd->md5, buf + 10, 16);
    ulen = (buf[26] << 8) | buf[27];
    if (ulen <= 0 || ulen > VOICE_URL_MAX || len != 28 + ulen) {
        return -1;
    }
    memcpy(cmd->url, buf + 28, (size_t)ulen);
    cmd->url[ulen] = '\0';
    return 0;
}

int voice_ack(uint8_t *out, int cap, uint64_t seq, uint8_t origin, uint8_t code)
{
    if (out == NULL || cap < 11) {
        return -1;
    }
    out[0] = VOICE_RESULT;
    put_u64(out + 1, seq);
    out[9] = origin;
    out[10] = code;
    return 11;
}

int voice_state(uint8_t *out, int cap, uint64_t seq, int on, int count)
{
    if (out == NULL || cap < 11) {
        return -1;
    }
    out[0] = VOICE_STATE;
    put_u64(out + 1, seq);
    out[9] = (uint8_t)(on ? 1 : 0);
    out[10] = (uint8_t)count;
    return 11;
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int join_path(char *out, int cap, const char *dir, const char *name)
{
    int n;

    n = snprintf(out, (size_t)cap, "%s/%s", dir, name);
    if (n <= 0 || n >= cap) {
        return -1;
    }
    return 0;
}

static int slot_file(const struct voice *voice, int slot, const char *ext, char *out, int cap)
{
    char name[16];

    snprintf(name, sizeof(name), "%d.%s", slot, ext);
    return join_path(out, cap, voice->dir, name);
}

static int find_slot(const struct voice *voice, int slot, char *out, int cap)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (slot_file(voice, slot, exts[i], out, cap) != 0) {
            return -1;
        }
        if (access(out, R_OK) == 0) {
            return 0;
        }
    }
    return -1;
}

static int file_count(const struct voice *voice)
{
    char path[256];
    int slot;
    int n;

    n = 0;
    for (slot = 1; slot <= VOICE_SLOTS; slot++) {
        if (find_slot(voice, slot, path, (int)sizeof(path)) == 0) {
            n++;
        }
    }
    return n;
}

static int write_text(const char *path, const char *text)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (fp == NULL) {
        return -1;
    }
    if (fputs(text, fp) < 0) {
        fclose(fp);
        return -1;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

static void load_switch(struct voice *voice)
{
    char path[256];
    FILE *fp;
    int c;

    voice->enabled = 1;
    if (join_path(path, (int)sizeof(path), voice->dir, "enabled") != 0) {
        return;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        return;
    }
    c = fgetc(fp);
    fclose(fp);
    if (c == '0') {
        voice->enabled = 0;
    }
}

static int save_switch(const struct voice *voice)
{
    char path[256];

    if (join_path(path, (int)sizeof(path), voice->dir, "enabled") != 0) {
        return -1;
    }
    return write_text(path, voice->enabled ? "1\n" : "0\n");
}

static int amp_set(int gpio, int on)
{
    char path[80];
    char num[16];
    FILE *fp;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    fp = fopen(path, "w");
    if (fp == NULL) {
        snprintf(num, sizeof(num), "%d", gpio);
        if (write_text("/sys/class/gpio/export", num) != 0) {
            return -1;
        }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
        if (write_text(path, "out") != 0) {
            return -1;
        }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    } else {
        fclose(fp);
    }
    return write_text(path, on ? "1" : "0");
}

static void silence_child(int keep_stdin)
{
    int nul;

    nul = open("/dev/null", O_RDWR);
    if (nul < 0) {
        return;
    }
    if (!keep_stdin) {
        dup2(nul, 0);
    }
    dup2(nul, 2);
    if (nul > 2) {
        close(nul);
    }
}

static int wait_pid(pid_t pid, int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    for (;;) {
        int st;
        pid_t got;

        got = waitpid(pid, &st, WNOHANG);
        if (got == pid) {
            return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
        }
        if (got < 0) {
            return -1;
        }
        if (pump != NULL && pump(ctx, 1000) != 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &st, 0);
            return -1;
        }
    }
}

static int play_direct(char *const argv[], int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        silence_child(0);
        execvp(argv[0], argv);
        _exit(127);
    }
    return wait_pid(pid, pump, ctx);
}

static int play_pipe(char *const dec[], char *const play[],
                     int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    int sp[2];
    pid_t dec_pid;
    pid_t play_pid;
    int st;
    int failed;

    if (pipe(sp) != 0) {
        return -1;
    }
    dec_pid = fork();
    if (dec_pid < 0) {
        close(sp[0]);
        close(sp[1]);
        return -1;
    }
    if (dec_pid == 0) {
        dup2(sp[1], 1);
        close(sp[0]);
        close(sp[1]);
        silence_child(0);
        execvp(dec[0], dec);
        _exit(127);
    }
    play_pid = fork();
    if (play_pid < 0) {
        kill(dec_pid, SIGTERM);
        waitpid(dec_pid, &st, 0);
        close(sp[0]);
        close(sp[1]);
        return -1;
    }
    if (play_pid == 0) {
        dup2(sp[0], 0);
        close(sp[0]);
        close(sp[1]);
        silence_child(1);
        execvp(play[0], play);
        _exit(127);
    }
    close(sp[0]);
    close(sp[1]);
    failed = 0;
    if (wait_pid(dec_pid, pump, ctx) != 0) {
        failed = 1;
    }
    if (wait_pid(play_pid, pump, ctx) != 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}

static int play_path(const char *path, int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    const char *dot;
    char *wav[] = { "aplay", "-q", NULL, NULL };
    char *pcm[] = { "aplay", "-q", "-f", "S16_LE", "-r", "48000", "-c", "1", NULL, NULL };
    char *mp3[] = { "madplay", "-Q", "-o", "wave:-", NULL, NULL };
    char *opus[] = { "opusdec", "--quiet", NULL, "-", NULL };
    char *speaker[] = { "aplay", "-q", "-", NULL };

    dot = strrchr(path, '.');
    if (dot == NULL) {
        return -1;
    }
    if (same_word(dot + 1, "wav")) {
        wav[2] = (char *)path;
        return play_direct(wav, pump, ctx);
    }
    if (same_word(dot + 1, "pcm")) {
        pcm[8] = (char *)path;
        return play_direct(pcm, pump, ctx);
    }
    if (same_word(dot + 1, "mp3")) {
        mp3[4] = (char *)path;
        return play_pipe(mp3, speaker, pump, ctx);
    }
    if (same_word(dot + 1, "opus")) {
        opus[2] = (char *)path;
        return play_pipe(opus, speaker, pump, ctx);
    }
    return -1;
}

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

static void remove_slot(const struct voice *voice, int slot, const char *keep_ext)
{
    char path[256];
    int i;

    for (i = 0; i < 4; i++) {
        if (keep_ext != NULL && same_word(exts[i], keep_ext)) {
            continue;
        }
        if (slot_file(voice, slot, exts[i], path, (int)sizeof(path)) == 0) {
            unlink(path);
        }
    }
}

static const char *url_ext(const char *url)
{
    const char *dot;
    int i;

    dot = strrchr(url, '.');
    if (dot == NULL) {
        return NULL;
    }
    for (i = 0; i < 4; i++) {
        if (same_word(dot + 1, exts[i])) {
            return exts[i];
        }
    }
    return NULL;
}

static int do_fetch(struct voice *voice, const struct voice_cmd *cmd,
                    int (*publish)(void *, const uint8_t *, int),
                    int (*pump)(void *, int), void *ctx)
{
    char tmp[256];
    char final_path[256];
    uint8_t digest[16];
    const char *ext;
    int rc;

    ext = url_ext(cmd->url);
    if (ext == NULL || strncmp(cmd->url, "http://", 7) != 0) {
        return send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_FAIL);
    }
    voice->busy = 1;
    if (send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_STARTED) != 0) {
        voice->busy = 0;
        return -1;
    }
    if (join_path(tmp, (int)sizeof(tmp), voice->dir, "download.tmp") != 0 ||
        slot_file(voice, cmd->slot, ext, final_path, (int)sizeof(final_path)) != 0) {
        voice->busy = 0;
        return send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_FAIL);
    }
    rc = http_save(cmd->url, tmp, digest, 2 * 1024 * 1024, pump, ctx);
    if (rc != 0) {
        unlink(tmp);
        voice->busy = 0;
        return send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_FAIL);
    }
    if (memcmp(digest, cmd->md5, 16) != 0) {
        unlink(tmp);
        voice->busy = 0;
        log_error("语音 MD5 不一致，槽位 %d 仍用旧文件", cmd->slot);
        return send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_BAD_MD5);
    }
    if (rename(tmp, final_path) != 0) {
        unlink(tmp);
        voice->busy = 0;
        return send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_FAIL);
    }
    remove_slot(voice, cmd->slot, ext);
    voice->busy = 0;
    log_info("语音已保存，槽位 %d", cmd->slot);
    return send_ack(publish, ctx, cmd->seq, VOICE_FETCH, VOICE_OK);
}

int voice_play_raw(struct voice *voice, int (*pump)(void *ctx, int idle_ms), void *ctx)
{
    char path[256];
    int slot;
    int played;

    if (voice == NULL) {
        return -1;
    }
    if (!voice->enabled || file_count(voice) == 0) {
        return 1;
    }
    if (voice->busy) {
        return -1;
    }
    voice->busy = 1;
    if (amp_set(voice->amp_gpio, 1) != 0) {
        voice->busy = 0;
        log_error("功放 GPIO%d 打不开", voice->amp_gpio);
        return -1;
    }
    played = 0;
    for (slot = 1; slot <= VOICE_SLOTS; slot++) {
        if (find_slot(voice, slot, path, (int)sizeof(path)) != 0) {
            continue;
        }
        played = 1;
        if (play_path(path, pump, ctx) != 0) {
            amp_set(voice->amp_gpio, 0);
            voice->busy = 0;
            log_error("播放失败 %s", path);
            return -1;
        }
    }
    amp_set(voice->amp_gpio, 0);
    voice->busy = 0;
    if (!played) {
        return 1;
    }
    log_info("播报完成");
    return 0;
}

static int do_play(struct voice *voice,
                   int (*publish)(void *, const uint8_t *, int),
                   int (*pump)(void *, int), void *ctx, uint64_t seq)
{
    int rc;

    if (!voice->enabled || file_count(voice) == 0 || voice->busy) {
        return send_ack(publish, ctx, seq, VOICE_PLAY, VOICE_FAIL);
    }
    if (send_ack(publish, ctx, seq, VOICE_PLAY, VOICE_STARTED) != 0) {
        return -1;
    }
    rc = voice_play_raw(voice, pump, ctx);
    return send_ack(publish, ctx, seq, VOICE_PLAY, rc == 0 ? VOICE_OK : VOICE_FAIL);
}

static int do_clear(struct voice *voice,
                    int (*publish)(void *, const uint8_t *, int),
                    void *ctx, uint64_t seq)
{
    int slot;

    if (voice->busy) {
        return send_ack(publish, ctx, seq, VOICE_CLEAR, VOICE_FAIL);
    }
    for (slot = 1; slot <= VOICE_SLOTS; slot++) {
        remove_slot(voice, slot, NULL);
    }
    log_info("语音文件已清空");
    return send_ack(publish, ctx, seq, VOICE_CLEAR, VOICE_OK);
}

int voice_amp(struct voice *voice, int on)
{
    if (voice == NULL) {
        return -1;
    }
    return amp_set(voice->amp_gpio, on);
}

#endif

void voice_init(struct voice *voice, const struct app_config *cfg)
{
    if (voice == NULL) {
        return;
    }
    memset(voice, 0, sizeof(*voice));
    voice->enabled = 1;
    voice->amp_gpio = cfg != NULL ? cfg->amp_gpio : 0;
    if (cfg != NULL && cfg->voice_dir[0] != '\0') {
        snprintf(voice->dir, sizeof(voice->dir), "%s", cfg->voice_dir);
    } else {
        snprintf(voice->dir, sizeof(voice->dir), "/etc/hydg02/voice");
    }
#ifndef _WIN32
    if (mkdir(voice->dir, 0755) != 0 && errno != EEXIST) {
        log_error("建不了语音目录 %s", voice->dir);
    }
    load_switch(voice);
#endif
}

int voice_on_packet(struct voice *voice, const uint8_t *buf, int len,
                    int (*publish)(void *ctx, const uint8_t *data, int n),
                    int (*pump)(void *ctx, int idle_ms), void *ctx)
{
#ifdef _WIN32
    (void)voice;
    (void)buf;
    (void)len;
    (void)publish;
    (void)pump;
    (void)ctx;
    return -1;
#else
    struct voice_cmd cmd;
    uint8_t out[16];
    int n;
    int rc;

    if (voice == NULL || publish == NULL) {
        return -1;
    }
    rc = voice_parse(buf, len, &cmd);
    if (rc < 0) {
        log_info("语音指令字段不对，%d 字节", len);
        return 0;
    }
    if (rc > 0) {
        log_info("先不做的业务命令 %02X", cmd.cmd);
        return send_ack(publish, ctx, cmd.seq, cmd.cmd, VOICE_FAIL);
    }
    if (cmd.cmd == VOICE_SET) {
        voice->enabled = cmd.on;
        if (save_switch(voice) != 0) {
            return send_ack(publish, ctx, cmd.seq, VOICE_SET, VOICE_FAIL);
        }
        log_info("语音开关=%d", voice->enabled);
        return send_ack(publish, ctx, cmd.seq, VOICE_SET, VOICE_OK);
    }
    if (cmd.cmd == VOICE_QUERY) {
        n = voice_state(out, (int)sizeof(out), cmd.seq, voice->enabled, file_count(voice));
        if (n < 0 || publish(ctx, out, n) != 0) {
            return -1;
        }
        return 0;
    }
    if (cmd.cmd == VOICE_CLEAR) {
        return do_clear(voice, publish, ctx, cmd.seq);
    }
    if (cmd.cmd == VOICE_FETCH) {
        if (voice->busy) {
            return send_ack(publish, ctx, cmd.seq, VOICE_FETCH, VOICE_FAIL);
        }
        return do_fetch(voice, &cmd, publish, pump, ctx);
    }
    if (voice->busy) {
        return send_ack(publish, ctx, cmd.seq, VOICE_PLAY, VOICE_FAIL);
    }
    return do_play(voice, publish, pump, ctx, cmd.seq);
#endif
}
