include $(TOPDIR)/rules.mk

PKG_NAME:=nes-emulator
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

# r19 is the upstream project's source-release identifier.  OpenWrt package
# revisions start at 1 for a new packages-feed entry.
PROJECT_SOURCE_RELEASE:=$(PKG_VERSION)-r19
PROJECT_SOURCE_DATE_EPOCH:=1787270400
FCEUMM_SHORT_COMMIT:=76f68314ce42
PROJECT_URL:=https://github.com/communism420/openwrt-nes-emulator

PKG_SOURCE:=openwrt-nes-emulator-$(PROJECT_SOURCE_RELEASE)-source.tar.gz
PKG_SOURCE_URL:=$(PROJECT_URL)/releases/download/v$(PROJECT_SOURCE_RELEASE)
PKG_HASH:=e3ec5c28cdc1bfd7f600854088add79f47c19d2c7c318b89d9c2153ac3541b3c
PKG_BUILD_DIR:=$(BUILD_DIR)/openwrt-nes-emulator-$(PROJECT_SOURCE_RELEASE)-source

PKG_LICENSE:=MIT GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE third_party/libretro-fceumm/Copying
PKG_BUILD_FLAGS:=no-mips16 gc-sections
PKG_BUILD_PARALLEL:=1
PKG_ASLR_PIE_REGULAR:=1

include $(INCLUDE_DIR)/package.mk

NESD_SOURCE_DIR:=$(PKG_BUILD_DIR)/package/nes-emulator/src
NESD_FILES_DIR:=$(PKG_BUILD_DIR)/package/nes-emulator/files
FCEUMM_DIR:=$(PKG_BUILD_DIR)/third_party/libretro-fceumm

define Package/nes-emulator
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=NES emulator service with built-in FCEUmm
  URL:=$(PROJECT_URL)
  DEPENDS:=+libatomic +libpthread
  USERID:=nesd:nesd
endef

define Package/nes-emulator/description
  Lightweight NES emulator service for OpenWrt with HTTP and WebSocket
  streaming. The pinned FCEUmm libretro core is linked into nesd, so no
  separate emulator-core package or shared object is required at run time.
endef

define Package/nes-emulator/conffiles
/etc/config/nes-emulator
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	echo "$(PROJECT_SOURCE_DATE_EPOCH)" > $(PKG_BUILD_DIR)/version.date
endef

define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(NESD_SOURCE_DIR) \
		BUILD_DIR="$(PKG_BUILD_DIR)/.nesd-build" \
		TARGET="$(PKG_BUILD_DIR)/nesd" \
		FCEUMM_DIR="$(FCEUMM_DIR)" \
		FCEUMM_GIT_VERSION="$(FCEUMM_SHORT_COMMIT)" \
		FCEUMM_ENDIAN_CPPFLAGS="$(if $(filter y,$(CONFIG_BIG_ENDIAN)),-DMSB_FIRST)" \
		CC="$(TARGET_CC)" \
		AR="$(TARGET_AR)" \
		RANLIB="$(TARGET_RANLIB)" \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		CFLAGS="$(TARGET_CFLAGS) -std=c11" \
		LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,stack-size=2097152" \
		LDLIBS="-ldl -lpthread -lm -latomic"
endef

define Package/nes-emulator/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/nesd $(1)/usr/bin/nesd

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(NESD_FILES_DIR)/nes-emulator.init \
		$(1)/etc/init.d/nes-emulator

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(NESD_FILES_DIR)/nes-emulator.config \
		$(1)/etc/config/nes-emulator

	$(INSTALL_DIR) $(1)/etc/nes-emulator/roms
	$(INSTALL_DIR) $(1)/etc/nes-emulator/saves
	$(INSTALL_DIR) $(1)/etc/nes-emulator/system
	chmod 0750 \
		$(1)/etc/nes-emulator \
		$(1)/etc/nes-emulator/roms \
		$(1)/etc/nes-emulator/saves \
		$(1)/etc/nes-emulator/system
endef

define Package/nes-emulator/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT:-}" ] && exit 0
[ -n "$${PKG_INSTROOT:-}" ] && exit 0
NESD_ON_DEMAND=1 NESD_SKIP_AUTOLOAD=1 \
	/etc/init.d/nes-emulator preflight
endef

$(eval $(call BuildPackage,nes-emulator))
