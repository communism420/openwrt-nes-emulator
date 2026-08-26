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

# All runtime locks belong to one root-only tmpfs directory. Exercise the exact
# safe-creation helpers with the current test user substituted for root when
# this contract runs on an unprivileged CI worker.
grep -Fxq 'LOCK_DIR=/var/run/nes-emulator' "$RPCD" ||
	fail "runtime locks do not use the protected /var/run directory"
grep -Fxq 'TOKEN_LOCK_FILE="$LOCK_DIR/auth.token.lock"' "$RPCD" ||
	fail "token lock is outside the protected runtime directory"
grep -Fxq 'UPLOAD_LOCK_FILE="$LOCK_DIR/upload.lock"' "$RPCD" ||
	fail "upload lock is outside the protected runtime directory"
grep -Fxq 'START_LOCK_FILE="$LOCK_DIR/start.lock"' "$RPCD" ||
	fail "startup lock is outside the protected runtime directory"
if grep -Eq '^(TOKEN|UPLOAD|START)_LOCK_FILE=(/etc|/var/lock)' "$RPCD"; then
	fail "a runtime lock remains on flash or in the world-writable lock directory"
fi

lock_directory_source="$(extract_function prepare_lock_directory)"
lock_file_source="$(extract_function prepare_lock_file)"
[ "$(printf '%s\n' "$lock_directory_source" |
	grep -Fc 'validate_directory_metadata "$LOCK_DIR" drwx------ 0 0')" -ge 2 ] ||
	fail "lock directory is not validated after safe creation"
case "$lock_directory_source" in
	*'umask 077'*'mkdir "$LOCK_DIR"'*) ;;
	*) fail "lock directory is not created with private permissions" ;;
esac
[ "$(printf '%s\n' "$lock_file_source" |
	grep -Fc 'validate_regular_metadata "$path" -rw------- 0 0')" -ge 3 ] ||
	fail "lock file is not validated before and after safe creation"
case "$lock_file_source" in
	*'set -C'*'umask 077'*': >"$path"'*) ;;
	*) fail "lock file is not created atomically with private permissions" ;;
esac

test_lock_source="$lock_directory_source
$lock_file_source"
if [ "$uid" != 0 ] || [ "$gid" != 0 ]; then
	test_lock_source="$(printf '%s\n' "$test_lock_source" |
		sed "s/drwx------ 0 0/drwx------ $uid $gid/g;
		     s/-rw------- 0 0/-rw------- $uid $gid/g")"
fi
eval "$test_lock_source"
LOCK_DIR="$TMP/locks"
TOKEN_LOCK_FILE="$LOCK_DIR/auth.token.lock"
UPLOAD_LOCK_FILE="$LOCK_DIR/upload.lock"
START_LOCK_FILE="$LOCK_DIR/start.lock"

prepare_lock_directory || fail "protected lock directory was not created"
validate_directory_metadata "$LOCK_DIR" drwx------ "$uid" "$gid" ||
	fail "protected lock directory has unsafe metadata"
for lock_file in "$TOKEN_LOCK_FILE" "$UPLOAD_LOCK_FILE" "$START_LOCK_FILE"; do
	prepare_lock_file "$lock_file" || fail "safe lock file was not created"
	validate_regular_metadata "$lock_file" -rw------- "$uid" "$gid" ||
		fail "created lock file has unsafe metadata"
	prepare_lock_file "$lock_file" || fail "safe existing lock file was rejected"
done

chmod 0755 "$LOCK_DIR"
prepare_lock_directory &&
	fail "world-searchable lock directory was accepted"
chmod 0700 "$LOCK_DIR"
prepare_lock_directory || fail "safe lock directory was not restored"

real_lock_dir="$TMP/real-locks"
mv "$LOCK_DIR" "$real_lock_dir"
ln -s "$real_lock_dir" "$LOCK_DIR"
prepare_lock_directory &&
	fail "lock-directory symlink was accepted"
