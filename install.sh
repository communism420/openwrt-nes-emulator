#!/bin/sh
set -eu
set -f

umask 077
LC_ALL=C
PATH="${OPENWRT_NES_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}"
export LC_ALL PATH

PROGRAM='openwrt-nes-installer'
REPOSITORY='communism420/openwrt-nes-emulator'
LATEST_BASE="${OPENWRT_NES_LATEST_BASE:-https://github.com/$REPOSITORY/releases/latest/download}"
RELEASES_BASE="${OPENWRT_NES_RELEASES_BASE:-https://github.com/$REPOSITORY/releases/download}"
OPENWRT_RELEASE_FILE="${OPENWRT_NES_RELEASE_FILE:-/etc/openwrt_release}"
APK_ARCH_FILE="${OPENWRT_NES_APK_ARCH_FILE:-/etc/apk/arch}"
MODEL_FILE="${OPENWRT_NES_MODEL_FILE:-/tmp/sysinfo/model}"
TEMP_ROOT="${OPENWRT_NES_TMPDIR:-${TMPDIR:-/tmp}}"
MIN_TEMP_KB=4096
MAX_MANIFEST_BYTES=1048576
MAX_APK_BYTES=8388608

say() {
	printf '%s: %s\n' "$PROGRAM" "$*"
}

die() {
	printf '%s: ERROR: %s\n' "$PROGRAM" "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
Usage: sh install.sh

Detect this router's OpenWrt package architecture, download the matching
nes-emulator and LuCI packages from the latest GitHub release, verify their
SHA-256 checksums, and install both packages in one transaction.
EOF
}

case "${1:-}" in
	'') ;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown argument: $1"
		;;
esac

case "$LATEST_BASE" in
	https://*) ;;
	*) die 'the latest-release download base must use HTTPS' ;;
esac
case "$RELEASES_BASE" in
	https://*) ;;
	*) die 'the tag-scoped release download base must use HTTPS' ;;
esac

user_id="$(id -u 2>/dev/null)" || die 'cannot determine the current user'
[ "$user_id" = '0' ] || die 'run this installer as root on the router'

[ -r "$OPENWRT_RELEASE_FILE" ] ||
	die "OpenWrt release metadata is missing: $OPENWRT_RELEASE_FILE"

DISTRIB_ID=''
DISTRIB_RELEASE=''
DISTRIB_REVISION=''
DISTRIB_TARGET=''
DISTRIB_ARCH=''
# This file is supplied by the installed OpenWrt system.
# shellcheck disable=SC1090
. "$OPENWRT_RELEASE_FILE"

[ "$DISTRIB_ID" = 'OpenWrt' ] ||
	die "this installer only supports OpenWrt (detected: ${DISTRIB_ID:-unknown})"
case "$DISTRIB_RELEASE" in
	25.12|25.12.*) ;;
	*)
		die "prebuilt APKs support OpenWrt 25.12.x only (detected: ${DISTRIB_RELEASE:-unknown})"
		;;
esac

if ! command -v apk >/dev/null 2>&1; then
	if command -v opkg >/dev/null 2>&1; then
		die 'this router uses opkg; prebuilt releases require OpenWrt 25.12 with apk'
	fi
	die 'the apk package manager is required'
fi

apk_version="$(apk --version 2>/dev/null)" ||
	die 'cannot determine the apk-tools version'
case "$apk_version" in
	*'
'*) die 'apk returned a malformed version string' ;;
esac
case "$apk_version" in
	'apk-tools 3.'*) ;;
	*) die "apk-tools v3 is required (detected: ${apk_version:-unknown})" ;;
esac

for required_command in awk df grep mktemp sed sha256sum uname wc; do
	command -v "$required_command" >/dev/null 2>&1 ||
		die "required command is missing: $required_command"
done

release_package_arch="$DISTRIB_ARCH"
[ -n "$release_package_arch" ] ||
	die 'OpenWrt release metadata does not declare a package architecture'
case "$release_package_arch" in
	*'
'*) die 'OpenWrt release metadata contains more than one package architecture' ;;
esac
[ "${#release_package_arch}" -le 64 ] ||
	die 'OpenWrt release metadata contains an unexpectedly long package architecture'
printf '%s\n' "$release_package_arch" |
	grep -Eq '^[A-Za-z0-9][A-Za-z0-9._+-]*$' ||
	die "OpenWrt release metadata contains an unsafe package architecture: $release_package_arch"

