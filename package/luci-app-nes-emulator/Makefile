include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-nes-emulator
PKG_VERSION:=1.0.0
PKG_RELEASE:=19

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=files/LICENSE-MIT
PKG_MAINTAINER:=OpenWrt NES Emulator contributors

LUCI_TITLE:=LuCI support for NES Emulator (FCEUmm)
LUCI_DEPENDS:=+luci-base +rpcd +jshn +jsonfilter +cgi-io +nes-emulator
LUCI_PKGARCH:=all

define Package/$(PKG_NAME)/description
  Web interface for the OpenWrt NES emulator daemon (FCEUmm host):
  authenticated service control, ROM import, settings, and play view.
endef

# Standalone install without luci.mk (works as a local package feed)
include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=$(LUCI_TITLE)
  DEPENDS:=$(LUCI_DEPENDS)
  EXTRA_DEPENDS:=nes-emulator (=$(PKG_VERSION)-r$(PKG_RELEASE))
  PKGARCH:=$(LUCI_PKGARCH)
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/*.json $(1)/usr/share/luci/menu.d/

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/*.json $(1)/usr/share/rpcd/acl.d/

	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/nes-emulator
	$(INSTALL_DATA) ./htdocs/luci-static/resources/nes-emulator.js \
		$(1)/www/luci-static/resources/nes-emulator.js
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/nes-emulator/*.js \
		$(1)/www/luci-static/resources/view/nes-emulator/

	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./root/usr/libexec/rpcd/nes-emulator $(1)/usr/libexec/rpcd/nes-emulator

	$(INSTALL_DIR) $(1)/usr/share/licenses/luci-app-nes-emulator
	$(INSTALL_DATA) ./files/LICENSE-MIT \
		$(1)/usr/share/licenses/luci-app-nes-emulator/MIT

endef

define Package/$(PKG_NAME)/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache 2>/dev/null
rm -rf /tmp/luci-modulecache 2>/dev/null
/etc/init.d/rpcd reload >/dev/null 2>&1 || true
exit 0
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