rm -f "$LOCK_DIR"
mv "$real_lock_dir" "$LOCK_DIR"
prepare_lock_directory || fail "safe lock directory was not restored after symlink test"

chmod 0644 "$TOKEN_LOCK_FILE"
prepare_lock_file "$TOKEN_LOCK_FILE" &&
	fail "world-readable lock file was accepted"
rm -f "$TOKEN_LOCK_FILE"
ln -s "$TMP/regular" "$TOKEN_LOCK_FILE"
prepare_lock_file "$TOKEN_LOCK_FILE" &&
	fail "lock-file symlink was accepted"
rm -f "$TOKEN_LOCK_FILE"
prepare_lock_file "$TOKEN_LOCK_FILE" || fail "safe token lock was not restored"
ln "$TOKEN_LOCK_FILE" "$TMP/token-lock-hardlink"
prepare_lock_file "$TOKEN_LOCK_FILE" &&
	fail "multiply-linked lock file was accepted"
rm -f "$TMP/token-lock-hardlink" "$TOKEN_LOCK_FILE"

# Opening any lock must be catchable under BusyBox ash and followed by another
# metadata validation. A bare special-builtin `exec` would terminate each
# subshell before it can create its marker.
token_lock_source="$(extract_function acquire_token_lock)"
upload_lock_source="$(extract_function acquire_upload_lock)"
start_lock_source="$(extract_function acquire_start_lock)"
case "$token_lock_source" in
	*'prepare_lock_file "$TOKEN_LOCK_FILE"'*'command exec 8<>"$TOKEN_LOCK_FILE"'*'validate_regular_metadata "$TOKEN_LOCK_FILE" -rw------- 0 0'*) ;;
	*) fail "token lock lacks safe preparation, catchable open, or post-open validation" ;;
esac
case "$token_lock_source" in
	*'sleep 1 9>&- 8>&- 7>&-'*) ;;
	*) fail "token-lock wait lets a sleep child inherit a lock descriptor" ;;
esac
case "$upload_lock_source" in
	*'prepare_lock_file "$UPLOAD_LOCK_FILE"'*'command exec 9<>"$UPLOAD_LOCK_FILE"'*'validate_regular_metadata "$UPLOAD_LOCK_FILE" -rw------- 0 0'*) ;;
	*) fail "upload lock lacks safe preparation, catchable open, or post-open validation" ;;
esac
case "$start_lock_source" in
	*'prepare_lock_file "$START_LOCK_FILE"'*'command exec 7<>"$START_LOCK_FILE"'*'validate_regular_metadata "$START_LOCK_FILE" -rw------- 0 0'*) ;;
	*) fail "startup lock lacks safe preparation, catchable open, or post-open validation" ;;
esac

token_open_caught="$TMP/token-open-caught"
(
	eval "$token_lock_source"
	TOKEN_LOCK_HELD=0
	TOKEN_LOCK_MAX_ATTEMPTS=1
	TOKEN_LOCK_FILE="$TMP/missing/token.lock"
	prepare_lock_file() { return 0; }
	validate_regular_metadata() { return 0; }
	acquire_token_lock && exit 1
	: >"$token_open_caught"
) >/dev/null 2>&1 || :
[ -e "$token_open_caught" ] ||
	fail "token-lock open failure terminated the shell"

upload_open_caught="$TMP/upload-open-caught"
(
	eval "$upload_lock_source"
	UPLOAD_LOCK_HELD=0
	UPLOAD_LOCK_MAX_ATTEMPTS=1
	UPLOAD_LOCK_FILE="$TMP/missing/upload.lock"
	prepare_lock_file() { return 0; }
	validate_regular_metadata() { return 0; }
	acquire_upload_lock && exit 1
	: >"$upload_open_caught"
) >/dev/null 2>&1 || :
[ -e "$upload_open_caught" ] ||
	fail "upload-lock open failure terminated the shell"

