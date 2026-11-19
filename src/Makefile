CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c99
LDFLAGS ?=

SRCS := main.c log.c config.c n32_frame.c reg_msg.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: hydg02

hydg02: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) hydg02 hydg02.exe
