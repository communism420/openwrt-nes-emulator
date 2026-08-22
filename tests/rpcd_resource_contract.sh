#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
RPCD="$ROOT/package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/nes-rpcd-resource.XXXXXX")"
trap 'rm -rf "$TMP"' 0 1 2 15

extract_function() {
	sed -n "/^$1() {$/,/^}$/p" "$RPCD"
}

fail() {
	printf '%s\n' "rpcd resource contract: $*" >&2
	exit 1
}

# OpenWrt ships BusyBox wc/mv, not GNU du/mv. Exercise the exact byte-count
# helper and keep unsupported options out of every runtime branch.
eval "$(extract_function file_size_bytes)"
size_fixture="$TMP/ROM image.nes"
printf 'NES\032012345678901' >"$size_fixture"
[ "$(file_size_bytes "$size_fixture")" = 16 ] ||
	fail "apparent file size was not measured with wc -c"
file_size_bytes "$TMP" >/dev/null 2>&1 &&
	fail "directory was accepted as a regular byte-count source"
if grep -Eq 'du[[:space:]]+-b|mv[[:space:]]+-fT' "$RPCD"; then
	fail "GNU-only du or mv option remains in the rpcd bridge"
fi
grep -Fq -- "-exec wc -c '{}' ';'" "$RPCD" ||
	fail "quota scan does not use the BusyBox-compatible wc applet"
grep -Fq 'mv -f "$tmp" "$dest"' "$RPCD" ||
	fail "ROM commit does not use the BusyBox-compatible mv form"

# Exercise the exact metadata parser and validators on real regular files,
# hardlinks, symlinks and directories.
eval "$(extract_function read_metadata)"
eval "$(extract_function validate_regular_metadata)"
eval "$(extract_function validate_directory_metadata)"

regular="$TMP/regular"
hardlink="$TMP/hardlink"
symlink="$TMP/symlink"
directory="$TMP/directory"
: >"$regular"
chmod 0600 "$regular"
mkdir "$directory"
chmod 0700 "$directory"
uid="$(id -u)"
gid="$(id -g)"

validate_regular_metadata "$regular" -rw------- "$uid" "$gid" ||
	fail "safe regular file was rejected"
validate_regular_metadata "$regular" -rw-r----- "$uid" "$gid" &&
	fail "wrong regular-file mode was accepted"
ln "$regular" "$hardlink"
validate_regular_metadata "$regular" -rw------- "$uid" "$gid" &&
	fail "multiply-linked token was accepted"
ln -s "$regular" "$symlink"
validate_regular_metadata "$symlink" -rw------- "$uid" "$gid" &&
	fail "symlink token was accepted"
validate_directory_metadata "$directory" drwx------ "$uid" "$gid" ||
	fail "safe upload directory was rejected"
validate_directory_metadata "$directory" drwxr-x--- "$uid" "$gid" &&
	fail "wrong directory mode was accepted"
validate_directory_metadata "$symlink" drwx------ "$uid" "$gid" &&
	fail "symlink was accepted as a directory"

validators="$(
	extract_function validate_regular_metadata
	extract_function validate_directory_metadata
)"
case "$validators" in
	*chown*|*chmod*) fail "read-only validators mutate metadata" ;;
esac

# `ensure_auth_token` may create a genuinely absent token, but must fail closed
# for every existing entry whose content or metadata was rejected.
ensure_source="$(extract_function ensure_auth_token)"
eval "$ensure_source"
TOKEN_FILE="$TMP/auth.token"
AUTH_TOKEN=
TOKEN_LOG="$TMP/token.log"

validate_token_parent() { return 0; }
acquire_token_lock() { return 0; }
release_token_lock() { :; }
remove_legacy_token() { printf '%s\n' remove >>"$TOKEN_LOG"; }
new_token() {
	printf '%064d' 0
	printf '%s\n' generate >>"$TOKEN_LOG"
}
write_auth_token() {
	printf 'write:%s\n' "$1" >>"$TOKEN_LOG"
	printf '%s\n' "$1" >"$TOKEN_FILE"
}
read_auth_token() {
	[ "${READ_TOKEN_OK:-0}" -eq 1 ] || return 1
	AUTH_TOKEN=abcdefghijklmnopqrstuvwxyzABCDEF
}

