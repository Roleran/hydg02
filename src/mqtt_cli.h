#ifndef MQTT_CLI_H
#define MQTT_CLI_H

/* MQTT 3.1.1，只做干净会话、订阅和 QoS 0 发布。失败返回 -1。 */
struct mqtt;

struct mqtt *mqtt_connect(const char *host, int port, const char *client_id);
int mqtt_subscribe(struct mqtt *mq, const char *topic);
int mqtt_publish(struct mqtt *mq, const char *topic,
                 const unsigned char *payload, int len);

/*
 * 等到一条发布。
 * 返回 1 时 topic/payload 有效。
 * 返回 0 是超时。返回 -1 是连接断了。
 */
int mqtt_wait_publish(struct mqtt *mq, char *topic, int topic_cap,
                      unsigned char *payload, int *payload_len, int payload_cap,
                      int timeout_ms);

int mqtt_ping(struct mqtt *mq);
void mqtt_close(struct mqtt *mq);

#endif
