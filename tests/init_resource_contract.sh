#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
INIT="$ROOT/package/nes-emulator/files/nes-emulator.init"
RPCD="$ROOT/package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/nes-init-resource.XXXXXX")"
trap 'rm -rf "$TMP"' 0 1 2 15

extract_function() {
	sed -n "/^$1() {$/,/^}$/p" "$2"
}

fail() {
	printf '%s\n' "init resource contract: $*" >&2
	exit 1
}

# The service uid selects owner bits first, then any effective group, then
# other. Writable data needs write/search; system data needs read/search.
eval "$(extract_function directory_is_writable_by_nesd "$INIT")"
eval "$(extract_function directory_is_readable_by_nesd "$INIT")"
eval "$(extract_function directory_is_searchable_by_nesd "$INIT")"
eval "$(extract_function external_data_dir_is_usable_by_nesd "$INIT")"
NESD_UID=1000
NESD_GID=2000
NESD_GROUPS='2000 4000'
export NESD_UID NESD_GID NESD_GROUPS

PATH_METADATA_UID=1000
PATH_METADATA_GID=3000
PATH_METADATA_MODE=drwx------
export PATH_METADATA_UID PATH_METADATA_GID PATH_METADATA_MODE
directory_is_writable_by_nesd || fail "owner write/search permissions were rejected"
PATH_METADATA_MODE=dr-xrwxrwx
directory_is_writable_by_nesd &&
	fail "group/other permissions overrode a matching read-only owner"

PATH_METADATA_UID=3000
PATH_METADATA_GID=4000
PATH_METADATA_MODE=dr-xrwx---
directory_is_writable_by_nesd ||
	fail "supplementary-group write/search permissions were rejected"
PATH_METADATA_MODE=dr-xr-xrwx
directory_is_writable_by_nesd &&
	fail "other permissions overrode a matching read-only group"

PATH_METADATA_GID=3000
PATH_METADATA_MODE=dr-xr-xrwx
directory_is_writable_by_nesd || fail "other write/search permissions were rejected"
PATH_METADATA_MODE=drwxrwxr-x
directory_is_writable_by_nesd && fail "read-only other permissions were accepted"
PATH_METADATA_MODE=dr-xr-xrwt
directory_is_writable_by_nesd || fail "sticky other write/search permissions were rejected"

PATH_METADATA_UID=1000
PATH_METADATA_GID=3000
PATH_METADATA_MODE=dr-x------
directory_is_readable_by_nesd || fail "owner read/search permissions were rejected"
PATH_METADATA_MODE=d-wxr-xr-x
directory_is_readable_by_nesd &&
	fail "group/other permissions overrode a matching unreadable owner"

PATH_METADATA_UID=3000
PATH_METADATA_GID=4000
PATH_METADATA_MODE=drwxr-x---
directory_is_readable_by_nesd ||
	fail "supplementary-group read/search permissions were rejected"
PATH_METADATA_MODE=drw---xr-x
directory_is_readable_by_nesd &&
	fail "other permissions overrode a matching unreadable group"

PATH_METADATA_GID=3000
PATH_METADATA_MODE=drwx---r-x
directory_is_readable_by_nesd || fail "other read/search permissions were rejected"
PATH_METADATA_MODE=drwx---rw-
directory_is_readable_by_nesd && fail "other read without search was accepted"
PATH_METADATA_MODE=drwx---r-t
directory_is_readable_by_nesd || fail "sticky other read/search permissions were rejected"

