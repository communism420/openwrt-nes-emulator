include $(TOPDIR)/rules.mk

PKG_NAME:=nes-emulator
PKG_VERSION:=1.0.0
PKG_RELEASE:=19

FCEUMM_SOURCE_DATE:=2026-07-24
FCEUMM_SOURCE_DATE_EPOCH:=1784897237
FCEUMM_COMMIT:=76f68314ce4213703174108f461c431001dcc204
FCEUMM_SHORT_COMMIT:=76f68314ce42
FCEUMM_STATE_PATCH_SHA256:=5402c5d7c4df5a3c6ea3f721aa8abfc6f6bc5242ddb69b50a75a7b07ea492c3e
FCEUMM_ROM_BUFFER_PATCH_SHA256:=8bb6a69091796b5af3c3bce2a4d4b8bb33e6751a3019370660edcabb842cbf32
PROJECT_SOURCE_DATE_EPOCH:=1787270400

PKG_SOURCE:=libretro-fceumm-$(FCEUMM_COMMIT).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/libretro/libretro-fceumm/tar.gz/$(FCEUMM_COMMIT)?
PKG_HASH:=b067ebd0a973751e9b5af56f5b54d74d0a6e67349549b392a4615d3f0d44f031
PKG_BUILD_DIR:=$(BUILD_DIR)/libretro-fceumm-$(FCEUMM_COMMIT)

PKG_MAINTAINER:=OpenWrt NES Emulator contributors
PKG_LICENSE:=GPL-2.0-only AND MIT
PKG_LICENSE_FILES:=Copying nesd-host/LICENSE-MIT
PKG_BUILD_FLAGS:=no-mips16
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/nes-emulator
  SECTION:=games
  CATEGORY:=Games
  TITLE:=NES emulator daemon with built-in FCEUmm
  URL:=https://github.com/libretro/libretro-fceumm
  USERID:=nesd:nesd
endef

define Package/nes-emulator/description
  Lightweight OpenWrt NES emulator daemon with HTTP/WebSocket streaming.
  The pinned FCEUmm libretro core is linked into nesd, so no separate core
  package or shared object is required at run time.
endef

