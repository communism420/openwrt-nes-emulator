include $(TOPDIR)/rules.mk

PKG_NAME:=libretro-fceumm
PKG_VERSION:=1.0.0
PKG_RELEASE:=19

PKG_MAINTAINER:=OpenWrt NES Emulator contributors
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/libretro-fceumm
  SECTION:=games
  CATEGORY:=Games
  TITLE:=FCEUmm transition package
  URL:=https://github.com/libretro/libretro-fceumm
  DEPENDS:=+nes-emulator
  PKGARCH:=all
  HIDDEN:=1
endef

define Package/libretro-fceumm/description
  Compatibility package for upgrades from the former standalone FCEUmm
  shared core. FCEUmm is now statically linked into nes-emulator.
endef

define Build/Compile
endef

define Package/libretro-fceumm/install
	$(INSTALL_DIR) $(1)/usr/share/doc/libretro-fceumm
	( \
		echo "Transitional package only."; \
		echo "FCEUmm is built into /usr/bin/nesd by nes-emulator."; \
	) > $(1)/usr/share/doc/libretro-fceumm/TRANSITION
	chmod 0644 $(1)/usr/share/doc/libretro-fceumm/TRANSITION
endef

$(eval $(call BuildPackage,libretro-fceumm))