start_open_caught="$TMP/start-open-caught"
(
	eval "$start_lock_source"
	START_LOCK_HELD=0
	START_LOCK_MAX_ATTEMPTS=1
	START_LOCK_FILE="$TMP/missing/start.lock"
	prepare_lock_file() { return 0; }
	validate_regular_metadata() { return 0; }
	acquire_start_lock && exit 1
	: >"$start_open_caught"
) >/dev/null 2>&1 || :
[ -e "$start_open_caught" ] ||
	fail "startup-lock open failure terminated the shell"

# A rotate request selects a shorter token-lock budget before the global token
# check, then encounters that lock once there and once inside rotate_token().
# Ordinary requests must retain the less aggressive default budget.
token_lock_attempts="$(sed -n 's/^TOKEN_LOCK_MAX_ATTEMPTS=//p' "$RPCD")"
rotate_token_attempts="$(sed -n 's/^ROTATE_TOKEN_LOCK_MAX_ATTEMPTS=//p' "$RPCD")"
rotate_start_attempts="$(sed -n 's/^ROTATE_START_LOCK_MAX_ATTEMPTS=//p' "$RPCD")"
for lock_attempts in \
	"$token_lock_attempts" \
	"$rotate_token_attempts" \
	"$rotate_start_attempts"; do
	case "$lock_attempts" in
	''|*[!0-9]*|0) fail "token lock budget is missing or invalid" ;;
	esac
done
[ "$token_lock_attempts" -eq 10 ] ||
	fail "ordinary requests lost the standard token-lock budget"
[ "$rotate_token_attempts" -lt "$token_lock_attempts" ] ||
	fail "token rotation does not use a shorter token-lock budget"

token_budget_selection="$(
	sed -n '/^case "$1:$2" in$/,/^esac$/p' "$RPCD"
)"
case "$token_budget_selection" in
	*'call:rotate_token) TOKEN_LOCK_MAX_ATTEMPTS="$ROTATE_TOKEN_LOCK_MAX_ATTEMPTS" ;;'*) ;;
	*) fail "rotation-specific token-lock budget selection is missing" ;;
esac
pre_dispatch_source="$(sed -n '/^load_config$/,/^ensure_auth_token || {/p' "$RPCD")"
case "$pre_dispatch_source" in
	*'call:rotate_token) TOKEN_LOCK_MAX_ATTEMPTS="$ROTATE_TOKEN_LOCK_MAX_ATTEMPTS" ;;'*'ensure_auth_token || {'*) ;;
	*) fail "rotation token-lock budget is not selected before token initialization" ;;
esac

(
	TOKEN_LOCK_MAX_ATTEMPTS="$token_lock_attempts"
	ROTATE_TOKEN_LOCK_MAX_ATTEMPTS="$rotate_token_attempts"
	set -- call rotate_token
	eval "$token_budget_selection"
	[ "$TOKEN_LOCK_MAX_ATTEMPTS" -eq "$rotate_token_attempts" ]
) || fail "call:rotate_token did not select its per-request token-lock budget"
(
	TOKEN_LOCK_MAX_ATTEMPTS="$token_lock_attempts"
	ROTATE_TOKEN_LOCK_MAX_ATTEMPTS="$rotate_token_attempts"
	set -- call status
	eval "$token_budget_selection"
	[ "$TOKEN_LOCK_MAX_ATTEMPTS" -eq "$token_lock_attempts" ]
) || fail "ordinary call unexpectedly inherited the rotation token-lock budget"
(
	TOKEN_LOCK_MAX_ATTEMPTS="$token_lock_attempts"
	ROTATE_TOKEN_LOCK_MAX_ATTEMPTS="$rotate_token_attempts"
	set -- list unused
	eval "$token_budget_selection"
	[ "$TOKEN_LOCK_MAX_ATTEMPTS" -eq "$token_lock_attempts" ]
) || fail "rpcd method listing unexpectedly inherited the rotation budget"

