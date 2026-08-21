#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
PACKAGE="$ROOT/package/nes-emulator/Makefile"
BUILDER="$ROOT/scripts/build-apks.sh"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/nes-postinst-resource.XXXXXX")"
trap 'rm -rf "$TMP"' 0 1 2 15

fail() {
	printf '%s\n' "postinst resource contract: $*" >&2
	exit 1
}

native="$(
	sed -n '/^define Package\/nes-emulator\/postinst$/,/^endef$/p' "$PACKAGE" |
		sed '1d;$d;s/\$\$/\$/g'
)"
standalone="$(
	sed -n '/^cat > "\$NES_CUSTOM_POST" <<'\''EOF'\''$/,/^EOF$/p' "$BUILDER" |
		sed '1d;$d'
)"
[ "$native" = "$standalone" ] ||
	fail "OpenWrt and standalone custom post-install scripts differ"

stop_line="$(printf '%s\n' "$standalone" | grep -n -m1 '/etc/init.d/nes-emulator stop' | cut -d: -f1)"
config_line="$(printf '%s\n' "$standalone" | grep -n -m1 '^repair_config_metadata ||' | cut -d: -f1)"
rom_line="$(printf '%s\n' "$standalone" | grep -n -m1 '^for rom in ' | cut -d: -f1)"
[ "$stop_line" -lt "$config_line" ] && [ "$stop_line" -lt "$rom_line" ] ||
	fail "service is not stopped before metadata repair"

repair_source="$(
	printf '%s\n' "$standalone" |
		sed -n '/^repair_managed_rom_metadata() {$/,/^}$/p'
)"
[ -n "$repair_source" ] || fail "managed-ROM repair helper is missing"
eval "$repair_source"

ROM_LOG="$TMP/rom.log"
READ_COUNT=0
TEST_SCENARIO=correct
read_metadata() {
	READ_COUNT=$((READ_COUNT + 1))
	case "$TEST_SCENARIO:$READ_COUNT" in
	unreadable:*) return 1 ;;
	hardlink:*) META_MODE=-rw-------; META_NLINK=2; META_UID=9; META_GID=9 ;;
	correct:*) META_MODE=-rw-r-----; META_NLINK=1; META_UID=0; META_GID=77 ;;
	wrong:1) META_MODE=-rw-rw-rw-; META_NLINK=1; META_UID=9; META_GID=9 ;;
	wrong:*) META_MODE=-rw-r-----; META_NLINK=1; META_UID=0; META_GID=77 ;;
	*) return 1 ;;
	esac
}
chown() { printf 'chown:%s\n' "$*" >>"$ROM_LOG"; }
chmod() { printf 'chmod:%s\n' "$*" >>"$ROM_LOG"; }

for scenario in correct hardlink unreadable; do
	TEST_SCENARIO="$scenario"
	READ_COUNT=0
	: >"$ROM_LOG"
	repair_managed_rom_metadata /managed/game.nes 77 ||
		fail "$scenario ROM metadata was treated as a fatal repair"
	[ ! -s "$ROM_LOG" ] ||
		fail "$scenario ROM metadata triggered a mutation"
done

TEST_SCENARIO=wrong
READ_COUNT=0
: >"$ROM_LOG"
repair_managed_rom_metadata /managed/game.nes 77 ||
	fail "repairable ROM metadata was rejected"
expected='chown:root:nesd /managed/game.nes
chmod:0640 /managed/game.nes'
[ "$(cat "$ROM_LOG")" = "$expected" ] ||
	fail "repairable ROM did not receive one exact metadata repair"

printf '%s\n' "postinst resource contract: OK"
