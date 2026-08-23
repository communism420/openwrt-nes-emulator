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
# other. Every accepted class must grant both write and search permission.
eval "$(extract_function directory_is_writable_by_nesd "$INIT")"
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

TEST_ANCESTOR_BLOCKED=1
read_path_metadata() {
	case "$1" in
	/mount/data)
		PATH_METADATA_MODE=drwxrwx---
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

if ! grep -Fq "if ! external_data_dir_is_usable_by_nesd \"\$d\"" "$INIT" ||
   ! grep -Fq 'external data path is not writable and searchable by nesd' "$INIT"; then
	fail "pre-existing external directories do not fail with a diagnostic"
fi

# Both writers of auth.token must share one root-controlled lock. Wrapping exec
# with command keeps a redirection failure catchable under BusyBox ash.
for source in "$INIT" "$RPCD"; do
	grep -Fq 'TOKEN_LOCK_FILE=/etc/nes-emulator/.auth.token.lock' "$source" ||
		fail "$source uses an unprotected or mismatched token lock"
	grep -Fq "command exec 8>\"\$TOKEN_LOCK_FILE\" || return 1" "$source" ||
		fail "$source cannot catch a token-lock redirection failure"
	if grep -Fq 'TOKEN_LOCK_FILE=/var/lock/' "$source"; then
		fail "$source still places the token lock in a world-writable directory"
	fi

	lock_source="$(extract_function acquire_token_lock "$source")"
	(
		set +e
		eval "$lock_source"
		TOKEN_LOCK_HELD=0
		TOKEN_LOCK_FILE="$TMP/missing/lock"
		export TOKEN_LOCK_HELD TOKEN_LOCK_FILE
		acquire_token_lock 2>/dev/null
		status=$?
		printf 'status:%s\n' "$status"
		exit 0
	) >"$TMP/lock-result"
	[ "$(cat "$TMP/lock-result")" = status:1 ] ||
		fail "$source terminated the shell instead of returning an open failure"
done

[ "$(grep -c 'AUTH_TOKEN' "$INIT" || :)" -eq 0 ] ||
	fail "the init script retains an unused authentication token value"

printf '%s\n' "init resource contract: OK"
