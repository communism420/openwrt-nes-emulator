#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/openwrt-nes-install-test.XXXXXX")"

cleanup() {
	status="$1"
	trap - 0 1 2 15
	case "$TEST_ROOT" in
		"${TMPDIR:-/tmp}"/openwrt-nes-install-test.*)
			rm -rf -- "$TEST_ROOT"
			;;
	esac
	exit "$status"
}
trap 'cleanup $?' 0
trap 'cleanup 129' 1
trap 'cleanup 130' 2
trap 'cleanup 143' 15

fail() {
	printf 'installer contract: %s\n' "$*" >&2
	exit 1
}

MOCK_BIN="$TEST_ROOT/bin"
FIXTURES="$TEST_ROOT/fixtures"
WORK_ROOT="$TEST_ROOT/work"
LOG_ROOT="$TEST_ROOT/log"
mkdir -p "$MOCK_BIN" "$FIXTURES" "$WORK_ROOT" "$LOG_ROOT"

cat > "$MOCK_BIN/id" <<'EOF'
#!/bin/sh
if [ "${1:-}" = '-u' ]; then
	printf '%s\n' "${INSTALL_TEST_UID:-0}"
	exit 0
fi
exec /usr/bin/id "$@"
EOF

cat > "$MOCK_BIN/uclient-fetch" <<'EOF'
#!/bin/sh
output=''
url=''
while [ "$#" -gt 0 ]; do
	case "$1" in
		-q) ;;
		-T)
			shift
			[ "$#" -gt 0 ] || exit 64
			;;
		-O)
			shift
			[ "$#" -gt 0 ] || exit 64
			output="$1"
			;;
		*) url="$1" ;;
	esac
	shift
done
[ -n "$output" ] && [ -n "$url" ] || exit 64
printf '%s\n' "$url" >> "$INSTALL_TEST_LOG/fetch.log"
name="${url##*/}"
[ -f "$INSTALL_TEST_FIXTURES/$name" ] || exit 8
cp "$INSTALL_TEST_FIXTURES/$name" "$output"
EOF

cat > "$MOCK_BIN/apk" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$INSTALL_TEST_LOG/apk.log"
if [ "${1:-}" = '--print-arch' ]; then
	# apk-tools reports its generic compile-time default here, not OpenWrt's
	# package profile. A real aarch64_cortex-a53 router commonly prints aarch64.
	printf '%s\n' 'aarch64'
	exit 0
fi
if [ "${1:-}" = '--version' ]; then
	printf '%s\n' "${INSTALL_TEST_APK_VERSION:-apk-tools 3.0.0, test build}"
	exit 0
fi
case "$*" in
	--allow-untrusted\ verify\ *)
		[ "${INSTALL_TEST_VERIFY_FAIL:-0}" = '0' ]
		;;
	--update-cache\ --wait\ 120\ add\ luci-base\ rpcd)
		[ "${INSTALL_TEST_DEPENDENCY_FAIL:-0}" = '0' ]
		;;
	--repositories-file\ /dev/null\ --no-network\ --no-cache\ --allow-untrusted\ --wait\ 120\ add\ *)
		[ "${INSTALL_TEST_ADD_FAIL:-0}" = '0' ]
		;;
	*)
		exit 64
		;;
esac
EOF

cat > "$MOCK_BIN/curl" <<'EOF'
#!/bin/sh
exit 7
EOF

cat > "$MOCK_BIN/wget" <<'EOF'
#!/bin/sh
exit 7
EOF

chmod 0755 "$MOCK_BIN/id" "$MOCK_BIN/uclient-fetch" "$MOCK_BIN/apk" \
	"$MOCK_BIN/curl" "$MOCK_BIN/wget"

RELEASE_FILE="$TEST_ROOT/openwrt_release"
APK_ARCH_FILE="$TEST_ROOT/apk_arch"
MODEL_FILE="$TEST_ROOT/model"
cat > "$RELEASE_FILE" <<'EOF'
DISTRIB_ID='OpenWrt'
DISTRIB_RELEASE='25.12.5'
DISTRIB_REVISION='r33051-f5dae5ece4'
DISTRIB_TARGET='x86/64'
DISTRIB_ARCH='x86_64'
EOF
printf '%s\n' 'x86_64' > "$APK_ARCH_FILE"
printf '%s\n' 'Contract Test Router' > "$MODEL_FILE"