define Package/nes-emulator/conffiles
/etc/config/nes-emulator
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	echo "$(PROJECT_SOURCE_DATE_EPOCH)" > $(PKG_BUILD_DIR)/version.date
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/nesd-host
	$(CP) ./src/Makefile ./src/*.c ./src/*.h \
		$(PKG_BUILD_DIR)/nesd-host/
	$(INSTALL_DATA) ./files/LICENSE-MIT \
		$(PKG_BUILD_DIR)/nesd-host/LICENSE-MIT
endef

define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR)/nesd-host \
		BUILD_DIR="$(PKG_BUILD_DIR)/nesd-host/.build" \
		TARGET="$(PKG_BUILD_DIR)/nesd-host/nesd" \
		FCEUMM_DIR="$(PKG_BUILD_DIR)" \
		FCEUMM_GIT_VERSION="$(FCEUMM_SHORT_COMMIT)" \
		FCEUMM_ENDIAN_CPPFLAGS="$(if $(filter y,$(CONFIG_BIG_ENDIAN)),-DMSB_FIRST)" \
		CC="$(TARGET_CC)" \
		AR="$(TARGET_AR)" \
		RANLIB="$(TARGET_RANLIB)" \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra -std=c11 \
			-ffunction-sections -fdata-sections" \
		LDFLAGS="$(TARGET_LDFLAGS) -static -Wl,--gc-sections \
			-Wl,-z,stack-size=2097152" \
		LDLIBS="-ldl -lpthread -lm -latomic"
endef

define Package/nes-emulator/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/nesd-host/nesd $(1)/usr/bin/nesd

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/nes-emulator.init $(1)/etc/init.d/nes-emulator

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/nes-emulator.config $(1)/etc/config/nes-emulator

	$(INSTALL_DIR) $(1)/etc/nes-emulator/roms
	$(INSTALL_DIR) $(1)/etc/nes-emulator/saves
	$(INSTALL_DIR) $(1)/etc/nes-emulator/system
	chmod 0750 \
		$(1)/etc/nes-emulator \
		$(1)/etc/nes-emulator/roms \
		$(1)/etc/nes-emulator/saves \
		$(1)/etc/nes-emulator/system

	$(INSTALL_DIR) $(1)/usr/share/licenses/nes-emulator
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/Copying \
		$(1)/usr/share/licenses/nes-emulator/FCEUmm-Copying
	$(INSTALL_DATA) ./files/LICENSE-MIT \
		$(1)/usr/share/licenses/nes-emulator/Host-MIT

	$(INSTALL_DIR) $(1)/usr/share/doc/nes-emulator
	( \
		echo "FCEUmm source: https://github.com/libretro/libretro-fceumm"; \
		echo "Commit: $(FCEUMM_COMMIT)"; \
		echo "Commit date: $(FCEUMM_SOURCE_DATE)"; \
		echo "Source archive SHA-256: $(PKG_HASH)"; \
		echo "Savestate parser hardening patch SHA-256: $(FCEUMM_STATE_PATCH_SHA256)"; \
		echo "Immutable ROM buffer patch SHA-256: $(FCEUMM_ROM_BUFFER_PATCH_SHA256)"; \
	) > $(1)/usr/share/doc/nes-emulator/FCEUMM-PROVENANCE
	chmod 0644 $(1)/usr/share/doc/nes-emulator/FCEUMM-PROVENANCE
endef

define Package/nes-emulator/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
[ -f /etc/config/nes-emulator ] &&
	[ ! -L /etc/config/nes-emulator ] || exit 1
META_MODE=
META_NLINK=
META_UID=
META_GID=
read_metadata() {
	local metadata
	metadata="$$(LC_ALL=C ls -ldn "$$1" 2>/dev/null)" || return 1
	case "$$metadata" in
		*'
') return 1 ;;
	esac
	set -- $$metadata
	[ "$$#" -ge 4 ] || return 1
	case "$$2:$$3:$$4" in *[!0-9:]*) return 1 ;; esac
	META_MODE="$$1"
	META_NLINK="$$2"
	META_UID="$$3"
	META_GID="$$4"
}
repair_config_metadata() {
	read_metadata /etc/config/nes-emulator || return 1
	[ "$$META_NLINK" = 1 ] || return 1
	if [ "$$META_MODE" = -rw------- ] &&
	   [ "$$META_UID" = 0 ] && [ "$$META_GID" = 0 ]; then
		return 0
	fi
	chown root:root /etc/config/nes-emulator &&
		chmod 0600 /etc/config/nes-emulator || return 1
	read_metadata /etc/config/nes-emulator &&
		[ "$$META_MODE" = -rw------- ] &&
		[ "$$META_NLINK" = 1 ] &&
		[ "$$META_UID" = 0 ] && [ "$$META_GID" = 0 ]
}
repair_directory_metadata() {
	local dir="$$1" expected_uid="$$2" expected_gid="$$3" owner_group="$$4"
	if [ -e "$$dir" ] || [ -L "$$dir" ]; then
		[ -d "$$dir" ] && [ ! -L "$$dir" ] || return 1
	else
		mkdir "$$dir" || return 1
	fi
	read_metadata "$$dir" || return 1
	if [ "$$META_MODE" = drwxr-x--- ] &&
	   [ "$$META_UID" = "$$expected_uid" ] &&
	   [ "$$META_GID" = "$$expected_gid" ]; then
		return 0
	fi
	chown "$$owner_group" "$$dir" && chmod 0750 "$$dir" || return 1
	read_metadata "$$dir" &&
		[ "$$META_MODE" = drwxr-x--- ] &&
		[ "$$META_UID" = "$$expected_uid" ] &&
		[ "$$META_GID" = "$$expected_gid" ]
}
repair_managed_rom_metadata() {
	local rom="$$1" expected_gid="$$2"
	read_metadata "$$rom" || return 0
	[ "$$META_NLINK" = 1 ] || return 0
	if [ "$$META_MODE" = -rw-r----- ] &&
	   [ "$$META_UID" = 0 ] && [ "$$META_GID" = "$$expected_gid" ]; then
		return 0
	fi
	chown root:nesd "$$rom" && chmod 0640 "$$rom" || return 1
	read_metadata "$$rom" &&
		[ "$$META_MODE" = -rw-r----- ] &&
		[ "$$META_NLINK" = 1 ] &&
		[ "$$META_UID" = 0 ] &&
		[ "$$META_GID" = "$$expected_gid" ]
}
# Stop every persistent or on-demand instance before replacing metadata or the
# executable. default_postinst starts it again only when configuration allows.
/etc/init.d/nes-emulator stop >/dev/null 2>&1 || true
nesd_uid="$$(id -u nesd 2>/dev/null)" || exit 1
nesd_gid="$$(id -g nesd 2>/dev/null)" || exit 1
case "$$nesd_uid:$$nesd_gid" in
	''|:*|*:|*[!0-9:]*) exit 1 ;;
esac
repair_config_metadata || exit 1
# One-time router-safety migration; keep mirrored in scripts/build-apks.sh.
migration="$$(uci -q get nes-emulator.main.safety_migration 2>/dev/null)"
stream_fps="$$(uci -q get nes-emulator.main.stream_fps 2>/dev/null)"
stream_format="$$(uci -q get nes-emulator.main.stream_format 2>/dev/null)"
port="$$(uci -q get nes-emulator.main.port 2>/dev/null)"
migrate_port() {
	case "$$port" in
		9090|''|*[!0-9]*)
			uci -q set nes-emulator.main.port='29876' || return 1
			;;
		*)
			[ "$$port" -ge 1 ] 2>/dev/null &&
				[ "$$port" -le 65535 ] 2>/dev/null ||
				uci -q set nes-emulator.main.port='29876' || return 1
			;;
	esac
}
migrate_extra_rom_dirs_enabled() {
	# Existing list entries are preserved, but old configurations require an
	# explicit opt-in before external storage is scanned again.
	uci -q set nes-emulator.main.extra_rom_dirs_enabled='0'
}
case "$$migration" in
4)
	;;
3)
	migrate_extra_rom_dirs_enabled || exit 1
	uci -q set nes-emulator.main.safety_migration='4' || exit 1
	uci -q commit nes-emulator || exit 1
	repair_config_metadata || exit 1
	;;
2)
	migrate_port || exit 1
	migrate_extra_rom_dirs_enabled || exit 1
	uci -q set nes-emulator.main.safety_migration='4' || exit 1
	uci -q commit nes-emulator || exit 1
	repair_config_metadata || exit 1
	;;
1)
	case "$$stream_fps" in
		1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|51|52|53|54|55|56|57|58|59|60) ;;
		*) uci -q set nes-emulator.main.stream_fps='2' || exit 1 ;;
	esac
	if [ "$$stream_format" = raw ] && [ "$$stream_fps" = 5 ]; then
		uci -q set nes-emulator.main.stream_fps='2' || exit 1
	fi
	migrate_port || exit 1
	migrate_extra_rom_dirs_enabled || exit 1
	uci -q set nes-emulator.main.safety_migration='4' || exit 1
	uci -q commit nes-emulator || exit 1
	repair_config_metadata || exit 1
	;;
''|0|*[!0-9]*)
	uci -q set nes-emulator.main.enabled='0' || exit 1
	case "$$stream_fps" in
		1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|51|52|53|54|55|56|57|58|59|60) ;;
		*) uci -q set nes-emulator.main.stream_fps='2' || exit 1 ;;
	esac
	migrate_port || exit 1
	migrate_extra_rom_dirs_enabled || exit 1
	uci -q set nes-emulator.main.safety_migration='4' || exit 1
	uci -q commit nes-emulator || exit 1
	repair_config_metadata || exit 1
	;;
*)
	# Preserve configurations written by a newer package.
	;;
esac
# End one-time router-safety migration.
repair_directory_metadata /etc/nes-emulator 0 "$$nesd_gid" root:nesd ||
	exit 1
for dir in \
	/etc/nes-emulator/roms \
	/etc/nes-emulator/saves \
	/etc/nes-emulator/system
do
	repair_directory_metadata "$$dir" "$$nesd_uid" "$$nesd_gid" \
		nesd:nesd || exit 1
done
for rom in /etc/nes-emulator/roms/*; do
	[ -e "$$rom" ] || [ -L "$$rom" ] || continue
	[ -f "$$rom" ] && [ ! -L "$$rom" ] || continue
	case "$$(printf '%s' "$${rom##*.}" |
		tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')" in
		nes|fds|unf|unif) ;;
		*) continue ;;
	esac
	repair_managed_rom_metadata "$$rom" "$$nesd_gid" || exit 1
done
exit 0
endef

$(eval $(call BuildPackage,nes-emulator))