[ -r "$APK_ARCH_FILE" ] ||
	die "apk package architecture metadata is missing: $APK_ARCH_FILE"
package_arch="$(sed -n \
	-e 's/^[[:space:]]*//' \
	-e 's/[[:space:]]*$//' \
	-e '/^$/d' \
	-e 'p' \
	-e 'q' \
	"$APK_ARCH_FILE")" ||
	die 'cannot read the apk package architecture metadata'
[ -n "$package_arch" ] ||
	die 'apk package architecture metadata is empty'
[ "${#package_arch}" -le 64 ] ||
	die 'apk package architecture metadata is unexpectedly long'
printf '%s\n' "$package_arch" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._+-]*$' ||
	die "apk package architecture metadata is unsafe: $package_arch"
[ "$package_arch" = "$release_package_arch" ] ||
	die "OpenWrt package architecture $release_package_arch does not match apk database architecture $package_arch"

router_model='unknown model'
if [ -r "$MODEL_FILE" ]; then
	router_model="$(sed -n '1p' "$MODEL_FILE")"
	[ -n "$router_model" ] || router_model='unknown model'
fi

say "router: $router_model"
openwrt_version="${DISTRIB_RELEASE:-unknown}"
[ -z "$DISTRIB_REVISION" ] || openwrt_version="$openwrt_version $DISTRIB_REVISION"
say "OpenWrt: $openwrt_version (${DISTRIB_TARGET:-unknown})"
say "OpenWrt package architecture: $package_arch"
say "kernel architecture: $(uname -m)"

[ -d "$TEMP_ROOT" ] || die "temporary directory does not exist: $TEMP_ROOT"
[ "$TEMP_ROOT" = '/' ] || TEMP_ROOT="${TEMP_ROOT%/}"
work_dir=''

cleanup() {
	cleanup_status="$1"
	trap - 0 1 2 15
	case "$work_dir" in
		"$TEMP_ROOT"/openwrt-nes-install.*)
			rm -rf -- "$work_dir"
			;;
		*)
			printf '%s: WARNING: refusing to remove unexpected path: %s\n' \
				"$PROGRAM" "$work_dir" >&2
			;;
	esac
	exit "$cleanup_status"
}

work_dir="$(mktemp -d "$TEMP_ROOT/openwrt-nes-install.XXXXXX")" ||
	die "cannot create a private directory below $TEMP_ROOT"
trap 'cleanup $?' 0
trap 'cleanup 129' 1
trap 'cleanup 130' 2
trap 'cleanup 143' 15
chmod 0700 "$work_dir"

available_kb="$(df -Pk "$work_dir" | awk 'END { print $4 }')"
case "$available_kb" in
	''|*[!0-9]*) die 'cannot determine available temporary storage' ;;
esac
[ "$available_kb" -ge "$MIN_TEMP_KB" ] ||
	die "at least ${MIN_TEMP_KB} KiB of free temporary storage is required"

download_to() {
	download_url="$1"
	download_output="$2"
	download_max_bytes="$3"
	download_part="$download_output.part"
	download_blocks=$(((download_max_bytes + 511) / 512))
	download_attempt=1
	download_ok=0
	while [ "$download_attempt" -le 3 ] && [ "$download_ok" -ne 1 ]; do
		rm -f -- "$download_part"

		if command -v uclient-fetch >/dev/null 2>&1 &&
			(ulimit -f "$download_blocks" &&
				exec uclient-fetch -q -T 30 -O "$download_part" "$download_url") &&
			[ -s "$download_part" ] &&
			[ "$(wc -c < "$download_part")" -le "$download_max_bytes" ]; then
			download_ok=1
		elif command -v curl >/dev/null 2>&1 &&
			(ulimit -f "$download_blocks" &&
				exec curl --fail --location --silent --show-error \
					--connect-timeout 20 --max-time 120 \
					--output "$download_part" "$download_url") &&
			[ -s "$download_part" ] &&
			[ "$(wc -c < "$download_part")" -le "$download_max_bytes" ]; then
			download_ok=1
		elif command -v wget >/dev/null 2>&1 &&
			(ulimit -f "$download_blocks" &&
				exec wget -q -T 30 -O "$download_part" "$download_url") &&
			[ -s "$download_part" ] &&
			[ "$(wc -c < "$download_part")" -le "$download_max_bytes" ]; then
			download_ok=1
		else
			download_attempt=$((download_attempt + 1))
		fi
	done

	if [ "$download_ok" -ne 1 ] || [ ! -s "$download_part" ]; then
		rm -f -- "$download_part"
		die "download failed or exceeded the safe size limit: $download_url"
	fi
	mv -f -- "$download_part" "$download_output"
}

