include $(TOPDIR)/rules.mk

PKG_NAME:=hydg02
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/hydg02
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=HY-DG02 checkpoint application
endef

define Package/hydg02/description
  HY-DG02 主控应用程序。4G 注册、定位、语音、对讲、LED、雷达和 RS485。
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra -std=c99" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/hydg02/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hydg02 $(1)/usr/sbin/hydg02
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/hydg02.init $(1)/etc/init.d/hydg02
	$(INSTALL_DIR) $(1)/etc/hydg02
	$(INSTALL_DATA) ./files/hydg02.conf $(1)/etc/hydg02/hydg02.conf
endef

$(eval $(call BuildPackage,hydg02))