reset_logs() {
	: > "$LOG_ROOT/fetch.log"
	: > "$LOG_ROOT/apk.log"
}

write_fixture() {
	fixture_version="$1"
	fixture_abi="$2"
	native_name="nes-emulator-$fixture_version-$fixture_abi.apk"
	luci_name="luci-app-nes-emulator-$fixture_version-$fixture_abi.apk"
	printf 'native:%s:%s\n' "$fixture_version" "$fixture_abi" > "$FIXTURES/$native_name"
	printf 'luci:%s:%s\n' "$fixture_version" "$fixture_abi" > "$FIXTURES/$luci_name"
	native_hash="$(sha256sum "$FIXTURES/$native_name" | awk 'NR == 1 { print $1 }')"
	luci_hash="$(sha256sum "$FIXTURES/$luci_name" | awk 'NR == 1 { print $1 }')"
	{
		printf '%s  %s\n' "$luci_hash" "$luci_name"
		printf '%s  %s\n' "$native_hash" "$native_name"
	} > "$FIXTURES/SHA256SUMS"
}

run_installer() {
	OPENWRT_NES_PATH="${CASE_PATH:-$MOCK_BIN:$PATH}" \
	INSTALL_TEST_FIXTURES="$FIXTURES" \
	INSTALL_TEST_LOG="$LOG_ROOT" \
	INSTALL_TEST_UID="${CASE_UID:-0}" \
	INSTALL_TEST_APK_VERSION="${CASE_APK_VERSION:-apk-tools 3.0.0, test build}" \
	INSTALL_TEST_VERIFY_FAIL="${CASE_VERIFY_FAIL:-0}" \
	INSTALL_TEST_DEPENDENCY_FAIL="${CASE_DEPENDENCY_FAIL:-0}" \
	INSTALL_TEST_ADD_FAIL="${CASE_ADD_FAIL:-0}" \
	OPENWRT_NES_RELEASE_FILE="${CASE_RELEASE_FILE:-$RELEASE_FILE}" \
	OPENWRT_NES_APK_ARCH_FILE="${CASE_APK_ARCH_FILE:-$APK_ARCH_FILE}" \
	OPENWRT_NES_MODEL_FILE="$MODEL_FILE" \
	OPENWRT_NES_TMPDIR="$WORK_ROOT" \
	OPENWRT_NES_LATEST_BASE='https://fixtures.invalid/releases/latest/download' \
	OPENWRT_NES_RELEASES_BASE='https://fixtures.invalid/releases/download' \
	sh "$ROOT/install.sh"
}

assert_no_install_workdir() {
	remaining="$(find "$WORK_ROOT" -mindepth 1 -maxdepth 1 -print -quit)"
	[ -z "$remaining" ] || fail "temporary installer directory leaked: $remaining"
}

write_fixture '1.2.3-r4' 'x86_64'
reset_logs
CASE_UID=0 CASE_VERIFY_FAIL=0 CASE_DEPENDENCY_FAIL=0 CASE_ADD_FAIL=0
export CASE_UID CASE_VERIFY_FAIL CASE_DEPENDENCY_FAIL CASE_ADD_FAIL
run_installer > "$TEST_ROOT/success.out" 2> "$TEST_ROOT/success.err" ||
	fail 'valid installation failed'
grep -Fq 'release: v1.2.3-r4' "$TEST_ROOT/success.out" ||
	fail 'detected release was not reported'
grep -Fq 'OpenWrt package architecture: x86_64' "$TEST_ROOT/success.out" ||
	fail 'detected ABI was not reported'
grep -Fq 'installation complete: 1.2.3-r4 for x86_64' "$TEST_ROOT/success.out" ||
	fail 'successful installation was not reported'
grep -Fxq 'https://fixtures.invalid/releases/latest/download/SHA256SUMS' \
	"$LOG_ROOT/fetch.log" || fail 'latest checksum URL was not requested'
