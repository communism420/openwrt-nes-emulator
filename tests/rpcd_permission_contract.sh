#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
RPCD="$ROOT/package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator"

# Execute the exact permission helpers from the installed rpcd bridge.
eval "$(sed -n '/^mode_allows_nesd()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^path_is_readable_by_nesd()/,/^}$/p' "$RPCD")"

TEMPORARY="$(mktemp -d /tmp/nes-rpc-permissions.XXXXXXXX)"
cleanup() {
	chmod -R u+rwx "$TEMPORARY" 2>/dev/null || :
	rm -rf -- "$TEMPORARY"
}
trap cleanup 0 1 2 15

mkdir "$TEMPORARY/parent"
printf x >"$TEMPORARY/parent/game.nes"

NESD_UID="$(id -u)"
NESD_GROUPS="$(id -G)"
chmod 0700 "$TEMPORARY" "$TEMPORARY/parent"
chmod 0600 "$TEMPORARY/parent/game.nes"
path_is_readable_by_nesd "$TEMPORARY/parent/game.nes"
chmod 0000 "$TEMPORARY/parent/game.nes"
! path_is_readable_by_nesd "$TEMPORARY/parent/game.nes"

NESD_UID=999999
NESD_GROUPS="$(id -g)"
chmod 0750 "$TEMPORARY" "$TEMPORARY/parent"
chmod 0640 "$TEMPORARY/parent/game.nes"
path_is_readable_by_nesd "$TEMPORARY/parent/game.nes"
chmod 0600 "$TEMPORARY/parent/game.nes"
! path_is_readable_by_nesd "$TEMPORARY/parent/game.nes"

NESD_GROUPS=999998
chmod 0755 "$TEMPORARY" "$TEMPORARY/parent"
chmod 0604 "$TEMPORARY/parent/game.nes"
path_is_readable_by_nesd "$TEMPORARY/parent/game.nes"

echo "rpcd permission contract: OK"