checksums="$work_dir/SHA256SUMS"
say 'locating the latest published release'
download_to "${LATEST_BASE%/}/SHA256SUMS" "$checksums" "$MAX_MANIFEST_BYTES"

manifest_entry() {
	entry_prefix="$1"
	entry_suffix="$2"
	awk -v prefix="$entry_prefix" -v suffix="$entry_suffix" '
		NF == 2 &&
		length($1) == 64 && $1 !~ /[^0-9a-f]/ &&
		index($2, prefix) == 1 &&
		length($2) > length(prefix) + length(suffix) &&
		substr($2, length($2) - length(suffix) + 1) == suffix {
			count++
			result = $1 " " $2
		}
		END {
			if (count == 1)
				print result
			else
				exit 1
		}
	' "$checksums"
}

native_entry="$(manifest_entry 'nes-emulator-' "-$package_arch.apk")" ||
	die "the latest release has no unique native APK for architecture $package_arch"
native_hash="${native_entry%% *}"
native_name="${native_entry#* }"
if [ "$native_hash" = "$native_entry" ] || [ -z "$native_name" ]; then
	die 'invalid native APK checksum entry'
fi

luci_entry="$(manifest_entry 'luci-app-nes-emulator-' "-$package_arch.apk")" ||
	die "the latest release has no unique LuCI APK for architecture $package_arch"
luci_hash="${luci_entry%% *}"
luci_name="${luci_entry#* }"
if [ "$luci_hash" = "$luci_entry" ] || [ -z "$luci_name" ]; then
	die 'invalid LuCI APK checksum entry'
fi

package_version="${native_name#nes-emulator-}"
package_version="${package_version%-"$package_arch".apk}"
luci_version="${luci_name#luci-app-nes-emulator-}"
luci_version="${luci_version%-"$package_arch".apk}"

printf '%s\n' "$package_version" |
	grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+-r[0-9]+$' ||
	die "the latest release has an invalid package version: $package_version"
[ "${#package_version}" -le 40 ] ||
	die 'the latest release has an unexpectedly long package version'
[ "$luci_version" = "$package_version" ] ||
	die 'the native and LuCI package revisions do not match'
[ "$native_name" = "nes-emulator-$package_version-$package_arch.apk" ] ||
	die 'the native APK filename is invalid'
[ "$luci_name" = "luci-app-nes-emulator-$package_version-$package_arch.apk" ] ||
	die 'the LuCI APK filename is invalid'

release_tag="v$package_version"
release_base="${RELEASES_BASE%/}/$release_tag"
native_path="$work_dir/$native_name"
luci_path="$work_dir/$luci_name"

say "release: $release_tag"
say "downloading $native_name"
download_to "$release_base/$native_name" "$native_path" "$MAX_APK_BYTES"
say "downloading $luci_name"
download_to "$release_base/$luci_name" "$luci_path" "$MAX_APK_BYTES"

verify_sha256() {
	verify_path="$1"
	verify_expected="$2"
	verify_actual="$(sha256sum "$verify_path" | awk 'NR == 1 { print $1 }')"
	[ "$verify_actual" = "$verify_expected" ] ||
		die "SHA-256 verification failed for ${verify_path##*/}"
}

verify_sha256 "$native_path" "$native_hash"
verify_sha256 "$luci_path" "$luci_hash"
apk --allow-untrusted verify "$native_path" "$luci_path" >/dev/null ||
	die 'APK container verification failed'
say 'checksums and APK containers verified'

say 'installing trusted LuCI runtime dependencies'
apk --update-cache --wait 120 add luci-base rpcd jshn jsonfilter cgi-io
say 'installing the verified emulator and LuCI packages in one transaction'
apk --repositories-file /dev/null --no-network --no-cache \
	--allow-untrusted --wait 120 add \
	"$native_path" "$luci_path"

say "installation complete: $package_version for $package_arch"
say 'open LuCI -> Services -> NES Emulator'