grep -Fxq 'https://fixtures.invalid/releases/download/v1.2.3-r4/nes-emulator-1.2.3-r4-x86_64.apk' \
	"$LOG_ROOT/fetch.log" || fail 'native package did not use the tag-scoped URL'
grep -Fxq 'https://fixtures.invalid/releases/download/v1.2.3-r4/luci-app-nes-emulator-1.2.3-r4-x86_64.apk' \
	"$LOG_ROOT/fetch.log" || fail 'LuCI package did not use the tag-scoped URL'
[ "$(grep -c '^--allow-untrusted verify ' "$LOG_ROOT/apk.log")" -eq 1 ] ||
	fail 'APK containers were not verified together exactly once'
[ "$(grep -c '^--update-cache --wait 120 add luci-base rpcd$' "$LOG_ROOT/apk.log")" -eq 1 ] ||
	fail 'trusted LuCI dependencies were not installed exactly once'
[ "$(grep -c '^--repositories-file /dev/null --no-network --no-cache --allow-untrusted --wait 120 add ' "$LOG_ROOT/apk.log")" -eq 1 ] ||
	fail 'packages were not installed in one transaction'
! grep -Eq -- '--update-cache .*--allow-untrusted|--allow-untrusted .*--update-cache' \
	"$LOG_ROOT/apk.log" || fail 'allow-untrusted leaked into a repository transaction'
local_add_line="$(grep '^--repositories-file /dev/null --no-network --no-cache --allow-untrusted --wait 120 add ' \
	"$LOG_ROOT/apk.log")"
printf '%s\n' "$local_add_line" | grep -Fq 'nes-emulator-1.2.3-r4-x86_64.apk' ||
	fail 'native package was absent from the APK transaction'
printf '%s\n' "$local_add_line" | grep -Fq 'luci-app-nes-emulator-1.2.3-r4-x86_64.apk' ||
	fail 'LuCI package was absent from the APK transaction'
! grep -Fxq -- '--print-arch' "$LOG_ROOT/apk.log" ||
	fail 'installer used apk --print-arch instead of the package architecture'
assert_no_install_workdir

# Regression for real OpenWrt AArch64 systems: apk-tools is compiled with the
# generic aarch64 default, while release assets use the exact package profile.
AARCH64_RELEASE_FILE="$TEST_ROOT/openwrt_release.aarch64-cortex-a53"
AARCH64_APK_ARCH_FILE="$TEST_ROOT/apk_arch.aarch64-cortex-a53"
sed \
	-e "s|DISTRIB_TARGET='x86/64'|DISTRIB_TARGET='mediatek/filogic'|" \
	-e "s|DISTRIB_ARCH='x86_64'|DISTRIB_ARCH='aarch64_cortex-a53'|" \
	"$RELEASE_FILE" > "$AARCH64_RELEASE_FILE"
printf '\n  %s  \n%s\n' 'aarch64_cortex-a53' 'all' > \
	"$AARCH64_APK_ARCH_FILE"
write_fixture '1.2.3-r4' 'aarch64_cortex-a53'
# Include the generic apk-tools default as a decoy. The installer must select
# the OpenWrt package architecture even when both suffixes are present.
printf '%s\n' 'native:1.2.3-r4:aarch64' > \
	"$FIXTURES/nes-emulator-1.2.3-r4-aarch64.apk"
printf '%s\n' 'luci:1.2.3-r4:aarch64' > \
	"$FIXTURES/luci-app-nes-emulator-1.2.3-r4-aarch64.apk"
generic_native_hash="$(sha256sum \
	"$FIXTURES/nes-emulator-1.2.3-r4-aarch64.apk" | awk 'NR == 1 { print $1 }')"