# The RPCD-side rotation phase has two token waits plus one startup-lock wait.
# A native restart may add its separately bounded nine-sleep token wait; LuCI
# therefore gives the complete operation an explicit extended client timeout.
combined_rotate_sleeps=$((
	(rotate_token_attempts - 1) * 2 + rotate_start_attempts - 1
))
[ "$combined_rotate_sleeps" -eq 7 ] ||
	fail "RPCD rotation locks no longer preserve the reviewed seven-sleep budget"

# External helpers must not retain any of the three request locks in their
# command-substitution or jshn children.
grep -Fq "fd 8 the token flock" "$RPCD" ||
	fail "lock descriptor inventory omits the token flock"
for helper in json_load_without_upload_fd read_metadata load_service_nesd_gid; do
	extract_function "$helper" | grep -Fq 'exec 7>&- 8>&- 9>&-' ||
		fail "$helper does not close all lock descriptors in its child"
done
extract_function json_dump_without_upload_fd |
	grep -Fq '7>&- 8>&- 9>&-' ||
	fail "json_dump_without_upload_fd does not close all lock descriptors"

new_token_source="$(extract_function new_token)"
validate_token_parent_source="$(extract_function validate_token_parent)"
read_auth_token_source="$(extract_function read_auth_token)"
write_auth_token_source="$(extract_function write_auth_token)"
remove_legacy_token_source="$(extract_function remove_legacy_token)"
printf '%s\n' "$new_token_source" | grep -Fq 'exec 7>&- 8>&- 9>&-' ||
	fail "new_token lets hexdump inherit request locks"
printf '%s\n' "$validate_token_parent_source" |
	grep -Fq 'exec 7>&- 8>&- 9>&-' ||
	fail "validate_token_parent lets readlink inherit request locks"
[ "$(printf '%s\n' "$read_auth_token_source" |
	grep -Fc 'exec 7>&- 8>&- 9>&-')" -ge 3 ] ||
	fail "read_auth_token lets a cat/wc/tr child inherit request locks"
printf '%s\n' "$write_auth_token_source" |
	grep -Fq 'exec 7>&- 8>&- 9>&-' ||
	fail "write_auth_token lets mktemp inherit request locks"
for child in chown chmod mv rm; do
	printf '%s\n' "$write_auth_token_source" |
		grep -Eq "[[:space:]]$child .*7>&- 8>&- 9>&-" ||
		fail "write_auth_token lets $child inherit request locks"
done
printf '%s\n' "$write_auth_token_source" |
	grep -Fq 'chmod 0640 "$temporary" 2>/dev/null 7>&- 8>&- 9>&-' ||
	fail "write_auth_token exposes an expected chmod failure on rpcd stderr"
[ "$(printf '%s\n' "$remove_legacy_token_source" |
	grep -Fc '7>&- 8>&- 9>&-')" -ge 3 ] ||
	fail "remove_legacy_token lets a uci child inherit request locks"
ensure_auth_token_source="$(extract_function ensure_auth_token)"
rotate_token_source="$(extract_function rotate_token)"
for child_source in "$ensure_auth_token_source" "$rotate_token_source"; do
	printf '%s\n' "$child_source" | awk '
		/token="\$\($/ { state = 1; next }
		state == 1 && /exec 7>&- 8>&- 9>&-/ { state = 2; next }
		state == 2 && /new_token/ { state = 3; next }
		state == 3 && /^[[:space:]]*\)"$/ { found = 1 }
		END { exit !found }
	' || fail "a new_token command-substitution shell inherits request locks"
done
printf '%s\n' "$rotate_token_source" |
	grep -Fq 'uci -q get nes-emulator.main.enabled 2>/dev/null' ||
	fail "rotate_token no longer checks the enabled state"