READ_TOKEN_OK=1
: >"$TOKEN_LOG"
ensure_auth_token || fail "valid existing token was rejected"
[ ! -s "$TOKEN_LOG" ] || fail "valid token triggered write-time churn"

READ_TOKEN_OK=0
rm -f "$TOKEN_FILE"
: >"$TOKEN_LOG"
ensure_auth_token || fail "missing token was not regenerated"
grep -q '^write:' "$TOKEN_LOG" || fail "missing token was not written"
grep -q '^remove$' "$TOKEN_LOG" || fail "legacy token cleanup was skipped"

printf '%s\n' invalid >"$TOKEN_FILE"
: >"$TOKEN_LOG"
ensure_auth_token && fail "invalid existing token was silently replaced"
[ ! -s "$TOKEN_LOG" ] || fail "invalid existing token triggered mutation"

rm -f "$TOKEN_FILE"
ln -s "$TMP/missing-target" "$TOKEN_FILE"
: >"$TOKEN_LOG"
ensure_auth_token && fail "dangling token symlink was silently replaced"
[ ! -s "$TOKEN_LOG" ] || fail "token symlink triggered mutation"

rm -f "$TOKEN_FILE"
printf '%s\n' invalid >"$TOKEN_FILE"
ln "$TOKEN_FILE" "$TMP/auth-token-hardlink"
: >"$TOKEN_LOG"
ensure_auth_token && fail "hardlinked token was silently replaced"
[ ! -s "$TOKEN_LOG" ] || fail "hardlinked token triggered mutation"

# Execute the exact rotate flow with only the absolute init-script path replaced
# by a test double. This verifies persistent, on-demand and stopped states.
rotate_source="$(
	extract_function rotate_token |
		sed 's#/etc/init.d/nes-emulator#service_cmd#g'
)"
eval "$rotate_source"
SERVICE_LOG="$TMP/service.log"
SERVICE_RUNNING=0
TEST_ENABLED=0

json_error() { return 1; }
json_init() { :; }
json_add_boolean() { :; }
json_add_string() { :; }
json_dump_without_upload_fd() { :; }
write_auth_token() { return 0; }
remove_legacy_token() { return 0; }
uci() {
	[ "$*" = "-q get nes-emulator.main.enabled" ] || return 1
	printf '%s\n' "$TEST_ENABLED"
}
service_cmd() {
	case "$1" in
	running)
		[ "$SERVICE_RUNNING" -eq 1 ]
		;;
	restart)
		printf 'restart:on_demand=%s:skip_autoload=%s\n' \
			"${NESD_ON_DEMAND:-0}" "${NESD_SKIP_AUTOLOAD:-0}" \
			>>"$SERVICE_LOG"
		;;
	*) return 1 ;;
	esac
}

: >"$SERVICE_LOG"
SERVICE_RUNNING=0
TEST_ENABLED=0
rotate_token || fail "token rotation failed while daemon was stopped"
[ ! -s "$SERVICE_LOG" ] ||
	fail "stopped daemon was started by token rotation"

: >"$SERVICE_LOG"
SERVICE_RUNNING=1
TEST_ENABLED=1
rotate_token || fail "persistent daemon token rotation failed"
grep -qx 'restart:on_demand=0:skip_autoload=0' "$SERVICE_LOG" ||
	fail "persistent daemon restart received on-demand overrides"

: >"$SERVICE_LOG"
SERVICE_RUNNING=1
TEST_ENABLED=0
rotate_token || fail "on-demand daemon token rotation failed"
grep -qx 'restart:on_demand=1:skip_autoload=1' "$SERVICE_LOG" ||
	fail "on-demand daemon was not restarted in transient mode"

printf '%s\n' "rpcd resource contract: OK"