generic_luci_hash="$(sha256sum \
	"$FIXTURES/luci-app-nes-emulator-1.2.3-r4-aarch64.apk" | awk 'NR == 1 { print $1 }')"
{
	printf '%s  %s\n' "$generic_native_hash" \
		'nes-emulator-1.2.3-r4-aarch64.apk'
	printf '%s  %s\n' "$generic_luci_hash" \
		'luci-app-nes-emulator-1.2.3-r4-aarch64.apk'
} >> "$FIXTURES/SHA256SUMS"
reset_logs
CASE_RELEASE_FILE="$AARCH64_RELEASE_FILE"
CASE_APK_ARCH_FILE="$AARCH64_APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE
run_installer > "$TEST_ROOT/aarch64-profile.out" \
	2> "$TEST_ROOT/aarch64-profile.err" ||
	fail 'aarch64 router with an OpenWrt package profile was rejected'
grep -Fq 'OpenWrt package architecture: aarch64_cortex-a53' \
	"$TEST_ROOT/aarch64-profile.out" ||
	fail 'aarch64 OpenWrt package profile was not reported'
grep -Fxq 'https://fixtures.invalid/releases/download/v1.2.3-r4/nes-emulator-1.2.3-r4-aarch64_cortex-a53.apk' \
	"$LOG_ROOT/fetch.log" ||
	fail 'native package did not use the exact OpenWrt package profile'
grep -Fxq 'https://fixtures.invalid/releases/download/v1.2.3-r4/luci-app-nes-emulator-1.2.3-r4-aarch64_cortex-a53.apk' \
	"$LOG_ROOT/fetch.log" ||
	fail 'LuCI package did not use the exact OpenWrt package profile'
! grep -Eq '/(nes-emulator|luci-app-nes-emulator)-1\.2\.3-r4-aarch64\.apk$' \
	"$LOG_ROOT/fetch.log" ||
	fail 'generic apk-tools architecture was selected instead of the package profile'
[ "$(grep -c '^--repositories-file /dev/null --no-network --no-cache --allow-untrusted --wait 120 add ' "$LOG_ROOT/apk.log")" -eq 1 ] ||
	fail 'aarch64 package pair was not installed in one transaction'
! grep -Fxq -- '--print-arch' "$LOG_ROOT/apk.log" ||
	fail 'aarch64 profile selection regressed to apk --print-arch'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
CASE_APK_ARCH_FILE="$APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE
write_fixture '1.2.3-r4' 'x86_64'

MISMATCH_APK_ARCH_FILE="$TEST_ROOT/apk_arch.mismatch"
printf '%s\n' 'aarch64' > "$MISMATCH_APK_ARCH_FILE"
reset_logs
CASE_RELEASE_FILE="$AARCH64_RELEASE_FILE"
CASE_APK_ARCH_FILE="$MISMATCH_APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE
if run_installer > "$TEST_ROOT/arch-mismatch.out" \
	2> "$TEST_ROOT/arch-mismatch.err"; then
	fail 'mismatched OpenWrt and apk database architectures were accepted'
fi
grep -Fq 'does not match apk database architecture aarch64' \
	"$TEST_ROOT/arch-mismatch.err" ||
	fail 'package architecture mismatch was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] ||
	fail 'package architecture mismatch accessed the network'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
CASE_APK_ARCH_FILE="$APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE

reset_logs
CASE_UID=1000
export CASE_UID
if run_installer > "$TEST_ROOT/nonroot.out" 2> "$TEST_ROOT/nonroot.err"; then
	fail 'non-root installation was accepted'
fi
grep -Fq 'run this installer as root' "$TEST_ROOT/nonroot.err" ||
	fail 'non-root failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] || fail 'non-root run accessed the network'
[ ! -s "$LOG_ROOT/apk.log" ] || fail 'non-root run invoked apk'
assert_no_install_workdir

NOT_OPENWRT_RELEASE_FILE="$TEST_ROOT/not_openwrt_release"
sed "s/DISTRIB_ID='OpenWrt'/DISTRIB_ID='NotOpenWrt'/" \
	"$RELEASE_FILE" > "$NOT_OPENWRT_RELEASE_FILE"
reset_logs
CASE_UID=0 CASE_RELEASE_FILE="$NOT_OPENWRT_RELEASE_FILE"
export CASE_UID CASE_RELEASE_FILE
if run_installer > "$TEST_ROOT/not-openwrt.out" 2> "$TEST_ROOT/not-openwrt.err"; then
	fail 'non-OpenWrt system was accepted'
