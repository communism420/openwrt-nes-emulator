#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
INIT="$ROOT/package/nes-emulator/files/nes-emulator.init"
RPCD="$ROOT/package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator"
PACKAGE_MAKEFILE="$ROOT/package/nes-emulator/Makefile"
APK_BUILDER="$ROOT/scripts/build-apks.sh"

extract_function() {
	local name="$1" source="$2"

	sed -n "/^${name}()/,/^}$/p" "$source"
}

INIT_HELPER="$(extract_function load_extra_rom_dirs "$INIT")"
RPCD_HELPER="$(extract_function scan_configured_extra_rom_roots "$RPCD")"
[ -n "$INIT_HELPER" ] || {
	echo "load_extra_rom_dirs was not found in the init script" >&2
	exit 1
}
[ -n "$RPCD_HELPER" ] || {
	echo "scan_configured_extra_rom_roots was not found in rpcd" >&2
	exit 1
}

# Every layer must reject the separators used by nesd's -e wire format. If a
# path containing one of them reached nesd, it would be split into extra roots.
for validator_source in "$INIT" "$RPCD"; do
	VALIDATOR="$(extract_function valid_data_dir "$validator_source")"
	[ -n "$VALIDATOR" ] || {
		echo "valid_data_dir was not found in $validator_source" >&2
		exit 1
	}
	eval "$VALIDATOR"
	valid_data_dir /mnt/usb/roms || {
		echo "$validator_source rejected a safe extra ROM path" >&2
		exit 1
	}
	for unsafe_path in \
		'/mnt/usb:roms' \
		'/mnt/usb;roms' \
		'/mnt/usb,roms'
	do
		if valid_data_dir "$unsafe_path"; then
			echo "$validator_source accepted nesd separator: $unsafe_path" >&2
			exit 1
		fi
	done
done

# Evaluate the exact production helpers. Their dependencies below are test
# doubles which count calls without reading a real UCI configuration.
eval "$INIT_HELPER"
eval "$RPCD_HELPER"

FLAG_MODE=missing
BOOL_CALLS=0
FOREACH_CALLS=0
FOREACH_ARGUMENTS=

config_get_bool() {
	local destination="$1" section="$2" option="$3" default="$4" value

	[ "$section:$option:$default" = "main:extra_rom_dirs_enabled:0" ] || {
		echo "unexpected config_get_bool arguments: $*" >&2
		exit 1
	}
	BOOL_CALLS=$((BOOL_CALLS + 1))
	case "$FLAG_MODE" in
		on) value=1 ;;
		missing|off|malformed) value=0 ;;
		*)
			echo "unknown flag test mode: $FLAG_MODE" >&2
			exit 1
			;;
	esac
	eval "$destination=\$value"
}

config_list_foreach() {
	FOREACH_CALLS=$((FOREACH_CALLS + 1))
	FOREACH_ARGUMENTS="$*"
}

run_helper_case() {
	local helper="$1" callback="$2" mode="$3" expected_calls="$4"
	local stored_before

	FLAG_MODE="$mode"
	BOOL_CALLS=0
	FOREACH_CALLS=0
	FOREACH_ARGUMENTS=
	STORED_EXTRA_ROM_DIRS='/mnt/usb-a/roms
/mnt/usb-b/roms'
	stored_before="$STORED_EXTRA_ROM_DIRS"

	"$helper"
	[ "$BOOL_CALLS" -eq 1 ] || {
		echo "$helper/$mode did not read the opt-in flag exactly once" >&2
		exit 1
	}
	[ "$FOREACH_CALLS" -eq "$expected_calls" ] || {
		echo "$helper/$mode called config_list_foreach $FOREACH_CALLS times" >&2
		exit 1
	}
	[ "$STORED_EXTRA_ROM_DIRS" = "$stored_before" ] || {
		echo "$helper/$mode changed stored extra_rom_dir values" >&2
		exit 1
	}
	if [ "$expected_calls" -eq 1 ]; then
		[ "$FOREACH_ARGUMENTS" = "main extra_rom_dir $callback" ] || {
			echo "$helper/$mode used unexpected foreach arguments: $FOREACH_ARGUMENTS" >&2
			exit 1
		}
	else
		[ -z "$FOREACH_ARGUMENTS" ] || {
			echo "$helper/$mode exposed disabled list values" >&2
			exit 1
		}
	fi
}

for mode in missing off malformed; do
	run_helper_case load_extra_rom_dirs append_extra "$mode" 0
	run_helper_case scan_configured_extra_rom_roots scan_rom_root "$mode" 0
done
run_helper_case load_extra_rom_dirs append_extra on 1
run_helper_case scan_configured_extra_rom_roots scan_rom_root on 1

# Disabling external roots must remain a scalar opt-in change. In particular,
# neither packaging path may delete the preserved UCI list.
for source in "$PACKAGE_MAKEFILE" "$APK_BUILDER"; do
	if sed 's/\$\$/\$/g' "$source" |
	   grep -E "uci([[:space:]]+-q)?[[:space:]]+delete[[:space:]]+['\"]?nes-emulator\\.main\\.extra_rom_dir(['\"]?[[:space:]]|['\"]?$)" \
		   >/dev/null; then
		echo "$source deletes preserved extra_rom_dir list entries" >&2
		exit 1
	fi

	MIGRATION_HELPER="$(
		extract_function migrate_extra_rom_dirs_enabled "$source" |
			sed 's/\$\$/\$/g'
	)"
	[ -n "$MIGRATION_HELPER" ] || {
		echo "migrate_extra_rom_dirs_enabled was not found in $source" >&2
		exit 1
	}
	eval "$MIGRATION_HELPER"

	MIGRATION_FLAG=1
	MIGRATION_CALLS=0
	MIGRATION_EXTRA_ROM_DIRS='/mnt/usb-a/roms
/mnt/usb-b/roms'
	MIGRATION_LIST_BEFORE="$MIGRATION_EXTRA_ROM_DIRS"
	uci() {
		[ "$*" = "-q set nes-emulator.main.extra_rom_dirs_enabled=0" ] || {
			echo "unexpected migration UCI operation: $*" >&2
			return 1
		}
		MIGRATION_CALLS=$((MIGRATION_CALLS + 1))
		MIGRATION_FLAG=0
	}

	migrate_extra_rom_dirs_enabled
	[ "$MIGRATION_CALLS" -eq 1 ] && [ "$MIGRATION_FLAG" -eq 0 ] || {
		echo "$source did not disable external roots exactly once" >&2
		exit 1
	}
	[ "$MIGRATION_EXTRA_ROM_DIRS" = "$MIGRATION_LIST_BEFORE" ] || {
		echo "$source changed preserved extra_rom_dir list entries" >&2
		exit 1
	}
done

echo "extra ROM directories contract: OK"
