CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c99
LDFLAGS ?=

SRCS := main.c log.c config.c n32_frame.c reg_msg.c \
	at_port.c modem.c mqtt_cli.c gps_fix.c loc_msg.c http_broker.c \
	md5.c voice.c talk.c led.c trig.c bus.c admin.c online.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: hydg02

hydg02: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) hydg02 hydg02.exe