fi
grep -Fq 'only supports OpenWrt' "$TEST_ROOT/not-openwrt.err" ||
	fail 'non-OpenWrt failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] || fail 'non-OpenWrt run accessed the network'
[ ! -s "$LOG_ROOT/apk.log" ] || fail 'non-OpenWrt run invoked apk'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
export CASE_RELEASE_FILE

FUTURE_RELEASE_FILE="$TEST_ROOT/openwrt_release.future"
sed "s/DISTRIB_RELEASE='25.12.5'/DISTRIB_RELEASE='26.0.0'/" \
	"$RELEASE_FILE" > "$FUTURE_RELEASE_FILE"
reset_logs
CASE_RELEASE_FILE="$FUTURE_RELEASE_FILE"
export CASE_RELEASE_FILE
if run_installer > "$TEST_ROOT/future.out" 2> "$TEST_ROOT/future.err"; then
	fail 'unsupported future OpenWrt release was accepted'
fi
grep -Fq 'prebuilt APKs support OpenWrt 25.12.x only' "$TEST_ROOT/future.err" ||
	fail 'unsupported OpenWrt release failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] || fail 'unsupported OpenWrt release accessed the network'
[ ! -s "$LOG_ROOT/apk.log" ] || fail 'unsupported OpenWrt release invoked apk'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
export CASE_RELEASE_FILE

OPKG_BIN="$TEST_ROOT/opkg-bin"
mkdir -p "$OPKG_BIN"
cp "$MOCK_BIN/id" "$OPKG_BIN/id"
cat > "$OPKG_BIN/opkg" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "$OPKG_BIN/id" "$OPKG_BIN/opkg"
reset_logs
CASE_PATH="$OPKG_BIN"
export CASE_PATH
if run_installer > "$TEST_ROOT/opkg.out" 2> "$TEST_ROOT/opkg.err"; then
	fail 'opkg-based OpenWrt was accepted'
fi
grep -Fq 'prebuilt releases require OpenWrt 25.12 with apk' "$TEST_ROOT/opkg.err" ||
	fail 'opkg failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] || fail 'opkg run accessed the network'
[ ! -s "$LOG_ROOT/apk.log" ] || fail 'opkg run invoked apk'
assert_no_install_workdir
CASE_PATH="$MOCK_BIN:$PATH"
export CASE_PATH

reset_logs
CASE_UID=0
UNSUPPORTED_RELEASE_FILE="$TEST_ROOT/openwrt_release.unsupported"
UNSUPPORTED_APK_ARCH_FILE="$TEST_ROOT/apk_arch.unsupported"
sed "s/DISTRIB_ARCH='x86_64'/DISTRIB_ARCH='unsupported_abi'/" \
	"$RELEASE_FILE" > "$UNSUPPORTED_RELEASE_FILE"
printf '%s\n' 'unsupported_abi' > "$UNSUPPORTED_APK_ARCH_FILE"
CASE_RELEASE_FILE="$UNSUPPORTED_RELEASE_FILE"
CASE_APK_ARCH_FILE="$UNSUPPORTED_APK_ARCH_FILE"
export CASE_UID CASE_RELEASE_FILE CASE_APK_ARCH_FILE
if run_installer > "$TEST_ROOT/unsupported.out" 2> "$TEST_ROOT/unsupported.err"; then
	fail 'unsupported ABI was accepted'
fi
grep -Fq 'no unique native APK for architecture unsupported_abi' "$TEST_ROOT/unsupported.err" ||
	fail 'unsupported ABI failure was not explained'
[ "$(wc -l < "$LOG_ROOT/fetch.log")" -eq 1 ] ||
	fail 'unsupported ABI downloaded more than the checksum manifest'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'unsupported ABI reached package verification or installation'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
CASE_APK_ARCH_FILE="$APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE

reset_logs
CASE_APK_VERSION='apk-tools 2.14.6, test build'
export CASE_APK_VERSION
if run_installer > "$TEST_ROOT/apk-v2.out" 2> "$TEST_ROOT/apk-v2.err"; then
	fail 'apk-tools v2 was accepted'