TEST_ANCESTOR_BLOCKED=1
TEST_LEAF_MODE=drwxrwx---
read_path_metadata() {
	case "$1" in
	/mount/data)
		PATH_METADATA_MODE="$TEST_LEAF_MODE"
		PATH_METADATA_UID=3000
		PATH_METADATA_GID=4000
		;;
	/mount)
		if [ "$TEST_ANCESTOR_BLOCKED" -eq 1 ]; then
			PATH_METADATA_MODE=drwx------
		else
			PATH_METADATA_MODE=drwxr-xr-x
		fi
		PATH_METADATA_UID=0
		PATH_METADATA_GID=0
		;;
	/)
		PATH_METADATA_MODE=drwxr-xr-x
		PATH_METADATA_UID=0
		PATH_METADATA_GID=0
		;;
	*) return 1 ;;
	esac
	return 0
}
external_data_dir_is_usable_by_nesd /mount/data &&
	fail "a root-only ancestor was accepted"
TEST_ANCESTOR_BLOCKED=0
external_data_dir_is_usable_by_nesd /mount/data ||
	fail "a supplementary-group leaf with searchable ancestors was rejected"
TEST_LEAF_MODE=dr-xr-xr-x
external_data_dir_is_usable_by_nesd /mount/data read ||
	fail "a read-only system directory was rejected"
external_data_dir_is_usable_by_nesd /mount/data write &&
	fail "a read-only leaf was accepted when write access was required"
TEST_LEAF_MODE=d--x--x--x
external_data_dir_is_usable_by_nesd /mount/data read &&
	fail "a searchable but unreadable system directory was accepted"
external_data_dir_is_usable_by_nesd /mount/data invalid &&
	fail "an unknown external-directory access requirement was accepted"

grep -Fq "prepare_data_dir \"\$system_dir\" read" "$INIT" ||
	fail "system_dir does not request read-only external access"
grep -Fq "if ! external_data_dir_is_usable_by_nesd \"\$d\" \"\$required\"" "$INIT" ||
	fail "pre-existing external directories do not enforce the requested access"
grep -Fq "external data path lacks \$access access for nesd" "$INIT" ||
	fail "external-directory failures do not identify the missing access"
grep -Fq "(uid \$NESD_UID, primary gid \$NESD_GID, groups \$NESD_GROUPS): \$d;" "$INIT" ||
	fail "external-directory failures do not report the nesd uid and gid"

