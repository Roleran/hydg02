#ifndef AT_PORT_H
#define AT_PORT_H

/* 打开 EC200A 的 AT 口。失败返回 -1。 */
int at_open(const char *path, int baud);

/* 发送一条 AT，把应答写入 resp。见到 OK 返回 0，ERROR 或超时返回 -1。 */
int at_exchange(int fd, const char *cmd, char *resp, int cap, int timeout_ms);

void at_close(int fd);

#endif