fi
grep -Fq 'apk-tools v3 is required' "$TEST_ROOT/apk-v2.err" ||
	fail 'apk-tools v2 failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] || fail 'apk-tools v2 run accessed the network'
assert_no_install_workdir
CASE_APK_VERSION='apk-tools 3.0.0, test build'
export CASE_APK_VERSION

UNSAFE_RELEASE_FILE="$TEST_ROOT/openwrt_release.unsafe"
UNSAFE_APK_ARCH_FILE="$TEST_ROOT/apk_arch.unsafe"
sed "s/DISTRIB_ARCH='x86_64'/DISTRIB_ARCH='bad;abi'/" \
	"$RELEASE_FILE" > "$UNSAFE_RELEASE_FILE"
printf '%s\n' 'bad;abi' > "$UNSAFE_APK_ARCH_FILE"
reset_logs
CASE_RELEASE_FILE="$UNSAFE_RELEASE_FILE"
CASE_APK_ARCH_FILE="$UNSAFE_APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE
if run_installer > "$TEST_ROOT/unsafe-abi.out" 2> "$TEST_ROOT/unsafe-abi.err"; then
	fail 'unsafe OpenWrt package architecture was accepted'
fi
grep -Fq 'unsafe package architecture' "$TEST_ROOT/unsafe-abi.err" ||
	fail 'unsafe OpenWrt package architecture failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] ||
	fail 'unsafe OpenWrt package architecture accessed the network'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
CASE_APK_ARCH_FILE="$APK_ARCH_FILE"
export CASE_RELEASE_FILE CASE_APK_ARCH_FILE

MISSING_ARCH_RELEASE_FILE="$TEST_ROOT/openwrt_release.missing-arch"
sed "/^DISTRIB_ARCH=/d" "$RELEASE_FILE" > "$MISSING_ARCH_RELEASE_FILE"
reset_logs
CASE_RELEASE_FILE="$MISSING_ARCH_RELEASE_FILE"
export CASE_RELEASE_FILE
if run_installer > "$TEST_ROOT/missing-abi.out" 2> "$TEST_ROOT/missing-abi.err"; then
	fail 'missing OpenWrt package architecture was accepted'
fi
grep -Fq 'does not declare a package architecture' "$TEST_ROOT/missing-abi.err" ||
	fail 'missing OpenWrt package architecture failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] ||
	fail 'missing OpenWrt package architecture accessed the network'
assert_no_install_workdir
CASE_RELEASE_FILE="$RELEASE_FILE"
export CASE_RELEASE_FILE

reset_logs
CASE_APK_ARCH_FILE="$TEST_ROOT/apk_arch.missing"
export CASE_APK_ARCH_FILE
if run_installer > "$TEST_ROOT/missing-apk-arch.out" \
	2> "$TEST_ROOT/missing-apk-arch.err"; then
	fail 'missing apk package architecture metadata was accepted'
fi
grep -Fq 'apk package architecture metadata is missing' \
	"$TEST_ROOT/missing-apk-arch.err" ||
	fail 'missing apk package architecture metadata failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] ||
	fail 'missing apk package architecture metadata accessed the network'
assert_no_install_workdir
CASE_APK_ARCH_FILE="$APK_ARCH_FILE"
export CASE_APK_ARCH_FILE

EMPTY_APK_ARCH_FILE="$TEST_ROOT/apk_arch.empty"
: > "$EMPTY_APK_ARCH_FILE"
reset_logs
CASE_APK_ARCH_FILE="$EMPTY_APK_ARCH_FILE"
export CASE_APK_ARCH_FILE
if run_installer > "$TEST_ROOT/empty-apk-arch.out" \
	2> "$TEST_ROOT/empty-apk-arch.err"; then
	fail 'empty apk package architecture metadata was accepted'
fi
grep -Fq 'apk package architecture metadata is empty' \
	"$TEST_ROOT/empty-apk-arch.err" ||
	fail 'empty apk package architecture metadata failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] ||
	fail 'empty apk package architecture metadata accessed the network'
assert_no_install_workdir