# Both writers of auth.token must share a root-only runtime lock. The lock file
# is created without following pre-existing links, reopened without truncation,
# revalidated after open, and acquired within a bounded number of attempts.
for source in "$INIT" "$RPCD"; do
	grep -Fq 'LOCK_DIR=/var/run/nes-emulator' "$source" ||
		fail "$source does not use the dedicated runtime lock directory"
	grep -Fq "TOKEN_LOCK_FILE=\"\$LOCK_DIR/auth.token.lock\"" "$source" ||
		fail "$source uses an unprotected or mismatched token lock"
	grep -Eq '^TOKEN_LOCK_MAX_ATTEMPTS=[1-9][0-9]*$' "$source" ||
		fail "$source does not define the bounded token-lock budget"
	grep -Fq "command exec 8<>\"\$TOKEN_LOCK_FILE\" || return 1" "$source" ||
		fail "$source cannot catch a token-lock redirection failure"
	grep -Fq "if [ \"\$attempts\" -ge \"\$TOKEN_LOCK_MAX_ATTEMPTS\" ]; then" "$source" ||
		fail "$source does not enforce the token-lock attempt budget"
	grep -Fq 'umask 077' "$source" ||
		fail "$source does not create lock resources with a root-only umask"
	grep -Fq 'set -C' "$source" ||
		fail "$source does not use no-clobber lock-file creation"
	if grep -Fq 'TOKEN_LOCK_FILE=/var/lock/' "$source"; then
		fail "$source still places the token lock in a world-writable directory"
	fi

	lock_source="$(extract_function acquire_token_lock "$source")"
	case "$source" in
	"$INIT")
		if ! grep -Fq "[ \"\$PATH_METADATA_MODE\" = drwx------ ]" "$source" ||
		   ! grep -Fq "[ \"\$PATH_METADATA_MODE\" = -rw------- ]" "$source" ||
		   ! grep -Fq "[ \"\$PATH_METADATA_NLINK\" = 1 ]" "$source"; then
			fail "$source does not validate root-only directory/file metadata"
		fi
		prepare_stub='prepare_token_lock_file() { return 0; }'
		validate_stub='validate_token_lock_file() { return 0; }'
		if ! printf '%s\n' "$lock_source" |
			grep -Fq 'prepare_token_lock_file || return 1' ||
		   ! printf '%s\n' "$lock_source" |
			grep -Fq 'validate_token_lock_file || {'; then
			fail "$source does not prepare and revalidate the token lock"
		fi
		;;
	"$RPCD")
		if ! grep -Fq "validate_directory_metadata \"\$LOCK_DIR\" drwx------ 0 0" "$source" ||
		   ! grep -Fq "validate_regular_metadata \"\$path\" -rw------- 0 0" "$source" ||
		   ! grep -Fq "[ \"\$META_NLINK\" = 1 ]" "$source"; then
			fail "$source does not validate root-only directory/file metadata"
		fi
		prepare_stub='prepare_lock_file() { return 0; }'
		validate_stub='validate_regular_metadata() { return 0; }'
		if ! printf '%s\n' "$lock_source" |
			grep -Fq "prepare_lock_file \"\$TOKEN_LOCK_FILE\" || return 1" ||
		   ! printf '%s\n' "$lock_source" |
			grep -Fq "validate_regular_metadata \"\$TOKEN_LOCK_FILE\" -rw------- 0 0 || {"; then
			fail "$source does not prepare and revalidate the token lock"
		fi
		;;
	esac
	(
		set +e
		eval "$lock_source"
		eval "$prepare_stub"
		eval "$validate_stub"
		TOKEN_LOCK_HELD=0
		TOKEN_LOCK_MAX_ATTEMPTS=3
		TOKEN_LOCK_FILE="$TMP/missing/lock"
		export TOKEN_LOCK_HELD TOKEN_LOCK_MAX_ATTEMPTS TOKEN_LOCK_FILE
		acquire_token_lock 2>/dev/null
		status=$?
		printf 'status:%s\n' "$status"
		exit 0
	) >"$TMP/lock-result"
	[ "$(cat "$TMP/lock-result")" = status:1 ] ||
		fail "$source terminated the shell instead of returning an open failure"

	(
		set +e
		eval "$lock_source"
		eval "$prepare_stub"
		eval "$validate_stub"
		TOKEN_LOCK_HELD=0
		TOKEN_LOCK_MAX_ATTEMPTS=3
		TOKEN_LOCK_FILE="$TMP/token.lock"
		: >"$TOKEN_LOCK_FILE"
		LOCK_FLOCK_CALLS=0
		LOCK_SLEEP_CALLS=0
		# Invoked indirectly by the extracted lock function evaluated above.
		# shellcheck disable=SC2317,SC2329
		flock() {
			LOCK_FLOCK_CALLS=$((LOCK_FLOCK_CALLS + 1))
			return 1
		}
		# Invoked indirectly by the extracted lock function evaluated above.
		# shellcheck disable=SC2317,SC2329
		sleep() {
			LOCK_SLEEP_CALLS=$((LOCK_SLEEP_CALLS + 1))
		}
		acquire_token_lock 2>/dev/null
		status=$?
		printf 'status:%s flock:%s sleep:%s\n' \
			"$status" "$LOCK_FLOCK_CALLS" "$LOCK_SLEEP_CALLS"
		exit 0
	) >"$TMP/lock-budget-result"
	[ "$(cat "$TMP/lock-budget-result")" = 'status:1 flock:3 sleep:2' ] ||
		fail "$source does not stop at the configured token-lock attempt budget"
done

[ "$(grep -c 'AUTH_TOKEN' "$INIT" || :)" -eq 0 ] ||
	fail "the init script retains an unused authentication token value"

printf '%s\n' "init resource contract: OK"
