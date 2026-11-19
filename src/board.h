#ifndef BOARD_H
#define BOARD_H

/*
 * 1V3 上挂在 MT 这一侧、基础功能会用到的节点。
 * mcu3 是屏控 N32G452 的 USART3，不是 N32S032。
 * rs485_6 的方向由驱动把 CH9437 的 3 脚当成 GPIO5 来扳。
 * 功放 12V 由模组 GPIO0 打开，设备树里还没有导出这只脚。
 */

#define DEV_N32       "/dev/mcu3"
#define N32_BAUD      115200u

#define DEV_RS485     "/dev/rs485"
#define DEV_RS485_6   "/dev/rs485_6"
#define RS485_BAUD    9600u

#define DEV_GPS       "/dev/gps"
#define GPS_BAUD      9600u

#define DEV_AT        "/dev/ttyUSB2"
#define NET_IF        "usb0"

#define AMP_GPIO      0

#endif