UNSAFE_APK_ARCH_ONLY_FILE="$TEST_ROOT/apk_arch.unsafe-only"
printf '%s\n' 'bad;abi' > "$UNSAFE_APK_ARCH_ONLY_FILE"
reset_logs
CASE_APK_ARCH_FILE="$UNSAFE_APK_ARCH_ONLY_FILE"
export CASE_APK_ARCH_FILE
if run_installer > "$TEST_ROOT/unsafe-apk-arch.out" \
	2> "$TEST_ROOT/unsafe-apk-arch.err"; then
	fail 'unsafe apk package architecture metadata was accepted'
fi
grep -Fq 'apk package architecture metadata is unsafe' \
	"$TEST_ROOT/unsafe-apk-arch.err" ||
	fail 'unsafe apk package architecture metadata failure was not explained'
[ ! -s "$LOG_ROOT/fetch.log" ] ||
	fail 'unsafe apk package architecture metadata accessed the network'
assert_no_install_workdir
CASE_APK_ARCH_FILE="$APK_ARCH_FILE"
export CASE_APK_ARCH_FILE

write_fixture '1.2.3-r4' 'x86_64'
native_line="$(grep '  nes-emulator-' "$FIXTURES/SHA256SUMS")"
printf '%s\n' "$native_line" >> "$FIXTURES/SHA256SUMS"
reset_logs
if run_installer > "$TEST_ROOT/duplicate.out" 2> "$TEST_ROOT/duplicate.err"; then
	fail 'duplicate native checksum entries were accepted'
fi
grep -Fq 'no unique native APK for architecture x86_64' \
	"$TEST_ROOT/duplicate.err" ||
	fail 'duplicate checksum failure was not explained'
[ "$(wc -l < "$LOG_ROOT/fetch.log")" -eq 1 ] ||
	fail 'duplicate checksum entries caused package downloads'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'duplicate checksum entries reached package verification or installation'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
rm -f -- "$FIXTURES/nes-emulator-1.2.3-r4-x86_64.apk"
reset_logs
if run_installer > "$TEST_ROOT/missing-asset.out" 2> "$TEST_ROOT/missing-asset.err"; then
	fail 'missing APK asset was accepted'
fi
grep -Fq 'download failed or exceeded the safe size limit' \
	"$TEST_ROOT/missing-asset.err" || fail 'missing APK failure was not explained'
[ "$(grep -Fc '/nes-emulator-1.2.3-r4-x86_64.apk' "$LOG_ROOT/fetch.log")" -eq 3 ] ||
	fail 'missing APK was not retried exactly three times'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'missing APK reached package verification or installation'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
dd if=/dev/zero of="$FIXTURES/SHA256SUMS" bs=1048576 count=2 2>/dev/null
reset_logs
if run_installer > "$TEST_ROOT/large-manifest.out" 2> "$TEST_ROOT/large-manifest.err"; then
	fail 'oversized checksum manifest was accepted'
fi
grep -Fq 'download failed or exceeded the safe size limit' \
	"$TEST_ROOT/large-manifest.err" || fail 'oversized manifest failure was not explained'
[ "$(grep -Fc '/SHA256SUMS' "$LOG_ROOT/fetch.log")" -eq 3 ] ||
	fail 'oversized manifest was not bounded across three attempts'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'oversized manifest reached package verification or installation'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
dd if=/dev/zero of="$FIXTURES/nes-emulator-1.2.3-r4-x86_64.apk" \
	bs=1048576 count=9 2>/dev/null
reset_logs
if run_installer > "$TEST_ROOT/large-apk.out" 2> "$TEST_ROOT/large-apk.err"; then
	fail 'oversized APK asset was accepted'
fi
grep -Fq 'download failed or exceeded the safe size limit' \
	"$TEST_ROOT/large-apk.err" || fail 'oversized APK failure was not explained'
[ "$(grep -Fc '/nes-emulator-1.2.3-r4-x86_64.apk' "$LOG_ROOT/fetch.log")" -eq 3 ] ||
	fail 'oversized APK was not bounded across three attempts'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'oversized APK reached package verification or installation'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
printf 'corrupted after manifest generation\n' >> "$FIXTURES/nes-emulator-1.2.3-r4-x86_64.apk"
reset_logs
if run_installer > "$TEST_ROOT/checksum.out" 2> "$TEST_ROOT/checksum.err"; then
	fail 'corrupted APK was accepted'