printf '%s\n' "$rotate_token_source" |
	grep -Fq 'exec 7>&- 8>&- 9>&-' ||
	fail "rotate_token lets its enabled-state lookup inherit the startup lock"

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
ROTATE_START_LOCK_MAX_ATTEMPTS="$rotate_start_attempts"
SERVICE_LOG="$TMP/service.log"
SERVICE_RUNNING=0
TEST_ENABLED=0
START_LOCK_ACQUIRES=0
START_LOCK_RELEASES=0
START_LOCK_BUDGET=
RUNNING_CHECKS=0
START_DURING_ROTATION=0

json_error() { return 1; }
json_init() { :; }
json_add_boolean() { :; }
json_add_string() { :; }
json_dump_without_upload_fd() { :; }
write_auth_token() { return 0; }
remove_legacy_token() { return 0; }
acquire_start_lock() {
	[ "$#" -eq 1 ] || fail "rotation did not supply a bounded startup-lock wait"
	START_LOCK_BUDGET="$1"
	START_LOCK_ACQUIRES=$((START_LOCK_ACQUIRES + 1))
	return 0
}
release_start_lock() {
	START_LOCK_RELEASES=$((START_LOCK_RELEASES + 1))
}
uci() {
	[ "$*" = "-q get nes-emulator.main.enabled" ] || return 1
	printf '%s\n' "$TEST_ENABLED"
}
service_cmd() {
	case "$1" in
	running)
		[ "$START_LOCK_ACQUIRES" -gt "$START_LOCK_RELEASES" ] ||
			fail "daemon state was sampled outside the startup lock"
		RUNNING_CHECKS=$((RUNNING_CHECKS + 1))
		if [ "$START_DURING_ROTATION" -eq 1 ] &&
		   [ "$RUNNING_CHECKS" -ge 2 ]; then
			return 0
		fi
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
RUNNING_CHECKS=0
rotate_token || fail "token rotation failed while daemon was stopped"
[ "$START_LOCK_BUDGET" = "$ROTATE_START_LOCK_MAX_ATTEMPTS" ] ||
	fail "rotation did not use its dedicated startup-lock budget"
[ ! -s "$SERVICE_LOG" ] ||
	fail "stopped daemon was started by token rotation"
[ "$START_LOCK_ACQUIRES" -eq "$START_LOCK_RELEASES" ] ||
	fail "stopped-daemon token rotation leaked the startup lock"

: >"$SERVICE_LOG"
SERVICE_RUNNING=1
TEST_ENABLED=1
RUNNING_CHECKS=0
rotate_token || fail "persistent daemon token rotation failed"
grep -qx 'restart:on_demand=0:skip_autoload=0' "$SERVICE_LOG" ||
	fail "persistent daemon restart received on-demand overrides"
[ "$START_LOCK_ACQUIRES" -eq "$START_LOCK_RELEASES" ] ||
	fail "persistent token rotation leaked the startup lock"

: >"$SERVICE_LOG"
SERVICE_RUNNING=1
TEST_ENABLED=0
RUNNING_CHECKS=0
rotate_token || fail "on-demand daemon token rotation failed"
grep -qx 'restart:on_demand=1:skip_autoload=1' "$SERVICE_LOG" ||
	fail "on-demand daemon was not restarted in transient mode"
[ "$START_LOCK_ACQUIRES" -eq "$START_LOCK_RELEASES" ] ||
	fail "on-demand token rotation leaked the startup lock"

: >"$SERVICE_LOG"
SERVICE_RUNNING=0
TEST_ENABLED=0
RUNNING_CHECKS=0
START_DURING_ROTATION=1
rotate_token || fail "token rotation lost a concurrent direct service start"
grep -qx 'restart:on_demand=1:skip_autoload=1' "$SERVICE_LOG" ||
	fail "a daemon started during token rotation kept the stale token"
[ "$RUNNING_CHECKS" -eq 2 ] ||
	fail "token rotation did not recheck a daemon that was initially stopped"
[ "$START_LOCK_ACQUIRES" -eq "$START_LOCK_RELEASES" ] ||
	fail "concurrent-start token rotation leaked the startup lock"

printf '%s\n' "rpcd resource contract: OK"
