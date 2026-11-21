# hydg02 Makefile - incremental build engine
#
# Automatically discovers .c files in app/, sal/, pal/, subsystems/*/
# Header dependency tracking via -MMD -MP (compiler-generated .d files)
#
# Targets:
#   hydg02     - link binary (default)
#   clean      - remove all .o/.d/binary
#   install    - build + scp to board
#   run        - build + scp + selftest on board
#
# Usage: ./build.sh [command]  (or make directly)

# === Toolchain (auto-detected by build.sh, can override) ===
CROSS   ?= $(TOOLCHAIN_BIN)/mipsel-openwrt-linux-musl-
CC       = $(CROSS)gcc

# === Board target ===
BOARD_IP ?= 192.168.50.1

# === Source discovery (add new dirs here as subsystems grow) ===
SRC_DIRS  = app/ sal/ pal/ net/
SRC_DIRS += $(wildcard subsystems/*/)

ALL_SRC   = $(foreach d,$(SRC_DIRS),$(wildcard $(d)*.c))
ALL_OBJ   = $(ALL_SRC:.c=.o)
ALL_DEPS  = $(ALL_OBJ:.o=.d)

# === Flags ===
CFLAGS  = -Wall -std=gnu99 \
           -Iinclude \
           -D_DEFAULT_SOURCE -D_BSD_SOURCE \
           -MMD -MP

# === mbedtls (TLS: MQTT 8883 / HTTPS 下载, 协议 V3.0) ===
# OpenWrt staging 静态库 —— 独立部署零固件依赖 (旧固件也能跑)
MBEDTLS_DIR ?= $(HOME)/source_code/mt7628dan_project/openwrt/staging_dir/target-mipsel_24kc_musl/usr
CFLAGS += -I$(MBEDTLS_DIR)/include

# initcall.lds: initcall/pumpcall 段收集 (内核风格分层初始化)
# DYNAMIC_LINK=1: OpenWrt 集成用动态链接 (固件已有 libmbedtls.so, 省 ~700KB)
# 默认 (-static): 独立部署零固件依赖
ifdef DYNAMIC_LINK
LDFLAGS = -Wl,-T,$(CURDIR)/initcall.lds
LDLIBS  = -lmbedtls -lmbedx509 -lmbedcrypto
else
LDFLAGS = -static -Wl,-T,$(CURDIR)/initcall.lds
LDLIBS  = -L$(MBEDTLS_DIR)/lib -lmbedtls -lmbedx509 -lmbedcrypto
endif

# === Targets ===

.DEFAULT_GOAL := hydg02

hydg02: $(ALL_OBJ)
	@echo "  LINK    hydg02"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "  CLEAN"
	@rm -f $(ALL_OBJ) $(ALL_DEPS) hydg02
	@find . -name "*.o" -delete 2>/dev/null || true
	@find . -name "*.d" -delete 2>/dev/null || true
	@echo "  Done."

install: hydg02
	@echo "  SCP     -> $(BOARD_IP):/tmp/"
	@scp -q hydg02 root@$(BOARD_IP):/tmp/
	@echo "  Deployed to $(BOARD_IP):/tmp/hydg02"

run: install
	@echo "  RUN     selftest on $(BOARD_IP)"
	@ssh root@$(BOARD_IP) "/tmp/hydg02 --selftest"

# === Include compiler-generated dependencies ===
-include $(ALL_DEPS)

.PHONY: clean install run