fi
grep -Fq 'SHA-256 verification failed' "$TEST_ROOT/checksum.err" ||
	fail 'checksum failure was not explained'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'corrupted APK reached package verification or installation'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
mv "$FIXTURES/luci-app-nes-emulator-1.2.3-r4-x86_64.apk" \
	"$FIXTURES/luci-app-nes-emulator-1.2.3-r5-x86_64.apk"
luci_hash="$(sha256sum "$FIXTURES/luci-app-nes-emulator-1.2.3-r5-x86_64.apk" | awk 'NR == 1 { print $1 }')"
native_hash="$(sha256sum "$FIXTURES/nes-emulator-1.2.3-r4-x86_64.apk" | awk 'NR == 1 { print $1 }')"
{
	printf '%s  %s\n' "$luci_hash" 'luci-app-nes-emulator-1.2.3-r5-x86_64.apk'
	printf '%s  %s\n' "$native_hash" 'nes-emulator-1.2.3-r4-x86_64.apk'
} > "$FIXTURES/SHA256SUMS"
reset_logs
if run_installer > "$TEST_ROOT/mixed.out" 2> "$TEST_ROOT/mixed.err"; then
	fail 'mixed package revisions were accepted'
fi
grep -Fq 'native and LuCI package revisions do not match' "$TEST_ROOT/mixed.err" ||
	fail 'mixed revision failure was not explained'
[ "$(wc -l < "$LOG_ROOT/fetch.log")" -eq 1 ] ||
	fail 'mixed revisions downloaded package files'
! grep -Eq ' verify | add ' "$LOG_ROOT/apk.log" ||
	fail 'mixed revisions reached package verification or installation'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
reset_logs
CASE_VERIFY_FAIL=1
export CASE_VERIFY_FAIL
if run_installer > "$TEST_ROOT/verify-fail.out" 2> "$TEST_ROOT/verify-fail.err"; then
	fail 'invalid APK container was accepted'
fi
grep -Fq 'APK container verification failed' "$TEST_ROOT/verify-fail.err" ||
	fail 'APK container failure was not explained'
! grep -q '^--repositories-file /dev/null --no-network --no-cache --allow-untrusted --wait 120 add ' \
	"$LOG_ROOT/apk.log" ||
	fail 'invalid APK container reached package installation'
assert_no_install_workdir
CASE_VERIFY_FAIL=0
export CASE_VERIFY_FAIL

write_fixture '1.2.3-r4' 'x86_64'
reset_logs
CASE_DEPENDENCY_FAIL=1 CASE_ADD_FAIL=0
export CASE_DEPENDENCY_FAIL CASE_ADD_FAIL
if run_installer > "$TEST_ROOT/dependency-fail.out" 2> "$TEST_ROOT/dependency-fail.err"; then
	fail 'trusted dependency failure was ignored'
fi
[ "$(grep -c '^--update-cache --wait 120 add luci-base rpcd$' "$LOG_ROOT/apk.log")" -eq 1 ] ||
	fail 'failed dependency transaction was retried or skipped'
! grep -q '^--repositories-file /dev/null --no-network --no-cache --allow-untrusted --wait 120 add ' \
	"$LOG_ROOT/apk.log" || fail 'local packages were installed after dependency failure'
assert_no_install_workdir

write_fixture '1.2.3-r4' 'x86_64'
reset_logs
CASE_DEPENDENCY_FAIL=0
CASE_ADD_FAIL=1
export CASE_DEPENDENCY_FAIL CASE_ADD_FAIL
if run_installer > "$TEST_ROOT/add-fail.out" 2> "$TEST_ROOT/add-fail.err"; then
	fail 'apk transaction failure was ignored'
fi
[ "$(grep -c '^--repositories-file /dev/null --no-network --no-cache --allow-untrusted --wait 120 add ' "$LOG_ROOT/apk.log")" -eq 1 ] ||
	fail 'failed APK transaction was retried or skipped'
assert_no_install_workdir

printf 'installer contract: OK\n'
