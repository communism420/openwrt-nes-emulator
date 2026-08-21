#!/usr/bin/env bash
# Build reproducible OpenWrt 25.12+ APK feeds with FCEUmm linked into nesd.
set -euo pipefail
IFS=$'\n\t'
umask 022
export LC_ALL=C

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
OUT="${OUT:-$ROOT_DIR/dist/apk}"
WORK_ROOT="${WORK:-${TMPDIR:-/tmp}/openwrt-nes-emulator-build}"
CACHE_ROOT="${CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/openwrt-nes-emulator}"
FINAL_OUT=""
OUT_STAGE=""
OUT_STAGE_IDENTITY=""
OUT_STAGE_TREE_SHA256=""
OUT_PARENT=""
OUT_BASENAME=""
OUT_BACKUP=""
OUT_BACKUP_IDENTITY=""
OUT_BACKUP_POLICY=""
OUT_BACKUP_TREE_SHA256=""
OUT_DELETE=""
OUT_DELETE_IDENTITY=""
OUT_ORIGINAL_IDENTITY=""
OUT_ORIGINAL_POLICY=""
OUT_ORIGINAL_TREE_SHA256=""
OUT_HAD_PREVIOUS=0
PUBLISH_IN_PROGRESS=0
OUT_IS_DEFAULT=0
OUTPUT_MARKER_NAME=".openwrt-nes-emulator-output"
OUTPUT_MARKER_VALUE="openwrt-nes-emulator-output-v1"
PUBLISH_MOVE_ATTEMPTS=8
PUBLISH_MOVE_RETRY_DELAY=0.25

PROJECT_VERSION="1.0.0"
PACKAGE_RELEASE="19"
APK_VERSION="${PROJECT_VERSION}-r${PACKAGE_RELEASE}"
PROJECT_SOURCE_DATE_EPOCH="1787270400" # 2026-08-21T00:00:00Z
OPENWRT_ARCH_RELEASE="25.12"
OPENWRT_TOOLCHAIN_RELEASE="25.12.5"

FCEUMM_COMMIT="76f68314ce4213703174108f461c431001dcc204"
FCEUMM_SHORT_COMMIT="${FCEUMM_COMMIT:0:12}"
FCEUMM_COMMIT_DATE="2026-07-24"
FCEUMM_SOURCE_URL="https://codeload.github.com/libretro/libretro-fceumm/tar.gz/$FCEUMM_COMMIT"
FCEUMM_SOURCE_SHA256="b067ebd0a973751e9b5af56f5b54d74d0a6e67349549b392a4615d3f0d44f031"
FCEUMM_TREE_SHA256="35f34a615153a644b067b597d93115bc0d83cf61d97ee7596fe9c958e49586cd"
FCEUMM_STATE_PATCH_SHA256="5402c5d7c4df5a3c6ea3f721aa8abfc6f6bc5242ddb69b50a75a7b07ea492c3e"
FCEUMM_ROM_BUFFER_PATCH_SHA256="8bb6a69091796b5af3c3bce2a4d4b8bb33e6751a3019370660edcabb842cbf32"

ZIG="${ZIG:-$(command -v zig || true)}"
APK="${APK:-$(command -v apk-static || command -v apk || true)}"
ZSTD="${ZSTD:-$(command -v zstd || command -v unzstd || true)}"
JOBS="${JOBS:-}"
KEEP_WORK="${KEEP_WORK:-0}"
SIGNING_KEY="${SIGNING_KEY:-}"
SIGNING_PUBKEY="${SIGNING_PUBKEY:-}"
SIGNING_TRUST_DIR=""
FCEUMM_SRC="${FCEUMM_SRC:-}"
ARCHES="${ARCHES:-}"

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

safe_realpath() {
	realpath -m -- "$1"
}

validate_base_dir() {
	local label="$1" path resolved
	path="$2"
	resolved="$(safe_realpath "$path")"
	[[ "$resolved" != "/" ]] || die "$label must not be /"
	[[ "$resolved" != "$ROOT_DIR" ]] || die "$label must not be the repository root"
	if [[ "$label" == "OUT" ]]; then
		case "$resolved" in
			/bin | /boot | /dev | /etc | /home | /lib | /lib64 | \
				/media | /mnt | /opt | /proc | /root | /run | /sbin | \
				/srv | /sys | /tmp | /usr | /var)
				die "OUT must not be a broad system directory: $resolved"
				;;
		esac
	fi
	printf '%s\n' "$resolved"
}

output_has_valid_marker() {
	local directory marker
	directory="$1"
	marker="$directory/$OUTPUT_MARKER_NAME"
	[[ -f "$marker" && ! -L "$marker" ]] &&
		[[ "$(<"$marker")" == "$OUTPUT_MARKER_VALUE" ]]
}

directory_is_empty() {
	local first_entry
	if ! first_entry="$(
		find "$1" -mindepth 1 -maxdepth 1 -print -quit
	)"; then
		printf 'Cannot inspect output directory: %s\n' "$1" >&2
		return 1
	fi
	[[ -z "$first_entry" ]]
}

path_identity() {
	stat -Lc '%d:%i' -- "$1"
}

validate_output_target() {
	[[ -d "$FINAL_OUT" && ! -L "$FINAL_OUT" ]] ||
		die "OUT must be a real directory: $FINAL_OUT"
	if [[ "$OUT_IS_DEFAULT" == "1" ]]; then
		return
	fi
	directory_is_empty "$FINAL_OUT" && return
	output_has_valid_marker "$FINAL_OUT" ||
		die "refusing to replace non-empty OUT without a valid ownership marker: $FINAL_OUT"
}

validate_output_directory_state() {
	local directory="$1" expected_identity="$2" marker_policy="$3"
	local description="$4" expected_tree_sha256="${5:-}"
	local actual_identity actual_tree_sha256
	[[ -n "$expected_identity" ]] || {
		printf '%s has no recorded identity: %s\n' \
			"$description" "$directory" >&2
		return 1
	}
	[[ -d "$directory" && ! -L "$directory" ]] || {
		printf '%s is not the expected real directory: %s\n' \
			"$description" "$directory" >&2
		return 1
	}
	actual_identity="$(path_identity "$directory")" || {
		printf '%s identity cannot be read: %s\n' \
			"$description" "$directory" >&2
		return 1
	}
	[[ "$actual_identity" == "$expected_identity" ]] || {
		printf '%s identity changed: %s\n' "$description" "$directory" >&2
		return 1
	}
	case "$marker_policy" in
		required)
			output_has_valid_marker "$directory" || {
				printf '%s ownership marker changed: %s\n' \
					"$description" "$directory" >&2
				return 1
			}
			;;
		empty)
			directory_is_empty "$directory" || {
				printf '%s is no longer empty: %s\n' \
					"$description" "$directory" >&2
				return 1
			}
			;;
		legacy | owned)
			;;
		*)
			die "invalid output marker policy: $marker_policy"
			;;
	esac
	if [[ -n "$expected_tree_sha256" ]]; then
		actual_tree_sha256="$(output_tree_sha256 "$directory")" || {
			printf '%s contents cannot be fingerprinted: %s\n' \
				"$description" "$directory" >&2
			return 1
		}
		[[ "$actual_tree_sha256" == "$expected_tree_sha256" ]] || {
			printf '%s contents changed: %s\n' \
				"$description" "$directory" >&2
			return 1
		}
	fi
}

move_output_directory() {
	local source="$1" destination="$2" description="$3"
	local expected_identity="$4" marker_policy="$5"
	local expected_tree_sha256="${6:-}"
	local source_resolved destination_resolved attempt move_error move_status
	source_resolved="$(safe_realpath "$source")"
	destination_resolved="$(safe_realpath "$destination")"
	[[ "$source_resolved" != "$destination_resolved" ]] ||
		die "refusing to move an output directory onto itself: $source_resolved"
	case "$source_resolved" in
		"$FINAL_OUT" | \
		"$OUT_PARENT"/."$OUT_BASENAME".stage.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".backup.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".delete.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".delete.*/payload)
			;;
		*)
			die "refusing to move an unexpected output source: $source_resolved"
			;;
	esac
	case "$destination_resolved" in
		"$FINAL_OUT" | \
		"$OUT_PARENT"/."$OUT_BASENAME".stage.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".backup.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".delete.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".delete.*/payload)
			;;
		*)
			die "refusing to move to an unexpected output destination: $destination_resolved"
			;;
	esac
	for ((attempt = 1; attempt <= PUBLISH_MOVE_ATTEMPTS; attempt++)); do
		validate_output_directory_state "$source" "$expected_identity" \
			"$marker_policy" "$description source" \
			"$expected_tree_sha256" || return 1
		[[ ! -e "$destination" && ! -L "$destination" ]] || {
			printf '%s destination unexpectedly exists: %s\n' \
				"$description" "$destination" >&2
			return 1
		}
		move_status=0
		if move_error="$(mv -T -n -- "$source" "$destination" 2>&1)"; then
			move_status=0
		else
			move_status=$?
		fi
		if [[ ! -e "$source" && ! -L "$source" ]]; then
			if validate_output_directory_state "$destination" \
				"$expected_identity" "$marker_policy" \
				"$description destination" "$expected_tree_sha256"; then
				return 0
			fi
			printf '%s left an ambiguous destination state: %s\n' \
				"$description" "$destination" >&2
			return 1
		fi
		if [[ -e "$destination" || -L "$destination" ]]; then
			printf '%s left both source and destination present\n' \
				"$description" >&2
			return 1
		fi
		validate_output_directory_state "$source" "$expected_identity" \
			"$marker_policy" "$description source after failure" \
			"$expected_tree_sha256" ||
			return 1
		if [[ "$move_status" == "0" ]]; then
			move_error="mv reported success but the source remained in place"
		fi
		if ((attempt < PUBLISH_MOVE_ATTEMPTS)); then
			printf '%s failed (attempt %d/%d); retrying: %s\n' \
				"$description" "$attempt" "$PUBLISH_MOVE_ATTEMPTS" \
				"$move_error" >&2
			sleep "$PUBLISH_MOVE_RETRY_DELAY"
		fi
	done
	printf '%s failed after %d attempts: %s\n' \
		"$description" "$PUBLISH_MOVE_ATTEMPTS" "$move_error" >&2
	return 1
}

remove_output_directory() {
	local directory="$1" description="$2" expected_identity="$3"
	local marker_policy="$4"
	local expected_tree_sha256="${5:-}" retry_tree_sha256
	local directory_resolved quarantine payload quarantine_identity
	local payload_identity attempt remove_error=""
	directory_resolved="$(safe_realpath "$directory")"
	case "$directory_resolved" in
		"$OUT_PARENT"/."$OUT_BASENAME".stage.* | \
		"$OUT_PARENT"/."$OUT_BASENAME".backup.*)
			;;
		*)
			die "refusing to remove an unexpected output directory: $directory_resolved"
			;;
	esac
	validate_output_directory_state "$directory" "$expected_identity" \
		"$marker_policy" "$description target" "$expected_tree_sha256" ||
		return 1
	[[ -z "$OUT_DELETE" ]] ||
		die "an output deletion quarantine is already active: $OUT_DELETE"
	OUT_DELETE="$(
		mktemp -d "$OUT_PARENT/.${OUT_BASENAME}.delete.XXXXXXXXXX"
	)"
	OUT_DELETE_IDENTITY="$(path_identity "$OUT_DELETE")"
	chmod 0700 "$OUT_DELETE"
	quarantine="$OUT_DELETE"
	quarantine_identity="$OUT_DELETE_IDENTITY"
	payload="$quarantine/payload"
	if ! move_output_directory "$directory" "$payload" \
		"$description quarantine move" "$expected_identity" \
		"$marker_policy" "$expected_tree_sha256"; then
		if [[ -d "$payload" && ! -L "$payload" ]]; then
			payload_identity="$(path_identity "$payload")"
			if [[ ! -e "$directory" && ! -L "$directory" ]]; then
				if ! move_output_directory "$payload" "$directory" \
					"$description preserving a raced directory" \
					"$payload_identity" owned; then
					printf 'WARNING: preserving an unverified directory in quarantine: %s\n' \
						"$payload" >&2
					OUT_DELETE=""
					OUT_DELETE_IDENTITY=""
					return 1
				fi
			else
				printf 'WARNING: both the cleanup target and quarantined data exist; preserving both: %s\n' \
					"$payload" >&2
				OUT_DELETE=""
				OUT_DELETE_IDENTITY=""
				return 1
			fi
		elif [[ -e "$payload" || -L "$payload" ]]; then
			printf 'WARNING: preserving an unexpected quarantined entry: %s\n' \
				"$payload" >&2
			OUT_DELETE=""
			OUT_DELETE_IDENTITY=""
			return 1
		fi
		if validate_output_directory_state "$quarantine" \
			"$quarantine_identity" owned "$description empty quarantine" &&
			directory_is_empty "$quarantine"; then
			rmdir -- "$quarantine" || true
		fi
		OUT_DELETE=""
		OUT_DELETE_IDENTITY=""
		return 1
	fi
	retry_tree_sha256="$expected_tree_sha256"
	for ((attempt = 1; attempt <= PUBLISH_MOVE_ATTEMPTS; attempt++)); do
		validate_output_directory_state "$quarantine" \
			"$quarantine_identity" owned "$description quarantine" ||
			return 1
		validate_output_directory_state "$payload" "$expected_identity" \
			owned "$description quarantined target" \
			"$retry_tree_sha256" || return 1
		retry_tree_sha256=""
		if remove_error="$(
			rm -rf --one-file-system --preserve-root=all -- \
				"$quarantine" 2>&1
		)" &&
			[[ ! -e "$quarantine" && ! -L "$quarantine" ]]; then
			OUT_DELETE=""
			OUT_DELETE_IDENTITY=""
			return 0
		fi
		if [[ ! -e "$quarantine" && ! -L "$quarantine" ]]; then
			OUT_DELETE=""
			OUT_DELETE_IDENTITY=""
			return 0
		fi
		[[ -n "$remove_error" ]] ||
			remove_error="rm reported success but the directory still exists"
		if ((attempt < PUBLISH_MOVE_ATTEMPTS)); then
			printf '%s failed (attempt %d/%d); retrying: %s\n' \
				"$description" "$attempt" "$PUBLISH_MOVE_ATTEMPTS" \
				"$remove_error" >&2
			sleep "$PUBLISH_MOVE_RETRY_DELAY"
		fi
	done
	printf '%s failed after %d attempts: %s\n' \
		"$description" "$PUBLISH_MOVE_ATTEMPTS" "$remove_error" >&2
	printf 'WARNING: preserving the undeleted quarantine: %s\n' \
		"$quarantine" >&2
	OUT_DELETE=""
	OUT_DELETE_IDENTITY=""
	return 1
}

cleanup_output_delete_placeholder() {
	local delete_resolved
	[[ -n "$OUT_DELETE" && ( -e "$OUT_DELETE" || -L "$OUT_DELETE" ) ]] ||
		return 0
	delete_resolved="$(safe_realpath "$OUT_DELETE")"
	case "$delete_resolved" in
		"$OUT_PARENT"/."$OUT_BASENAME".delete.*)
			if [[ -n "$OUT_DELETE_IDENTITY" ]] &&
				validate_output_directory_state "$delete_resolved" \
					"$OUT_DELETE_IDENTITY" owned \
					"interrupted deletion quarantine" &&
				directory_is_empty "$delete_resolved"; then
				rmdir -- "$delete_resolved" ||
					printf 'WARNING: empty deletion quarantine cleanup failed: %s\n' \
						"$delete_resolved" >&2
			else
				printf 'WARNING: interrupted deletion data was preserved in quarantine: %s\n' \
					"$delete_resolved" >&2
			fi
			;;
		*)
			printf 'Refusing to clean unexpected deletion quarantine: %s\n' \
				"$delete_resolved" >&2
			;;
	esac
	OUT_DELETE=""
	OUT_DELETE_IDENTITY=""
}

cleanup_output_backup() {
	local backup_resolved
	[[ -n "$OUT_BACKUP" && -e "$OUT_BACKUP" ]] || return 0
	backup_resolved="$(safe_realpath "$OUT_BACKUP")"
	case "$backup_resolved" in
		"$OUT_PARENT"/."$OUT_BASENAME".backup.*)
			if ! remove_output_directory "$backup_resolved" \
				"removing the previous OUT backup" \
				"$OUT_BACKUP_IDENTITY" "$OUT_BACKUP_POLICY" \
				"$OUT_BACKUP_TREE_SHA256"; then
				printf 'WARNING: valid OUT was published, but its old backup remains: %s\n' \
					"$backup_resolved" >&2
			fi
			;;
		*)
			printf 'Refusing to clean unexpected output backup: %s\n' \
				"$backup_resolved" >&2
			;;
	esac
	OUT_BACKUP=""
	OUT_BACKUP_IDENTITY=""
	OUT_BACKUP_POLICY=""
	OUT_BACKUP_TREE_SHA256=""
}

rollback_output_publish() {
	local backup_policy backup_tree_sha256 stage_policy stage_tree_sha256
	[[ "$PUBLISH_IN_PROGRESS" == "1" ]] || return 0
	if [[ "$OUT_HAD_PREVIOUS" == "1" ]]; then
		if [[ -e "$OUT_BACKUP" || -L "$OUT_BACKUP" ]]; then
			backup_policy="$OUT_ORIGINAL_POLICY"
			backup_tree_sha256="$OUT_ORIGINAL_TREE_SHA256"
			if ! validate_output_directory_state "$OUT_BACKUP" \
				"$OUT_ORIGINAL_IDENTITY" "$OUT_ORIGINAL_POLICY" \
				"previous OUT backup" "$OUT_ORIGINAL_TREE_SHA256"; then
				validate_output_directory_state "$OUT_BACKUP" \
					"$OUT_ORIGINAL_IDENTITY" owned \
					"changed previous OUT backup" || return 1
				printf 'WARNING: previous OUT changed during publish; restoring its original directory identity without deleting it\n' >&2
				backup_policy=owned
				backup_tree_sha256=""
			fi
			if [[ -e "$FINAL_OUT" || -L "$FINAL_OUT" ]]; then
				[[ ! -e "$OUT_STAGE" && ! -L "$OUT_STAGE" ]] || {
					printf 'Cannot roll published OUT back over an existing stage\n' >&2
					return 1
				}
				stage_policy=required
				stage_tree_sha256="$OUT_STAGE_TREE_SHA256"
				if ! validate_output_directory_state "$FINAL_OUT" \
					"$OUT_STAGE_IDENTITY" required \
					"incomplete published OUT" "$OUT_STAGE_TREE_SHA256"; then
					validate_output_directory_state "$FINAL_OUT" \
						"$OUT_STAGE_IDENTITY" owned \
						"changed incomplete published OUT" || return 1
					printf 'WARNING: incomplete published OUT changed during rollback; preserving its directory identity in staging\n' >&2
					stage_policy=owned
					stage_tree_sha256=""
				fi
				move_output_directory "$FINAL_OUT" "$OUT_STAGE" \
					"moving the incomplete published OUT back to staging" \
					"$OUT_STAGE_IDENTITY" "$stage_policy" \
					"$stage_tree_sha256" || return 1
			fi
			move_output_directory "$OUT_BACKUP" "$FINAL_OUT" \
				"restoring the previous OUT" "$OUT_ORIGINAL_IDENTITY" \
				"$backup_policy" "$backup_tree_sha256" ||
				return 1
		else
			validate_output_directory_state "$FINAL_OUT" \
				"$OUT_ORIGINAL_IDENTITY" "$OUT_ORIGINAL_POLICY" \
				"unchanged previous OUT" || return 1
		fi
	elif [[ "$OUT_HAD_PREVIOUS" == "0" &&
		( -e "$FINAL_OUT" || -L "$FINAL_OUT" ) ]]; then
		[[ ! -e "$OUT_STAGE" && ! -L "$OUT_STAGE" ]] || {
			printf 'Cannot roll published OUT back over an existing stage\n' >&2
			return 1
		}
		stage_policy=required
		stage_tree_sha256="$OUT_STAGE_TREE_SHA256"
		if ! validate_output_directory_state "$FINAL_OUT" \
			"$OUT_STAGE_IDENTITY" required "incomplete published OUT" \
			"$OUT_STAGE_TREE_SHA256"; then
			validate_output_directory_state "$FINAL_OUT" \
				"$OUT_STAGE_IDENTITY" owned \
				"changed incomplete published OUT" || return 1
			printf 'WARNING: incomplete published OUT changed during rollback; preserving its directory identity in staging\n' >&2
			stage_policy=owned
			stage_tree_sha256=""
		fi
		move_output_directory "$FINAL_OUT" "$OUT_STAGE" \
			"moving the incomplete published OUT back to staging" \
			"$OUT_STAGE_IDENTITY" "$stage_policy" "$stage_tree_sha256" ||
			return 1
	fi
	OUT_BACKUP=""
	OUT_BACKUP_IDENTITY=""
	OUT_BACKUP_POLICY=""
	OUT_BACKUP_TREE_SHA256=""
	OUT_HAD_PREVIOUS=0
	PUBLISH_IN_PROGRESS=0
}

publish_output() {
	OUT_STAGE_TREE_SHA256="$(output_tree_sha256 "$OUT_STAGE")"
	validate_output_directory_state "$OUT_STAGE" "$OUT_STAGE_IDENTITY" \
		required "staged OUT" "$OUT_STAGE_TREE_SHA256" ||
		die "output staging directory changed"
	validate_output_target
	validate_output_directory_state "$FINAL_OUT" "$OUT_ORIGINAL_IDENTITY" \
		"$OUT_ORIGINAL_POLICY" "previous OUT" \
		"$OUT_ORIGINAL_TREE_SHA256" ||
		die "OUT changed while packages were being built"
	OUT_BACKUP="$(
		mktemp -d "$OUT_PARENT/.${OUT_BASENAME}.backup.XXXXXXXXXX"
	)"
	OUT_BACKUP_IDENTITY="$(path_identity "$OUT_BACKUP")"
	OUT_BACKUP_POLICY=owned
	OUT_BACKUP_TREE_SHA256="$(output_tree_sha256 "$OUT_BACKUP")"
	rmdir -- "$OUT_BACKUP"
	OUT_BACKUP_IDENTITY=""
	OUT_BACKUP_POLICY=""
	OUT_BACKUP_TREE_SHA256=""
	if [[ -e "$FINAL_OUT" || -L "$FINAL_OUT" ]]; then
		[[ -d "$FINAL_OUT" && ! -L "$FINAL_OUT" ]] ||
			die "refusing to replace non-directory OUT: $FINAL_OUT"
		OUT_HAD_PREVIOUS=1
	fi
	PUBLISH_IN_PROGRESS=1
	if [[ "$OUT_HAD_PREVIOUS" == "1" ]] &&
		! move_output_directory "$FINAL_OUT" "$OUT_BACKUP" \
			"moving the previous OUT aside" "$OUT_ORIGINAL_IDENTITY" \
			"$OUT_ORIGINAL_POLICY" "$OUT_ORIGINAL_TREE_SHA256"; then
		die "failed to move the previous OUT aside"
	fi
	OUT_BACKUP_IDENTITY="$OUT_ORIGINAL_IDENTITY"
	OUT_BACKUP_POLICY="$OUT_ORIGINAL_POLICY"
	OUT_BACKUP_TREE_SHA256="$OUT_ORIGINAL_TREE_SHA256"
	if ! move_output_directory "$OUT_STAGE" "$FINAL_OUT" \
		"publishing the staged OUT" "$OUT_STAGE_IDENTITY" required \
		"$OUT_STAGE_TREE_SHA256"; then
		die "failed to publish the staged output"
	fi
	PUBLISH_IN_PROGRESS=0
	OUT_STAGE=""
	OUT_STAGE_IDENTITY=""
	OUT_STAGE_TREE_SHA256=""
	cleanup_output_backup
	OUT_HAD_PREVIOUS=0
	OUT="$FINAL_OUT"
}

sha256_check() {
	local expected="$1" file="$2"
	printf '%s  %s\n' "$expected" "$file" | sha256sum --check --status
}

tree_sha256() {
	local directory="$1" non_regular
	(
		cd "$directory"
		if ! non_regular="$(
			find . -path '*/.git' -prune -o \
				! -type d ! -type f -print -quit
		)"; then
			printf 'cannot inspect source tree: %s\n' "$directory" >&2
			exit 1
		fi
		if [[ -n "$non_regular" ]]; then
			printf 'source tree contains a non-regular entry: %s\n' \
				"$directory/$non_regular" >&2
			exit 1
		fi
		find . -path './.git' -prune -o -type f -print0 |
			sort -z |
			xargs -0 sha256sum
	) | sha256sum | cut -d' ' -f1
}

output_tree_sha256() {
	local directory="$1" non_regular
	(
		cd "$directory"
		if ! non_regular="$(
			find . ! -type d ! -type f -print -quit
		)"; then
			printf 'cannot inspect output tree: %s\n' "$directory" >&2
			exit 1
		fi
		if [[ -n "$non_regular" ]]; then
			printf 'output tree contains a non-regular entry: %s\n' \
				"$directory/$non_regular" >&2
			exit 1
		fi
		{
			find . -type d -printf 'd %m %U %G %p\0' | sort -z
			find . -type f -printf 'f %m %U %G %s %p\0' | sort -z
			find . -type f -print0 | sort -z |
				xargs -0 -r sha256sum
		}
	) | sha256sum | cut -d' ' -f1
}

copy_clean_source_tree() {
	local source="$1" destination="$2"
	assert_regular_tree "$source"
	mkdir -p "$destination"
	(
		cd "$source"
		find . \
			\( -type d \( -name .git -o -name .build -o -name __pycache__ \) \
				-prune \) -o \
			\( -type f \( -name nesd -o -name '*.o' -o -name '*.d' -o \
				-name '*.so' -o -name '*.ipk' -o -name '*.apk' -o \
				-name '*.pyc' -o -name '*.pyo' -o -name '*.pyd' \) \) \
				-prune -o \
			-print0
	) |
		tar --null --no-recursion -C "$source" -T - -cf - |
		tar -C "$destination" -xf -
}

normalize_project_source_modes() {
	local directory="$1" source_file first_line
	find "$directory" -type d -exec chmod 0755 {} +
	find "$directory" -type f -exec chmod 0644 {} +
	while IFS= read -r -d '' source_file; do
		first_line=""
		IFS= read -r first_line < "$source_file" || true
		[[ "$first_line" == '#!'* ]] && chmod 0755 "$source_file"
	done < <(find "$directory" -type f -print0)
	return 0
}

assert_no_build_products() {
	local directory="$1" offender
	offender="$(
		find "$directory" \
			\( -type d \( -name .git -o -name .build -o \
				-name __pycache__ \) -o \
				-type f \( -name nesd -o -name '*.o' -o -name '*.d' -o \
				-name '*.so' -o -name '*.ipk' -o -name '*.apk' -o \
				-name '*.pyc' -o -name '*.pyo' -o -name '*.pyd' \) \) \
			-print -quit
	)"
	[[ -z "$offender" ]] ||
		die "build product leaked into source snapshot: $offender"
}

assert_regular_tree() {
	local directory="$1" non_regular
	non_regular="$(
		find "$directory" -path '*/.git' -prune -o \
			! -type d ! -type f -print -quit
	)"
	[[ -z "$non_regular" ]] ||
		die "source tree contains a non-regular entry: $non_regular"
}

download_to() {
	local url="$1" destination="$2"
	if command -v curl >/dev/null 2>&1; then
		curl --fail --location --retry 3 --output "$destination" "$url"
	elif command -v wget >/dev/null 2>&1; then
		wget --output-document="$destination" "$url"
	else
		die "curl or wget is required to download pinned build inputs"
	fi
}

validate_project_metadata() {
	local project_root="$1" package_makefile nes_package_makefile
	local package_license license_bytes
	for package_makefile in \
		"$project_root/package/nes-emulator/Makefile" \
		"$project_root/package/libretro-fceumm/Makefile" \
		"$project_root/package/luci-app-nes-emulator/Makefile"; do
		[[ "$PROJECT_VERSION" == \
			"$(sed -n 's/^PKG_VERSION:=//p' "$package_makefile")" ]] ||
			die "PROJECT_VERSION differs from $package_makefile"
		[[ "$PACKAGE_RELEASE" == \
			"$(sed -n 's/^PKG_RELEASE:=//p' "$package_makefile")" ]] ||
			die "PACKAGE_RELEASE differs from $package_makefile"
	done
	nes_package_makefile="$project_root/package/nes-emulator/Makefile"
	[[ "$FCEUMM_COMMIT" == \
		"$(sed -n 's/^FCEUMM_COMMIT:=//p' "$nes_package_makefile")" ]] ||
		die "FCEUMM_COMMIT differs from package/nes-emulator/Makefile"
	[[ "$FCEUMM_SHORT_COMMIT" == \
		"$(sed -n 's/^FCEUMM_SHORT_COMMIT:=//p' "$nes_package_makefile")" ]] ||
		die "FCEUMM_SHORT_COMMIT differs from package/nes-emulator/Makefile"
	[[ "$FCEUMM_COMMIT_DATE" == \
		"$(sed -n 's/^FCEUMM_SOURCE_DATE:=//p' "$nes_package_makefile")" ]] ||
		die "FCEUMM_COMMIT_DATE differs from package/nes-emulator/Makefile"
	[[ "$FCEUMM_SOURCE_SHA256" == \
		"$(sed -n 's/^PKG_HASH:=//p' "$nes_package_makefile")" ]] ||
		die "FCEUMM_SOURCE_SHA256 differs from package/nes-emulator/Makefile"
	[[ "$FCEUMM_STATE_PATCH_SHA256" == \
		"$(sed -n 's/^FCEUMM_STATE_PATCH_SHA256:=//p' \
			"$nes_package_makefile")" ]] ||
		die "FCEUMM_STATE_PATCH_SHA256 differs from package/nes-emulator/Makefile"
	[[ "$FCEUMM_ROM_BUFFER_PATCH_SHA256" == \
		"$(sed -n 's/^FCEUMM_ROM_BUFFER_PATCH_SHA256:=//p' \
			"$nes_package_makefile")" ]] ||
		die "FCEUMM_ROM_BUFFER_PATCH_SHA256 differs from package/nes-emulator/Makefile"
	[[ "$PROJECT_SOURCE_DATE_EPOCH" == \
		"$(sed -n 's/^PROJECT_SOURCE_DATE_EPOCH:=//p' \
			"$nes_package_makefile")" ]] ||
		die "PROJECT_SOURCE_DATE_EPOCH differs from package/nes-emulator/Makefile"
	license_bytes="$(wc -c < "$project_root/LICENSE")"
	for package_license in \
		"$project_root/package/nes-emulator/files/LICENSE-MIT" \
		"$project_root/package/luci-app-nes-emulator/files/LICENSE-MIT"; do
		head -c "$license_bytes" "$package_license" | \
			cmp -s "$project_root/LICENSE" - ||
			die "$package_license does not preserve the repository MIT license"
		if ! grep -Fq 'FCEUmm is mandatory in release builds' "$package_license" ||
			! grep -Fq 'combined' "$package_license" ||
			! grep -Fq 'GPL-2.0-only' "$package_license"; then
			die "$package_license does not explain the combined nesd license"
		fi
	done
}

run_publish_helpers_selftest() {
	local test_parent test_root remove_sentinel
	test_parent="$(safe_realpath "${TMPDIR:-/tmp}")"
	test_root="$(mktemp -d "$test_parent/openwrt-nes-publish-selftest.XXXXXXXXXX")"
	case "$(safe_realpath "$test_root")" in
		"$test_parent"/openwrt-nes-publish-selftest.*)
			;;
		*)
			die "self-test created an unexpected temporary path"
			;;
	esac
	TEST_PUBLISH_ROOT="$test_root"
	TEST_PUBLISH_PARENT="$test_parent"
	selftest_cleanup() {
		local resolved
		[[ -n "${TEST_PUBLISH_ROOT:-}" &&
			( -e "$TEST_PUBLISH_ROOT" || -L "$TEST_PUBLISH_ROOT" ) ]] ||
			return 0
		resolved="$(safe_realpath "$TEST_PUBLISH_ROOT")"
		case "$resolved" in
			"$TEST_PUBLISH_PARENT"/openwrt-nes-publish-selftest.*)
				command rm -rf --one-file-system --preserve-root=all -- \
					"$resolved"
				;;
			*)
				printf 'Refusing to clean unexpected self-test path: %s\n' \
					"$resolved" >&2
				;;
		esac
	}
	trap selftest_cleanup EXIT
	PUBLISH_MOVE_ATTEMPTS=2
	PUBLISH_MOVE_RETRY_DELAY=0.01

	selftest_context() {
		OUT_PARENT="$1"
		OUT_BASENAME=apk
		FINAL_OUT="$OUT_PARENT/$OUT_BASENAME"
		OUT_STAGE=""
		OUT_STAGE_IDENTITY=""
		OUT_STAGE_TREE_SHA256=""
		OUT_BACKUP=""
		OUT_BACKUP_IDENTITY=""
		OUT_BACKUP_POLICY=""
		OUT_BACKUP_TREE_SHA256=""
		OUT_DELETE=""
		OUT_DELETE_IDENTITY=""
		OUT_ORIGINAL_IDENTITY=""
		OUT_ORIGINAL_POLICY=""
		OUT_ORIGINAL_TREE_SHA256=""
		OUT_HAD_PREVIOUS=0
		PUBLISH_IN_PROGRESS=0
		OUT_IS_DEFAULT=0
		mkdir -p "$OUT_PARENT"
	}
	selftest_marker() {
		printf '%s\n' "$OUTPUT_MARKER_VALUE" > "$1/$OUTPUT_MARKER_NAME"
	}
	selftest_capture_original() {
		OUT_ORIGINAL_IDENTITY="$(path_identity "$FINAL_OUT")"
		OUT_ORIGINAL_POLICY=required
		OUT_ORIGINAL_TREE_SHA256="$(output_tree_sha256 "$FINAL_OUT")"
	}
	selftest_stage() {
		OUT_STAGE="$(
			mktemp -d "$OUT_PARENT/.${OUT_BASENAME}.stage.XXXXXXXXXX"
		)"
		OUT_STAGE_IDENTITY="$(path_identity "$OUT_STAGE")"
		selftest_marker "$OUT_STAGE"
		printf 'new\n' > "$OUT_STAGE/new"
		OUT_STAGE_TREE_SHA256="$(output_tree_sha256 "$OUT_STAGE")"
	}

	selftest_context "$test_root/normal"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	selftest_stage
	publish_output
	[[ -f "$FINAL_OUT/new" && ! -e "$FINAL_OUT/old" ]] ||
		die "normal publish self-test failed"
	[[ -z "$(find "$OUT_PARENT" -maxdepth 1 \
		\( -name '.apk.stage.*' -o -name '.apk.backup.*' -o \
			-name '.apk.delete.*' \) -print -quit)" ]] ||
		die "normal publish left a temporary output directory"

	selftest_context "$test_root/old-move-failure"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	selftest_stage
	mv() {
		local -a arguments=("$@")
		local source destination
		source="${arguments[${#arguments[@]} - 2]}"
		destination="${arguments[${#arguments[@]} - 1]}"
		if [[ "$source" == "$FINAL_OUT" &&
			"$destination" == "$OUT_PARENT"/.apk.backup.* ]]; then
			printf 'injected previous OUT move failure\n' >&2
			return 1
		fi
		command mv "$@"
	}
	if (
		trap rollback_output_publish EXIT
		publish_output
	); then
		die "persistent previous OUT move failure unexpectedly succeeded"
	fi
	unset -f mv
	[[ -f "$FINAL_OUT/old" && -f "$OUT_STAGE/new" ]] ||
		die "previous OUT move failure changed either snapshot"
	[[ -z "$(find "$OUT_PARENT" -maxdepth 1 \
		-name '.apk.backup.*' -print -quit)" ]] ||
		die "previous OUT move failure left its backup directory"

	selftest_context "$test_root/old-mutation"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	selftest_stage
	mv() {
		local -a arguments=("$@")
		local source destination
		source="${arguments[${#arguments[@]} - 2]}"
		destination="${arguments[${#arguments[@]} - 1]}"
		if [[ "$source" == "$FINAL_OUT" &&
			"$destination" == "$OUT_PARENT"/.apk.backup.* ]]; then
			command mv "$@"
			printf 'changed\n' > "$destination/changed"
			return 1
		fi
		command mv "$@"
	}
	if (
		trap rollback_output_publish EXIT
		publish_output
	); then
		die "mutated previous OUT unexpectedly published"
	fi
	unset -f mv
	[[ -f "$FINAL_OUT/old" && -f "$FINAL_OUT/changed" &&
		-f "$OUT_STAGE/new" ]] ||
		die "mutated previous OUT was not conservatively restored"

	selftest_context "$test_root/fail-after-rename"
	selftest_stage
	mv() {
		command mv "$@"
		return 1
	}
	move_output_directory "$OUT_STAGE" "$FINAL_OUT" \
		"self-test fail-after-rename" "$OUT_STAGE_IDENTITY" required \
		"$OUT_STAGE_TREE_SHA256" ||
		die "fail-after-rename was not recognized as a successful move"
	unset -f mv
	validate_output_directory_state "$FINAL_OUT" "$OUT_STAGE_IDENTITY" \
		required "fail-after-rename result" "$OUT_STAGE_TREE_SHA256" ||
		die "fail-after-rename produced the wrong destination"

	selftest_context "$test_root/destination-race"
	selftest_stage
	mv() {
		local -a arguments=("$@")
		local destination
		destination="${arguments[${#arguments[@]} - 1]}"
		if [[ "$destination" == "$FINAL_OUT" &&
			! -e "$destination" && ! -L "$destination" ]]; then
			mkdir "$destination"
		fi
		command mv "$@"
	}
	if move_output_directory "$OUT_STAGE" "$FINAL_OUT" \
		"self-test destination race" "$OUT_STAGE_IDENTITY" required \
		"$OUT_STAGE_TREE_SHA256"; then
		die "a raced move destination was overwritten"
	fi
	unset -f mv
	[[ -f "$OUT_STAGE/new" && -d "$FINAL_OUT" ]] ||
		die "destination race did not preserve both directories"

	selftest_context "$test_root/replacement"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	command rm -rf -- "$FINAL_OUT"
	mkdir "$FINAL_OUT"
	printf 'replacement\n' > "$FINAL_OUT/replacement"
	if move_output_directory "$FINAL_OUT" \
		"$OUT_PARENT/.apk.backup.replacement" \
		"self-test replacement" "$OUT_ORIGINAL_IDENTITY" required \
		"$OUT_ORIGINAL_TREE_SHA256"; then
		die "replacement output directory passed the identity check"
	fi
	[[ -f "$FINAL_OUT/replacement" &&
		! -e "$OUT_PARENT/.apk.backup.replacement" ]] ||
		die "replacement rejection changed filesystem state"

	selftest_context "$test_root/remove-retry"
	OUT_STAGE="$(
		mktemp -d "$OUT_PARENT/.${OUT_BASENAME}.stage.XXXXXXXXXX"
	)"
	OUT_STAGE_IDENTITY="$(path_identity "$OUT_STAGE")"
	remove_sentinel="$OUT_PARENT/remove-failed-once"
	rm() {
		if [[ "$*" == *".apk.delete."* &&
			! -e "$remove_sentinel" ]]; then
			: > "$remove_sentinel"
			printf 'injected remove failure\n' >&2
			return 1
		fi
		command rm "$@"
	}
	remove_output_directory "$OUT_STAGE" "self-test staged cleanup" \
		"$OUT_STAGE_IDENTITY" owned ||
		die "owned unmarked stage could not be cleaned after a retry"
	unset -f rm
	[[ ! -e "$OUT_STAGE" ]] ||
		die "staged cleanup self-test left its directory"

	selftest_context "$test_root/remove-replacement"
	OUT_STAGE="$(
		mktemp -d "$OUT_PARENT/.${OUT_BASENAME}.stage.XXXXXXXXXX"
	)"
	OUT_STAGE_IDENTITY="$(path_identity "$OUT_STAGE")"
	printf 'generated\n' > "$OUT_STAGE/generated"
	remove_sentinel="$OUT_PARENT/remove-replaced-once"
	# Invoked indirectly by move_output_directory() during this fault injection.
	# shellcheck disable=SC2329
	mv() {
		local -a arguments=("$@")
		local source destination
		source="${arguments[${#arguments[@]} - 2]}"
		destination="${arguments[${#arguments[@]} - 1]}"
		if [[ "$source" == "$OUT_STAGE" &&
			"$destination" == "$OUT_PARENT"/.apk.delete.*/payload &&
			! -e "$remove_sentinel" ]]; then
			command mv -T -n -- "$source" "$OUT_PARENT/original-aside"
			mkdir "$source"
			printf 'replacement\n' > "$source/replacement"
			: > "$remove_sentinel"
			printf 'injected pre-quarantine replacement\n' >&2
		fi
		command mv "$@"
	}
	if remove_output_directory "$OUT_STAGE" \
		"self-test replacement cleanup" "$OUT_STAGE_IDENTITY" owned; then
		die "replacement directory passed the cleanup identity check"
	fi
	unset -f mv
	[[ -f "$OUT_STAGE/replacement" &&
		-f "$OUT_PARENT/original-aside/generated" ]] ||
		die "quarantine identity rejection lost raced directory data"

	selftest_context "$test_root/rollback"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	selftest_stage
	mv() {
		local -a arguments=("$@")
		local source destination
		source="${arguments[${#arguments[@]} - 2]}"
		destination="${arguments[${#arguments[@]} - 1]}"
		if [[ "$source" == "$OUT_STAGE" &&
			"$destination" == "$FINAL_OUT" ]]; then
			printf 'injected staged publish failure\n' >&2
			return 1
		fi
		command mv "$@"
	}
	if (
		trap rollback_output_publish EXIT
		publish_output
	); then
		die "persistent staged publish failure unexpectedly succeeded"
	fi
	unset -f mv
	[[ -f "$FINAL_OUT/old" && -f "$OUT_STAGE/new" ]] ||
		die "publish rollback did not restore both snapshots"
	[[ -z "$(find "$OUT_PARENT" -maxdepth 1 \
		-name '.apk.backup.*' -print -quit)" ]] ||
		die "publish rollback left its backup directory"

	selftest_context "$test_root/stage-mutation"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	selftest_stage
	mv() {
		local -a arguments=("$@")
		local source destination
		source="${arguments[${#arguments[@]} - 2]}"
		destination="${arguments[${#arguments[@]} - 1]}"
		if [[ "$source" == "$OUT_STAGE" &&
			"$destination" == "$FINAL_OUT" ]]; then
			command mv "$@"
			printf 'changed\n' > "$destination/changed"
			return 1
		fi
		command mv "$@"
	}
	if (
		trap rollback_output_publish EXIT
		publish_output
	); then
		die "mutated staged OUT unexpectedly published"
	fi
	unset -f mv
	[[ -f "$FINAL_OUT/old" && -f "$OUT_STAGE/new" &&
		-f "$OUT_STAGE/changed" ]] ||
		die "mutated staged OUT was not conservatively preserved"

	selftest_context "$test_root/content-change"
	mkdir "$FINAL_OUT"
	selftest_marker "$FINAL_OUT"
	printf 'old\n' > "$FINAL_OUT/old"
	selftest_capture_original
	selftest_stage
	printf 'changed\n' > "$FINAL_OUT/changed"
	if (publish_output); then
		die "concurrent OUT content change was not rejected"
	fi
	[[ -f "$FINAL_OUT/changed" && -f "$OUT_STAGE/new" ]] ||
		die "content-change rejection modified either snapshot"

	selftest_cleanup
	TEST_PUBLISH_ROOT=""
	trap - EXIT
	printf 'publish helper self-test: OK\n'
}

require_command realpath
require_command sha256sum
require_command sort
require_command find
require_command xargs
require_command stat
require_command mv
require_command rm
require_command mktemp
require_command rmdir
require_command sleep
require_command chmod

if [[ "$#" == "1" && "$1" == "--self-test-publish" ]]; then
	run_publish_helpers_selftest
	exit 0
fi
[[ "$#" == "0" ]] || die "unexpected command-line argument"

require_command tar
require_command gzip
require_command make
require_command file
require_command readelf
require_command cmp
require_command patch

[[ -n "$ZIG" && -x "$ZIG" ]] || die "set ZIG to an executable Zig compiler"
[[ -n "$APK" && -x "$APK" ]] || die "set APK to an apk-tools v3 executable"
APK_MKPKG_HELP="$("$APK" mkpkg --help 2>&1 || true)"
[[ "$APK_MKPKG_HELP" == *"apk mkpkg"* ]] ||
	die "APK must provide the apk-tools v3 mkpkg applet"
TAR_VERSION="$(tar --version 2>/dev/null || true)"
[[ "$TAR_VERSION" == *"GNU tar"* ]] ||
	die "GNU tar is required for the reproducible source bundle"

if [[ -z "$JOBS" ]]; then
	if command -v nproc >/dev/null 2>&1; then
		JOBS="$(nproc)"
	else
		JOBS="1"
	fi
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"

if [[ -n "$SIGNING_KEY" || -n "$SIGNING_PUBKEY" ]]; then
	[[ -n "$SIGNING_KEY" && -n "$SIGNING_PUBKEY" ]] ||
		die "set both SIGNING_KEY and SIGNING_PUBKEY, or neither"
	[[ -r "$SIGNING_KEY" ]] || die "cannot read SIGNING_KEY: $SIGNING_KEY"
	[[ -r "$SIGNING_PUBKEY" ]] || die "cannot read SIGNING_PUBKEY: $SIGNING_PUBKEY"
fi

WORK_ROOT="$(validate_base_dir WORK "$WORK_ROOT")"
CACHE_ROOT="$(validate_base_dir CACHE "$CACHE_ROOT")"
OUT="$(validate_base_dir OUT "$OUT")"
FINAL_OUT="$OUT"
OUT_PARENT="$(dirname "$FINAL_OUT")"
OUT_BASENAME="$(basename "$FINAL_OUT")"
mkdir -p "$WORK_ROOT" "$CACHE_ROOT" "$FINAL_OUT"
DEFAULT_OUT="$ROOT_DIR/dist/apk"
DEFAULT_OUT_REAL="$(safe_realpath "$DEFAULT_OUT")"
if [[ "$FINAL_OUT" == "$DEFAULT_OUT_REAL" &&
	! -L "$DEFAULT_OUT" &&
	"$(safe_realpath "$ROOT_DIR/dist")" == "$ROOT_DIR/dist" ]]; then
	OUT_IS_DEFAULT=1
fi
validate_output_target
OUT_ORIGINAL_IDENTITY="$(path_identity "$FINAL_OUT")"
if output_has_valid_marker "$FINAL_OUT"; then
	OUT_ORIGINAL_POLICY=required
elif directory_is_empty "$FINAL_OUT"; then
	OUT_ORIGINAL_POLICY=empty
elif [[ "$OUT_IS_DEFAULT" == "1" ]]; then
	OUT_ORIGINAL_POLICY=legacy
else
	die "cannot establish ownership policy for OUT: $FINAL_OUT"
fi
OUT_ORIGINAL_TREE_SHA256="$(output_tree_sha256 "$FINAL_OUT")"

case "$WORK_ROOT$CACHE_ROOT" in
	*[[:space:]]*)
		die "WORK and CACHE paths must not contain whitespace (GNU make limitation)"
		;;
esac

RUN_DIR="$(mktemp -d "$WORK_ROOT/run.XXXXXXXXXX")"
touch "$RUN_DIR/.openwrt-nes-emulator-owned"

cleanup() {
	local resolved stage_resolved rollback_ok=1
	rollback_output_publish || rollback_ok=0
	cleanup_output_delete_placeholder
	if [[ "$rollback_ok" == "1" && "$PUBLISH_IN_PROGRESS" == "0" ]]; then
		cleanup_output_backup
	fi
	if [[ -n "$OUT_STAGE" && -e "$OUT_STAGE" ]]; then
		stage_resolved="$(safe_realpath "$OUT_STAGE")"
		case "$stage_resolved" in
			"$OUT_PARENT"/."$OUT_BASENAME".stage.*)
				if [[ -n "$OUT_STAGE_IDENTITY" ]]; then
					remove_output_directory "$stage_resolved" \
						"removing the staged OUT" \
						"$OUT_STAGE_IDENTITY" owned \
						"$OUT_STAGE_TREE_SHA256" ||
						printf 'WARNING: staged OUT cleanup failed: %s\n' \
							"$stage_resolved" >&2
				elif directory_is_empty "$stage_resolved"; then
					rmdir -- "$stage_resolved" ||
						printf 'WARNING: empty staged OUT cleanup failed: %s\n' \
							"$stage_resolved" >&2
				else
					printf 'Refusing to clean unclaimed staged OUT: %s\n' \
						"$stage_resolved" >&2
				fi
				;;
			*)
				printf 'Refusing to clean unexpected output stage: %s\n' \
					"$stage_resolved" >&2
				;;
		esac
	fi
	[[ "$KEEP_WORK" == "1" ]] && {
		printf 'Work directory kept at %s\n' "$RUN_DIR"
		return
	}
	resolved="$(safe_realpath "$RUN_DIR")"
	case "$resolved" in
		"$WORK_ROOT"/run.*)
			[[ -f "$resolved/.openwrt-nes-emulator-owned" ]] &&
				rm -rf -- "$resolved"
			;;
		*)
			printf 'Refusing to clean unexpected work path: %s\n' "$resolved" >&2
			;;
	esac
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

OUT_STAGE="$(mktemp -d "$OUT_PARENT/.${OUT_BASENAME}.stage.XXXXXXXXXX")"
OUT_STAGE_IDENTITY="$(path_identity "$OUT_STAGE")"
chmod 0755 "$OUT_STAGE"
printf '%s\n' "$OUTPUT_MARKER_VALUE" > "$OUT_STAGE/$OUTPUT_MARKER_NAME"
chmod 0644 "$OUT_STAGE/$OUTPUT_MARKER_NAME"
OUT="$OUT_STAGE"

PROJECT_SNAPSHOT="$RUN_DIR/project-src"
mkdir -p \
	"$PROJECT_SNAPSHOT/.github" \
	"$PROJECT_SNAPSHOT/docs" \
	"$PROJECT_SNAPSHOT/package" \
	"$PROJECT_SNAPSHOT/scripts" \
	"$PROJECT_SNAPSHOT/tests"
copy_clean_source_tree "$ROOT_DIR/.github" "$PROJECT_SNAPSHOT/.github"
copy_clean_source_tree "$ROOT_DIR/docs" "$PROJECT_SNAPSHOT/docs"
copy_clean_source_tree \
	"$ROOT_DIR/package/nes-emulator" \
	"$PROJECT_SNAPSHOT/package/nes-emulator"
copy_clean_source_tree \
	"$ROOT_DIR/package/luci-app-nes-emulator" \
	"$PROJECT_SNAPSHOT/package/luci-app-nes-emulator"
copy_clean_source_tree \
	"$ROOT_DIR/package/libretro-fceumm" \
	"$PROJECT_SNAPSHOT/package/libretro-fceumm"
copy_clean_source_tree "$ROOT_DIR/scripts" "$PROJECT_SNAPSHOT/scripts"
copy_clean_source_tree "$ROOT_DIR/tests" "$PROJECT_SNAPSHOT/tests"
cp -a \
	"$ROOT_DIR/.gitattributes" \
	"$ROOT_DIR/.gitignore" \
	"$ROOT_DIR/CHANGELOG.md" \
	"$ROOT_DIR/CODE_OF_CONDUCT.md" \
	"$ROOT_DIR/CONTRIBUTING.md" \
	"$ROOT_DIR/LICENSE" \
	"$ROOT_DIR/README.md" \
	"$ROOT_DIR/SECURITY.md" \
	"$ROOT_DIR/THIRD_PARTY_NOTICES.md" \
	"$ROOT_DIR/feeds.conf.example" \
	"$PROJECT_SNAPSHOT/"
normalize_project_source_modes "$PROJECT_SNAPSHOT"
assert_no_build_products "$PROJECT_SNAPSHOT"
assert_regular_tree "$PROJECT_SNAPSHOT"
validate_project_metadata "$PROJECT_SNAPSHOT"
NES_PACKAGE_DIR="$PROJECT_SNAPSHOT/package/nes-emulator"
HOST_SRC_DIR="$PROJECT_SNAPSHOT/package/nes-emulator/src"
LUCI_DIR="$PROJECT_SNAPSHOT/package/luci-app-nes-emulator"

if [[ -n "$SIGNING_KEY" ]]; then
	require_command openssl
	OPENSSL="$(command -v openssl)"
	signing_public_basename="$(basename "$SIGNING_PUBKEY")"
	SIGNING_TRUST_DIR="$RUN_DIR/signing/trusted"
	SIGNING_PRIVATE_DIR="$RUN_DIR/signing/private"
	mkdir -p "$SIGNING_TRUST_DIR" "$SIGNING_PRIVATE_DIR"
	install -m 0600 "$SIGNING_KEY" "$SIGNING_PRIVATE_DIR/key.pem"
	install -m 0644 "$SIGNING_PUBKEY" \
		"$SIGNING_TRUST_DIR/$signing_public_basename"
	SIGNING_KEY="$SIGNING_PRIVATE_DIR/key.pem"
	SIGNING_PUBKEY="$SIGNING_TRUST_DIR/$signing_public_basename"
	"$OPENSSL" pkey -in "$SIGNING_KEY" -pubout -outform DER \
		-out "$RUN_DIR/signing/derived-public.der" >/dev/null 2>&1 ||
		die "cannot derive a public key from SIGNING_KEY"
	"$OPENSSL" pkey -pubin -in "$SIGNING_PUBKEY" -pubout -outform DER \
		-out "$RUN_DIR/signing/supplied-public.der" >/dev/null 2>&1 ||
		die "cannot parse SIGNING_PUBKEY"
	cmp -s \
		"$RUN_DIR/signing/derived-public.der" \
		"$RUN_DIR/signing/supplied-public.der" ||
		die "SIGNING_KEY and SIGNING_PUBKEY do not form a key pair"
fi

if [[ "$(id -u)" -eq 0 ]]; then
	ROOT_MODE="root"
elif command -v unshare >/dev/null 2>&1 &&
	unshare --user --map-root-user -- true >/dev/null 2>&1; then
	ROOT_MODE="userns"
else
	die "root or working unprivileged user namespaces are required so APK payload owners are root:root"
fi

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$PROJECT_SOURCE_DATE_EPOCH}"
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] ||
	die "SOURCE_DATE_EPOCH must be a non-negative integer"

FCEUMM_TREE="$RUN_DIR/libretro-fceumm"
mkdir -p "$FCEUMM_TREE"

if [[ -n "$FCEUMM_SRC" ]]; then
	FCEUMM_SRC="$(safe_realpath "$FCEUMM_SRC")"
	if [[ -d "$FCEUMM_SRC/.git" || -f "$FCEUMM_SRC/.git" ]]; then
		require_command git
		resolved_commit="$(
			git -C "$FCEUMM_SRC" rev-parse --verify "$FCEUMM_COMMIT^{commit}"
		)"
		[[ "$resolved_commit" == "$FCEUMM_COMMIT" ]] ||
			die "FCEUMM_SRC does not contain the pinned commit"
		git -C "$FCEUMM_SRC" archive --format=tar "$FCEUMM_COMMIT" |
			tar -xf - -C "$FCEUMM_TREE"
	else
		[[ -d "$FCEUMM_SRC" ]] ||
			die "FCEUMM_SRC is not a directory"
		[[ "$(tree_sha256 "$FCEUMM_SRC")" == "$FCEUMM_TREE_SHA256" ]] ||
			die "non-Git FCEUMM_SRC does not match the pinned source tree"
		cp -a "$FCEUMM_SRC/." "$FCEUMM_TREE/"
	fi
else
	FCEUMM_CACHE_DIR="$CACHE_ROOT/sources"
	FCEUMM_ARCHIVE="$FCEUMM_CACHE_DIR/libretro-fceumm-$FCEUMM_COMMIT.tar.gz"
	mkdir -p "$FCEUMM_CACHE_DIR"
	if [[ -f "$FCEUMM_ARCHIVE" ]] &&
		! sha256_check "$FCEUMM_SOURCE_SHA256" "$FCEUMM_ARCHIVE"; then
		mv -- "$FCEUMM_ARCHIVE" \
			"$FCEUMM_ARCHIVE.invalid.$(date -u +%Y%m%dT%H%M%SZ)"
	fi
	if [[ ! -f "$FCEUMM_ARCHIVE" ]]; then
		TEMP_ARCHIVE="$RUN_DIR/fceumm-download.tar.gz"
		printf 'Downloading pinned FCEUmm %s...\n' "$FCEUMM_SHORT_COMMIT"
		download_to "$FCEUMM_SOURCE_URL" "$TEMP_ARCHIVE"
		sha256_check "$FCEUMM_SOURCE_SHA256" "$TEMP_ARCHIVE" ||
			die "FCEUmm source SHA-256 mismatch"
		mv -- "$TEMP_ARCHIVE" "$FCEUMM_ARCHIVE"
	fi
	sha256_check "$FCEUMM_SOURCE_SHA256" "$FCEUMM_ARCHIVE" ||
		die "cached FCEUmm source SHA-256 mismatch"
	tar -xzf "$FCEUMM_ARCHIVE" --strip-components=1 -C "$FCEUMM_TREE"
fi

for required in \
	Copying \
	Makefile.common \
	src/fceu.c \
	src/drivers/libretro/libretro.c
do
	[[ -f "$FCEUMM_TREE/$required" ]] ||
		die "pinned FCEUmm source is incomplete: $required is missing"
done
[[ "$(tree_sha256 "$FCEUMM_TREE")" == "$FCEUMM_TREE_SHA256" ]] ||
	die "extracted FCEUmm tree does not match the pinned commit"

# Preserve an independently verified pristine tree for the corresponding-source
# archive. The working tree below is patched in place for compilation; bundling
# that tree would make FCEUMM_SRC fail its pristine hash check and then attempt
# to apply the same patch a second time during a reproducibility rebuild.
FCEUMM_PRISTINE_TREE="$RUN_DIR/libretro-fceumm-pristine"
cp -a "$FCEUMM_TREE" "$FCEUMM_PRISTINE_TREE"
[[ "$(tree_sha256 "$FCEUMM_PRISTINE_TREE")" == "$FCEUMM_TREE_SHA256" ]] ||
	die "the preserved pristine FCEUmm tree changed unexpectedly"

FCEUMM_PATCH_DIR="$NES_PACKAGE_DIR/patches"
mapfile -d '' FCEUMM_PATCHES < <(
	find "$FCEUMM_PATCH_DIR" -maxdepth 1 -type f -name '*.patch' \
		-print0 | sort -z
)
[[ "${#FCEUMM_PATCHES[@]}" -gt 0 ]] ||
	die "the audited FCEUmm patch set is missing"
sha256_check "$FCEUMM_STATE_PATCH_SHA256" \
	"$FCEUMM_PATCH_DIR/001-propagate-savestate-parse-errors.patch" ||
	die "the FCEUmm savestate parser patch SHA-256 does not match"
sha256_check "$FCEUMM_ROM_BUFFER_PATCH_SHA256" \
	"$FCEUMM_PATCH_DIR/002-load-supplied-rom-buffer.patch" ||
	die "the FCEUmm immutable ROM buffer patch SHA-256 does not match"
for fceumm_patch in "${FCEUMM_PATCHES[@]}"; do
	patch --batch --forward -d "$FCEUMM_TREE" -p1 < "$fceumm_patch" ||
		die "cannot apply FCEUmm patch: $fceumm_patch"
done
FCEUMM_PATCHED_TREE_SHA256="$(tree_sha256 "$FCEUMM_TREE")"

HOST_BUILD_SRC="$RUN_DIR/nesd-src"
mkdir -p "$HOST_BUILD_SRC"
install -m 0644 \
	"$HOST_SRC_DIR/Makefile" \
	"$HOST_SRC_DIR/"*.c \
	"$HOST_SRC_DIR/"*.h \
	"$HOST_BUILD_SRC/"
assert_no_build_products "$HOST_BUILD_SRC"

ARCH_PROFILE_ROWS=(
	'aarch64_cortex-a53|zig|aarch64-linux-musl|-mcpu=cortex_a53|little|ELF64|AArch64|none'
	'aarch64_cortex-a72|zig|aarch64-linux-musl|-mcpu=cortex_a72|little|ELF64|AArch64|none'
	'aarch64_cortex-a76|zig|aarch64-linux-musl|-mcpu=cortex_a76|little|ELF64|AArch64|none'
	'aarch64_generic|zig|aarch64-linux-musl|-mcpu=generic|little|ELF64|AArch64|none'
	'arm_arm1176jzf-s_vfp|zig|arm-linux-musleabihf|-mcpu=arm1176jzf_s -mfpu=vfp -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_arm926ej-s|openwrt-gcc|arm-openwrt-linux-muslgnueabi|toolchain-default|little|ELF32|ARM|arm-soft'
	'arm_cortex-a15_neon-vfpv4|zig|arm-linux-musleabihf|-mcpu=cortex_a15 -mfpu=neon-vfpv4 -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_cortex-a5_vfpv4|zig|arm-linux-musleabihf|-mcpu=cortex_a5-neon -mfpu=vfpv4 -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_cortex-a7|zig|arm-linux-musleabi|-mcpu=baseline-neon-vfp4-vfp3-vfp2-fpregs-d32|little|ELF32|ARM|arm-soft'
	'arm_cortex-a7_neon-vfpv4|zig|arm-linux-musleabihf|-mcpu=cortex_a7 -mfpu=neon-vfpv4 -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_cortex-a7_vfpv4|zig|arm-linux-musleabihf|-mcpu=cortex_a7-neon -mfpu=vfpv4 -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_cortex-a8_vfpv3|zig|arm-linux-musleabihf|-mcpu=cortex_a8-neon -mfpu=vfpv3 -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_cortex-a9|zig|arm-linux-musleabi|-mcpu=baseline-neon-vfp4-vfp3-vfp2-fpregs-d32|little|ELF32|ARM|arm-soft'
	'arm_cortex-a9_neon|zig|arm-linux-musleabihf|-mcpu=cortex_a9 -mfpu=neon -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_cortex-a9_vfpv3-d16|zig|arm-linux-musleabihf|-mcpu=cortex_a9-neon-d32 -mfpu=vfpv3-d16 -mfloat-abi=hard|little|ELF32|ARM|arm-hard'
	'arm_fa526|openwrt-gcc|arm-openwrt-linux-muslgnueabi|toolchain-default|little|ELF32|ARM|arm-soft'
	'arm_xscale|openwrt-gcc|arm-openwrt-linux-muslgnueabi|toolchain-default|little|ELF32|ARM|arm-soft'
	'armeb_xscale|openwrt-gcc|armeb-openwrt-linux-muslgnueabi|toolchain-default|big|ELF32|ARM|arm-soft'
	'i386_pentium-mmx|zig|x86-linux-musl|-mcpu=pentium_mmx|little|ELF32|Intel_80386|none'
	'i386_pentium4|zig|x86-linux-musl|-mcpu=pentium4|little|ELF32|Intel_80386|none'
	'loongarch64_generic|zig|loongarch64-linux-musl|-mcpu=loongarch64|little|ELF64|LoongArch|loongarch-double'
	'mips64_mips64r2|openwrt-gcc|mips64-openwrt-linux-musl|toolchain-default|big|ELF64|MIPS|mips64r2-soft'
	'mips64_octeonplus|openwrt-gcc|mips64-openwrt-linux-musl|toolchain-default|big|ELF64|MIPS|mips64r2-octeon-soft'
	'mips64el_mips64r2|openwrt-gcc|mips64el-openwrt-linux-musl|toolchain-default|little|ELF64|MIPS|mips64r2-soft'
	'mips_24kc|zig|mips-linux-musleabi|-mcpu=mips32r2 -msoft-float -mno-mips16|big|ELF32|MIPS|mips32r2-soft'
	'mips_mips32|zig|mips-linux-musleabi|-mcpu=mips32 -msoft-float -mno-mips16|big|ELF32|MIPS|mips32r1-soft'
	'mipsel_24kc|zig|mipsel-linux-musleabi|-mcpu=mips32r2 -msoft-float -mno-mips16|little|ELF32|MIPS|mips32r2-soft'
	'mipsel_24kc_24kf|zig|mipsel-linux-musleabihf|-mcpu=mips32r2 -mno-mips16|little|ELF32|MIPS|mips32r2-hard'
	'mipsel_74kc|zig|mipsel-linux-musleabi|-mcpu=mips32r2 -msoft-float -mno-mips16|little|ELF32|MIPS|mips32r2-soft'
	'mipsel_mips32|zig|mipsel-linux-musleabi|-mcpu=mips32 -msoft-float -mno-mips16|little|ELF32|MIPS|mips32r1-soft'
	'powerpc64_e5500|zig|powerpc64-linux-musl|-mcpu=e5500|big|ELF64|PowerPC64|powerpc64-elfv2'
	'powerpc_464fp|openwrt-gcc|powerpc-openwrt-linux-musl|toolchain-default|big|ELF32|PowerPC|powerpc-hard'
	'powerpc_8548|openwrt-gcc|powerpc-openwrt-linux-musl|toolchain-default|big|ELF32|PowerPC|powerpc-soft'
	'riscv64_generic|zig|riscv64-linux-musl|-mcpu=baseline_rv64|little|ELF64|RISC_V|riscv-lp64d'
	'x86_64|zig|x86_64-linux-musl|-mcpu=baseline|little|ELF64|X86_64|none'
)

TOOLCHAIN_PROFILE_ROWS=(
	'arm_arm926ej-s|https://downloads.openwrt.org/releases/25.12.5/targets/mxs/generic/openwrt-toolchain-25.12.5-mxs-generic_gcc-14.3.0_musl_eabi.Linux-x86_64.tar.zst|fb7e916746fee657f49160edba668568dc26be7abf43532b5ad6882a53ef2331|arm-openwrt-linux-muslgnueabi-'
	'arm_fa526|https://downloads.openwrt.org/releases/25.12.5/targets/gemini/generic/openwrt-toolchain-25.12.5-gemini-generic_gcc-14.3.0_musl_eabi.Linux-x86_64.tar.zst|c12a251a57f7a45ca39b1814de1f4c1806ebe8d341090c73d28a16eb2901629f|arm-openwrt-linux-muslgnueabi-'
	'arm_xscale|https://downloads.openwrt.org/releases/25.12.5/targets/kirkwood/generic/openwrt-toolchain-25.12.5-kirkwood-generic_gcc-14.3.0_musl_eabi.Linux-x86_64.tar.zst|b9478f8179e296f6606180f80163ffccd5f127adda2d2cae22271f2fb9bbe707|arm-openwrt-linux-muslgnueabi-'
	'armeb_xscale|https://downloads.openwrt.org/releases/25.12.5/targets/ixp4xx/generic/openwrt-toolchain-25.12.5-ixp4xx-generic_gcc-14.3.0_musl.Linux-x86_64.tar.zst|12e7da4cebbb2f14bde1ad57575ba5388c5b53d16bb4efc7fb4cfd91176d49e6|armeb-openwrt-linux-muslgnueabi-'
	'mips64_mips64r2|https://downloads.openwrt.org/releases/25.12.5/targets/malta/be64/openwrt-toolchain-25.12.5-malta-be64_gcc-14.3.0_musl.Linux-x86_64.tar.zst|873a6da34e43fe1b538c2febafcfec8db4c1964fe279014d3af1ddbb3765bf61|mips64-openwrt-linux-musl-'
	'mips64_octeonplus|https://downloads.openwrt.org/releases/25.12.5/targets/octeon/generic/openwrt-toolchain-25.12.5-octeon-generic_gcc-14.3.0_musl.Linux-x86_64.tar.zst|a0e3f270cd52e00692dab69a6ebef4242ca7018269b73bebf3b3f9db7dc824c0|mips64-openwrt-linux-musl-'
	'mips64el_mips64r2|https://downloads.openwrt.org/releases/25.12.5/targets/malta/le64/openwrt-toolchain-25.12.5-malta-le64_gcc-14.3.0_musl.Linux-x86_64.tar.zst|2adff1d4cdf0d10bc01f6fff534cf31c139d085a08fdbc1d89ff58366f51a1ce|mips64el-openwrt-linux-musl-'
	'powerpc_464fp|https://downloads.openwrt.org/releases/25.12.5/targets/apm821xx/nand/openwrt-toolchain-25.12.5-apm821xx-nand_gcc-14.3.0_musl.Linux-x86_64.tar.zst|28e87f0d4238a89f2558638bb785d90bbf5933bbf74b3ce7c5d5c0c7b2af94d3|powerpc-openwrt-linux-musl-'
	'powerpc_8548|https://downloads.openwrt.org/releases/25.12.5/targets/mpc85xx/p1010/openwrt-toolchain-25.12.5-mpc85xx-p1010_gcc-14.3.0_musl.Linux-x86_64.tar.zst|05052e19da3a7689f779aa35e3197dd77340888bbd39b8bab00338f3a9c2430f|powerpc-openwrt-linux-musl-'
)

declare -A PROFILE_BUILDER=()
declare -A PROFILE_TARGET=()
declare -A PROFILE_FLAGS=()
declare -A PROFILE_ENDIAN=()
declare -A PROFILE_CLASS=()
declare -A PROFILE_MACHINE=()
declare -A PROFILE_ABI=()
declare -A TOOLCHAIN_URL=()
declare -A TOOLCHAIN_SHA256=()
declare -A TOOLCHAIN_PREFIX=()

for profile_row in "${ARCH_PROFILE_ROWS[@]}"; do
	IFS='|' read -r openwrt_arch builder target target_flags endian \
		elf_class elf_machine elf_abi <<< "$profile_row"
	[[ -n "$openwrt_arch" && -n "$builder" && -n "$target" &&
		-n "$target_flags" && -n "$endian" && -n "$elf_class" &&
		-n "$elf_machine" && -n "$elf_abi" ]] ||
		die "invalid architecture profile row: $profile_row"
	[[ -z "${PROFILE_BUILDER[$openwrt_arch]:-}" ]] ||
		die "duplicate architecture profile: $openwrt_arch"
	[[ "$builder" == "zig" || "$builder" == "openwrt-gcc" ]] ||
		die "invalid builder for $openwrt_arch: $builder"
	[[ "$endian" == "little" || "$endian" == "big" ]] ||
		die "invalid endian value for $openwrt_arch: $endian"
	[[ "$elf_class" == "ELF32" || "$elf_class" == "ELF64" ]] ||
		die "invalid ELF class for $openwrt_arch: $elf_class"
	PROFILE_BUILDER[$openwrt_arch]="$builder"
	PROFILE_TARGET[$openwrt_arch]="$target"
	PROFILE_FLAGS[$openwrt_arch]="$target_flags"
	PROFILE_ENDIAN[$openwrt_arch]="$endian"
	PROFILE_CLASS[$openwrt_arch]="$elf_class"
	PROFILE_MACHINE[$openwrt_arch]="$elf_machine"
	PROFILE_ABI[$openwrt_arch]="$elf_abi"
done
[[ "${#PROFILE_BUILDER[@]}" -eq 35 ]] ||
	die "the OpenWrt $OPENWRT_ARCH_RELEASE matrix must contain exactly 35 architectures"

for toolchain_row in "${TOOLCHAIN_PROFILE_ROWS[@]}"; do
	IFS='|' read -r openwrt_arch toolchain_url toolchain_sha256 \
		toolchain_prefix <<< "$toolchain_row"
	[[ -n "$openwrt_arch" && -n "$toolchain_url" &&
		"$toolchain_sha256" =~ ^[0-9a-f]{64}$ &&
		-n "$toolchain_prefix" ]] ||
		die "invalid OpenWrt toolchain row: $toolchain_row"
	[[ "${PROFILE_BUILDER[$openwrt_arch]:-}" == "openwrt-gcc" ]] ||
		die "toolchain metadata has no matching GCC profile: $openwrt_arch"
	[[ -z "${TOOLCHAIN_URL[$openwrt_arch]:-}" ]] ||
		die "duplicate toolchain metadata: $openwrt_arch"
	TOOLCHAIN_URL[$openwrt_arch]="$toolchain_url"
	TOOLCHAIN_SHA256[$openwrt_arch]="$toolchain_sha256"
	TOOLCHAIN_PREFIX[$openwrt_arch]="$toolchain_prefix"
done

for openwrt_arch in "${!PROFILE_BUILDER[@]}"; do
	if [[ "${PROFILE_BUILDER[$openwrt_arch]}" == "openwrt-gcc" ]]; then
		[[ -n "${TOOLCHAIN_URL[$openwrt_arch]:-}" ]] ||
			die "missing OpenWrt toolchain metadata: $openwrt_arch"
	else
		[[ -z "${TOOLCHAIN_URL[$openwrt_arch]:-}" ]] ||
			die "unexpected OpenWrt toolchain metadata: $openwrt_arch"
	fi
done

if [[ -n "$ARCHES" ]]; then
	mapfile -t OPENWRT_ARCHES < <(
		printf '%s\n' "$ARCHES" |
			tr ',' '\n' |
			tr ' ' '\n' |
			sed '/^$/d' |
			sort -u
	)
	for openwrt_arch in "${OPENWRT_ARCHES[@]}"; do
		[[ -n "${PROFILE_BUILDER[$openwrt_arch]:-}" ]] ||
			die "unsupported ARCHES entry: $openwrt_arch"
	done
else
	mapfile -t OPENWRT_ARCHES < <(
		printf '%s\n' "${!PROFILE_BUILDER[@]}" | sort
	)
fi
[[ "${#OPENWRT_ARCHES[@]}" -gt 0 ]] || die "ARCHES selected no targets"
ZIG_VERSION="$("$ZIG" version)"

toolchain_cache_is_valid() {
	local cache_dir="$1" arch="$2" marker
	marker="$cache_dir/.openwrt-toolchain-source"
	[[ -d "$cache_dir" && ! -L "$cache_dir" &&
		-f "$marker" && ! -L "$marker" ]] || return 1
	[[ "$(sed -n '1p' "$marker")" == \
		"release=$OPENWRT_TOOLCHAIN_RELEASE" ]] || return 1
	[[ "$(sed -n '2p' "$marker")" == \
		"url=${TOOLCHAIN_URL[$arch]}" ]] || return 1
	[[ "$(sed -n '3p' "$marker")" == \
		"sha256=${TOOLCHAIN_SHA256[$arch]}" ]] || return 1
	[[ -z "$(sed -n '4p' "$marker")" ]] || return 1
}

PREPARED_CC=""
PREPARED_AR=""
PREPARED_RANLIB=""
PREPARED_COMPILER_ID=""
PREPARED_STAGING_DIR=""

prepare_openwrt_toolchain() {
	local arch="$1" url sha256 prefix archive_name archive_dir archive
	local cache_parent cache_dir marker extract_dir tar_file member normalized
	local invalid_name expected_machine compiler_real cache_real bin_dir
	local toolchain_root
	local -a compiler_matches=()

	url="${TOOLCHAIN_URL[$arch]}"
	sha256="${TOOLCHAIN_SHA256[$arch]}"
	prefix="${TOOLCHAIN_PREFIX[$arch]}"
	archive_name="${url##*/}"
	archive_dir="$CACHE_ROOT/toolchains/archives"
	archive="$archive_dir/$archive_name"
	cache_parent="$CACHE_ROOT/toolchains/$OPENWRT_TOOLCHAIN_RELEASE"
	cache_dir="$cache_parent/$arch-$sha256"
	marker="$cache_dir/.openwrt-toolchain-source"
	mkdir -p "$archive_dir" "$cache_parent"

	[[ -n "$ZSTD" && -x "$ZSTD" ]] ||
		die "set ZSTD to a zstd executable for the pinned OpenWrt toolchains"
	if [[ -f "$archive" ]] && ! sha256_check "$sha256" "$archive"; then
		invalid_name="$archive.invalid.$(date -u +%Y%m%dT%H%M%SZ).$$"
		mv -- "$archive" "$invalid_name"
	fi
	if [[ ! -f "$archive" ]]; then
		printf 'Downloading pinned OpenWrt %s toolchain for %s...\n' \
			"$OPENWRT_TOOLCHAIN_RELEASE" "$arch"
		download_to "$url" "$RUN_DIR/$archive_name"
		sha256_check "$sha256" "$RUN_DIR/$archive_name" ||
			die "OpenWrt toolchain SHA-256 mismatch for $arch"
		mv -- "$RUN_DIR/$archive_name" "$archive"
	fi
	sha256_check "$sha256" "$archive" ||
		die "cached OpenWrt toolchain SHA-256 mismatch for $arch"

	if [[ -e "$cache_dir" || -L "$cache_dir" ]]; then
		if ! toolchain_cache_is_valid "$cache_dir" "$arch"; then
			invalid_name="$cache_dir.invalid.$(date -u +%Y%m%dT%H%M%SZ).$$"
			mv -T -- "$cache_dir" "$invalid_name"
		fi
	fi
	if [[ ! -d "$cache_dir" ]]; then
		extract_dir="$RUN_DIR/toolchain-$arch"
		tar_file="$RUN_DIR/toolchain-$arch.tar"
		mkdir -p "$extract_dir"
		"$ZSTD" -dc -- "$archive" > "$tar_file"
		while IFS= read -r member; do
			normalized="${member#./}"
			case "$normalized" in
				"" | /* | ../* | */../* | */..)
					die "unsafe path in OpenWrt toolchain archive for $arch: $member"
					;;
			esac
		done < <(tar -tf "$tar_file")
		tar --no-same-owner -xf "$tar_file" -C "$extract_dir"
		rm -f -- "$tar_file"
		{
			printf 'release=%s\n' "$OPENWRT_TOOLCHAIN_RELEASE"
			printf 'url=%s\n' "$url"
			printf 'sha256=%s\n' "$sha256"
		} > "$extract_dir/.openwrt-toolchain-source"
		if ! mv -T -- "$extract_dir" "$cache_dir"; then
			toolchain_cache_is_valid "$cache_dir" "$arch" ||
				die "concurrent toolchain cache creation failed for $arch"
		fi
	fi
	toolchain_cache_is_valid "$cache_dir" "$arch" ||
		die "invalid OpenWrt toolchain cache for $arch"

	mapfile -t compiler_matches < <(
		find "$cache_dir" \( -type f -o -type l \) \
			-path "*/bin/${prefix}gcc" -print
	)
	[[ "${#compiler_matches[@]}" -eq 1 ]] ||
		die "expected exactly one ${prefix}gcc in the $arch toolchain"
	PREPARED_CC="${compiler_matches[0]}"
	bin_dir="$(dirname "$PREPARED_CC")"
	PREPARED_AR="$bin_dir/${prefix}ar"
	PREPARED_RANLIB="$bin_dir/${prefix}ranlib"
	[[ -x "$PREPARED_CC" && -x "$PREPARED_AR" &&
		-x "$PREPARED_RANLIB" ]] ||
		die "OpenWrt toolchain executables are incomplete for $arch"
	cache_real="$(safe_realpath "$cache_dir")"
	compiler_real="$(safe_realpath "$PREPARED_CC")"
	case "$compiler_real" in
		"$cache_real"/*)
			;;
		*)
			die "OpenWrt compiler escapes its verified cache for $arch"
			;;
	esac
	toolchain_root="$(dirname "$bin_dir")"
	PREPARED_STAGING_DIR="$(dirname "$toolchain_root")"
	expected_machine="${prefix%-}"
	[[ "$(env STAGING_DIR="$PREPARED_STAGING_DIR" \
		"$PREPARED_CC" -dumpmachine)" == "$expected_machine" ]] ||
		die "OpenWrt compiler target mismatch for $arch"
	PREPARED_COMPILER_ID="$(
		env STAGING_DIR="$PREPARED_STAGING_DIR" \
			"$PREPARED_CC" --version | sed -n '1p'
	) (archive $sha256)"
}

declare -A CC_COMMAND=()
declare -A AR_COMMAND=()
declare -A RANLIB_COMMAND=()
declare -A COMPILER_ID=()
declare -A BUILD_LDLIBS=()

for openwrt_arch in "${OPENWRT_ARCHES[@]}"; do
	if [[ "${PROFILE_BUILDER[$openwrt_arch]}" == "zig" ]]; then
		target_flags="${PROFILE_FLAGS[$openwrt_arch]}"
		CC_COMMAND[$openwrt_arch]="$ZIG cc -target ${PROFILE_TARGET[$openwrt_arch]}"
		[[ "$target_flags" == "-" ]] ||
			CC_COMMAND[$openwrt_arch]+=" $target_flags"
		AR_COMMAND[$openwrt_arch]="$ZIG ar"
		RANLIB_COMMAND[$openwrt_arch]="$ZIG ranlib"
		COMPILER_ID[$openwrt_arch]="Zig $ZIG_VERSION"
		BUILD_LDLIBS[$openwrt_arch]="-ldl -lpthread -lm"
	else
		prepare_openwrt_toolchain "$openwrt_arch"
		CC_COMMAND[$openwrt_arch]="env STAGING_DIR=$PREPARED_STAGING_DIR $PREPARED_CC"
		AR_COMMAND[$openwrt_arch]="$PREPARED_AR"
		RANLIB_COMMAND[$openwrt_arch]="$PREPARED_RANLIB"
		COMPILER_ID[$openwrt_arch]="$PREPARED_COMPILER_ID"
		BUILD_LDLIBS[$openwrt_arch]="-ldl -lpthread -lm -latomic"
	fi
done

source_fingerprint() {
	local arch="$1"
	{
		printf '%s\n' \
			"apk-version=$APK_VERSION" \
			"source-date-epoch=$SOURCE_DATE_EPOCH" \
			"fceumm-commit=$FCEUMM_COMMIT" \
			"fceumm-sha256=$FCEUMM_SOURCE_SHA256" \
			"fceumm-patched-tree=$FCEUMM_PATCHED_TREE_SHA256" \
			"zig-version=$ZIG_VERSION" \
			"openwrt-arch=$arch" \
			"builder=${PROFILE_BUILDER[$arch]}" \
			"compiler-id=${COMPILER_ID[$arch]}" \
			"target=${PROFILE_TARGET[$arch]}" \
			"target-flags=${PROFILE_FLAGS[$arch]}" \
			"endian=${PROFILE_ENDIAN[$arch]}" \
			"elf-class=${PROFILE_CLASS[$arch]}" \
			"elf-machine=${PROFILE_MACHINE[$arch]}" \
			"elf-abi=${PROFILE_ABI[$arch]}" \
			"cc=${CC_COMMAND[$arch]}" \
			"ar=${AR_COMMAND[$arch]}" \
			"ranlib=${RANLIB_COMMAND[$arch]}" \
			"host-cflags=-O2 -Wall -Wextra -std=c11 -ffunction-sections -fdata-sections" \
			"fceumm-standard=gnu11" \
			"ldflags=-static -s -no-pie -Wl,--gc-sections,-z,stack-size=2097152" \
			"ldlibs=${BUILD_LDLIBS[$arch]}"
		(
			cd "$HOST_BUILD_SRC"
			find . -type f -print0 |
				sort -z |
				xargs -0 sha256sum
		)
	} | sha256sum | cut -d' ' -f1
}

declare -A BIN_CACHE=()

verify_nesd_elf() {
	local binary="$1" arch="$2" headers programs dynamic attributes
	local actual_class actual_data actual_machine file_description flags_lower
	local stack_size
	[[ -s "$binary" && -x "$binary" ]] || {
		printf 'ELF verification failed for %s: missing executable\n' "$arch" >&2
		return 1
	}
	headers="$(readelf -hW "$binary" 2>/dev/null)" || {
		printf 'ELF verification failed for %s: unreadable ELF header\n' \
			"$arch" >&2
		return 1
	}
	programs="$(readelf -lW "$binary" 2>/dev/null)" || return 1
	dynamic="$(readelf -dW "$binary" 2>/dev/null)" || return 1
	attributes="$(readelf -A "$binary" 2>/dev/null || true)"
	actual_class="$(
		sed -n 's/^[[:space:]]*Class:[[:space:]]*//p' <<< "$headers"
	)"
	actual_data="$(
		sed -n 's/^[[:space:]]*Data:[[:space:]]*//p' <<< "$headers"
	)"
	actual_machine="$(
		sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p' <<< "$headers"
	)"
	[[ "$actual_class" == "${PROFILE_CLASS[$arch]}" ]] || {
		printf 'ELF verification failed for %s: class %s, expected %s\n' \
			"$arch" "$actual_class" "${PROFILE_CLASS[$arch]}" >&2
		return 1
	}
	[[ "$actual_data" == *"${PROFILE_ENDIAN[$arch]} endian"* ]] || {
		printf 'ELF verification failed for %s: data encoding %s\n' \
			"$arch" "$actual_data" >&2
		return 1
	}
	case "${PROFILE_MACHINE[$arch]}" in
		AArch64 | ARM | LoongArch | PowerPC | PowerPC64)
			[[ "$actual_machine" == "${PROFILE_MACHINE[$arch]}" ]] ||
				return 1
			;;
		Intel_80386)
			[[ "$actual_machine" == "Intel 80386" ]] || return 1
			;;
		MIPS)
			[[ "$actual_machine" == *"MIPS"* ]] || return 1
			;;
		RISC_V)
			[[ "$actual_machine" == *"RISC-V"* ]] || return 1
			;;
		X86_64)
			[[ "$actual_machine" == *"X86-64"* ||
				"$actual_machine" == *"x86-64"* ]] || return 1
			;;
		*)
			printf 'ELF verification has no machine rule for %s\n' "$arch" >&2
			return 1
			;;
	esac
	[[ "$headers" == *"Type:"*"EXEC (Executable file)"* ]] || {
		printf 'ELF verification failed for %s: not an executable ELF\n' \
			"$arch" >&2
		return 1
	}
	! grep -Eq '[[:space:]]INTERP[[:space:]]' <<< "$programs" || {
		printf 'ELF verification failed for %s: PT_INTERP is present\n' \
			"$arch" >&2
		return 1
	}
	stack_size="$(
		awk '$1 == "GNU_STACK" { print $6; exit }' <<< "$programs"
	)"
	if [[ ! "$stack_size" =~ ^0x[0-9a-fA-F]+$ ]] ||
	   ! (( stack_size > 0 && stack_size <= 0x200000 )); then
		printf 'ELF verification failed for %s: unsafe GNU_STACK size %s\n' \
			"$arch" "${stack_size:-missing}" >&2
		return 1
	fi
	! grep -Eq '\(NEEDED\)' <<< "$dynamic" || {
		printf 'ELF verification failed for %s: DT_NEEDED is present\n' \
			"$arch" >&2
		return 1
	}
	flags_lower="$(tr '[:upper:]' '[:lower:]' <<< "$headers")"
	case "${PROFILE_ABI[$arch]}" in
		none)
			;;
		arm-hard)
			[[ "$flags_lower" == *"hard-float abi"* ]] || return 1
			;;
		arm-soft)
			[[ "$flags_lower" == *"soft-float abi"* ]] || return 1
			;;
		loongarch-double)
			[[ "$flags_lower" == *"double-float"* ]] || return 1
			;;
		mips32r1-soft)
			[[ "$attributes" == *"ISA: MIPS32"* &&
				"$attributes" != *"ISA: MIPS32r2"* &&
				"$attributes" == *"FP ABI: Soft float"* ]] || return 1
			;;
		mips32r2-soft)
			[[ "$attributes" == *"ISA: MIPS32r2"* &&
				"$attributes" == *"FP ABI: Soft float"* ]] || return 1
			;;
		mips32r2-hard)
			[[ "$attributes" == *"ISA: MIPS32r2"* &&
				"$attributes" == *"FP ABI: Hard float"* ]] || return 1
			;;
		mips64r2-soft | mips64r2-octeon-soft)
			[[ "$attributes" == *"ISA: MIPS64r2"* &&
				"$attributes" == *"FP ABI: Soft float"* ]] || return 1
			;;
		powerpc-hard)
			[[ "$attributes" == \
				*"Tag_GNU_Power_ABI_FP: hard float"* ]] || return 1
			;;
		powerpc-soft)
			[[ "$attributes" == \
				*"Tag_GNU_Power_ABI_FP: soft float"* ]] || return 1
			;;
		powerpc64-elfv2)
			[[ "$flags_lower" == *"abiv2"* ]] || return 1
			;;
		riscv-lp64d)
			[[ "$flags_lower" == *"double-float abi"* ]] || return 1
			;;
		*)
			printf 'ELF verification has no ABI rule for %s\n' "$arch" >&2
			return 1
			;;
	esac
	file_description="$(file -b "$binary")"
	[[ "$file_description" == *"statically linked"* ]] || {
		printf 'ELF verification failed for %s: not statically linked\n' \
			"$arch" >&2
		return 1
	}
}

build_nesd() {
	local arch="$1" fingerprint cache_dir cache_bin build_dir endian_flags
	local cache_hash_file cache_hash cache_is_valid file_description
	fingerprint="$(source_fingerprint "$arch")"
	cache_dir="$CACHE_ROOT/binaries/$arch/$fingerprint"
	cache_bin="$cache_dir/nesd"

	cache_hash_file="$cache_dir/nesd.sha256"
	cache_is_valid=0
	if [[ -s "$cache_bin" && -x "$cache_bin" && -s "$cache_hash_file" ]]; then
		cache_hash="$(sed -n '1p' "$cache_hash_file")"
		[[ "$cache_hash" =~ ^[0-9a-f]{64}$ ]] &&
			sha256_check "$cache_hash" "$cache_bin" &&
			verify_nesd_elf "$cache_bin" "$arch" &&
			cache_is_valid=1
	fi

	if [[ "$cache_is_valid" -ne 1 ]]; then
		build_dir="$RUN_DIR/build-$arch"
		endian_flags=""
		[[ "${PROFILE_ENDIAN[$arch]}" == "big" ]] &&
			endian_flags="-DMSB_FIRST"
		printf 'Compiling nesd + FCEUmm for %s (%s)...\n' \
			"$arch" "${PROFILE_BUILDER[$arch]}"
		make -j"$JOBS" -C "$HOST_BUILD_SRC" \
			BUILD_DIR="$build_dir/objects" \
			TARGET="$build_dir/nesd" \
			FCEUMM_DIR="$FCEUMM_TREE" \
			FCEUMM_GIT_VERSION="$FCEUMM_SHORT_COMMIT" \
			FCEUMM_ENDIAN_CPPFLAGS="$endian_flags" \
			CC="${CC_COMMAND[$arch]}" \
			AR="${AR_COMMAND[$arch]}" \
			RANLIB="${RANLIB_COMMAND[$arch]}" \
			CPPFLAGS="" \
			CFLAGS="-O2 -Wall -Wextra -std=c11 -ffunction-sections -fdata-sections" \
			LDFLAGS="-static -s -no-pie \
				-Wl,--gc-sections,-z,stack-size=2097152" \
			LDLIBS="${BUILD_LDLIBS[$arch]}"
		[[ -s "$build_dir/nesd" ]] || die "link produced no nesd for $arch"
		chmod 0755 "$build_dir/nesd"
		verify_nesd_elf "$build_dir/nesd" "$arch" ||
			die "linked nesd failed ELF/ABI verification for $arch"
		mkdir -p "$cache_dir"
		cp "$build_dir/nesd" "$cache_bin.tmp"
		chmod 0755 "$cache_bin.tmp"
		mv -f -- "$cache_bin.tmp" "$cache_bin"
		sha256sum "$cache_bin" | cut -d' ' -f1 > "$cache_hash_file.tmp"
		mv -f -- "$cache_hash_file.tmp" "$cache_hash_file"
	else
		printf 'Using verified-input and verified-ABI cache for %s.\n' "$arch"
	fi

	file_description="$(file -b "$cache_bin")"
	printf '%s: %s\n' "$cache_bin" "$file_description"
	verify_nesd_elf "$cache_bin" "$arch" ||
		die "cached nesd failed ELF/ABI verification for $arch"
	BIN_CACHE[$arch]="$cache_bin"
}

for openwrt_arch in "${OPENWRT_ARCHES[@]}"; do
	build_nesd "$openwrt_arch"
done

run_apk_as_root() {
	if [[ "$ROOT_MODE" == "root" ]]; then
		"$@"
	else
		unshare --user --map-root-user -- "$@"
	fi
}

verify_apk() {
	local package_file="$1"
	if [[ -n "$SIGNING_KEY" ]]; then
		"$APK" --keys-dir "$SIGNING_TRUST_DIR" verify "$package_file"
	else
		"$APK" --allow-untrusted verify "$package_file"
	fi
	"$APK" adbdump "$package_file" >/dev/null
}

mk_apk() {
	local name="$1" arch="$2" description="$3" depends="$4"
	local license="$5" files_root="$6" output="$7"
	shift 7
	local scripts=("$@")
	local args=(
		"$APK" mkpkg
		--info "name:$name"
		--info "version:$APK_VERSION"
		--info "description:$description"
		--info "arch:$arch"
		--info "license:$license"
		--info "origin:openwrt-nes-emulator"
		--info "maintainer:OpenWrt NES Emulator contributors"
		--info "url:https://github.com/libretro/libretro-fceumm"
		--files "$files_root"
		--output "$output"
	)
	[[ -n "$depends" ]] && args+=(--info "depends:$depends")
	[[ -n "$SIGNING_KEY" ]] && args+=(--sign-key "$SIGNING_KEY")

	local script
	for script in "${scripts[@]}"; do
		[[ -n "$script" ]] && args+=(--script "$script")
	done

	if [[ "$ROOT_MODE" == "root" ]]; then
		chown -R 0:0 "$files_root"
	fi
	run_apk_as_root "${args[@]}"
	verify_apk "$output"
	printf '  -> %s\n' "$output"
}

write_file_list() {
	local root="$1" package_name="$2" temporary_list
	temporary_list="$(mktemp "$RUN_DIR/$package_name.list.XXXXXXXX")"
	(
		cd "$root"
		find . -type f -o -type l |
			sed 's#^\./#/#' |
			sort
	) > "$temporary_list"
	mv "$temporary_list" "$root/lib/apk/packages/$package_name.list"
	chmod 0644 "$root/lib/apk/packages/$package_name.list"
}

write_openwrt_post_scripts() {
	local package_name="$1" scripts_dir="$2" custom_file="${3:-}"
	mkdir -p "$scripts_dir"
	{
		# Expanded on the router when apk runs the generated script.
		# shellcheck disable=SC2016
		printf '%s\n' \
			'#!/bin/sh' \
			'[ "${IPKG_NO_SCRIPT}" = "1" ] && exit 0' \
			'[ -s "${IPKG_INSTROOT}/lib/functions.sh" ] || exit 0' \
			'. "${IPKG_INSTROOT}/lib/functions.sh"' \
			'export root="${IPKG_INSTROOT}"' \
			"export pkgname=\"$package_name\"" \
			'add_group_and_user'
		if [[ -n "$custom_file" ]]; then
			printf '%s\n' '('
			sed '/^[[:space:]]*#!/d' "$custom_file"
			printf '%s\n' ') || exit $?'
		fi
		printf '%s\n' 'default_postinst'
	} > "$scripts_dir/post-install"
	{
		printf '%s\n' '#!/bin/sh' 'export PKG_UPGRADE=1'
		sed '/^[[:space:]]*#!/d' "$scripts_dir/post-install"
	} > "$scripts_dir/post-upgrade"
	{
		# Expanded on the router when apk runs the generated script.
		# shellcheck disable=SC2016
		printf '%s\n' \
			'#!/bin/sh' \
			'[ -s "${IPKG_INSTROOT}/lib/functions.sh" ] || exit 0' \
			'. "${IPKG_INSTROOT}/lib/functions.sh"' \
			'export root="${IPKG_INSTROOT}"' \
			"export pkgname=\"$package_name\"" \
			'default_prerm'
	} > "$scripts_dir/pre-deinstall"
	chmod 0755 \
		"$scripts_dir/post-install" \
		"$scripts_dir/post-upgrade" \
		"$scripts_dir/pre-deinstall"
}

NES_CUSTOM_POST="$RUN_DIR/nes-custom-post.sh"
cat > "$NES_CUSTOM_POST" <<'EOF'
#!/bin/sh
[ -n "${IPKG_INSTROOT}" ] && exit 0
[ -f /etc/config/nes-emulator ] &&
	[ ! -L /etc/config/nes-emulator ] || exit 1
META_MODE=
META_NLINK=
META_UID=
META_GID=
read_metadata() {
	local metadata
	metadata="$(LC_ALL=C ls -ldn "$1" 2>/dev/null)" || return 1
	case "$metadata" in
		*'
') return 1 ;;
	esac
	set -- $metadata
	[ "$#" -ge 4 ] || return 1
	case "$2:$3:$4" in *[!0-9:]*) return 1 ;; esac
	META_MODE="$1"
	META_NLINK="$2"
	META_UID="$3"
	META_GID="$4"
}
repair_config_metadata() {
	read_metadata /etc/config/nes-emulator || return 1
	[ "$META_NLINK" = 1 ] || return 1
	if [ "$META_MODE" = -rw------- ] &&
	   [ "$META_UID" = 0 ] && [ "$META_GID" = 0 ]; then
		return 0
	fi
	chown root:root /etc/config/nes-emulator &&
		chmod 0600 /etc/config/nes-emulator || return 1
	read_metadata /etc/config/nes-emulator &&
		[ "$META_MODE" = -rw------- ] &&
		[ "$META_NLINK" = 1 ] &&
		[ "$META_UID" = 0 ] && [ "$META_GID" = 0 ]
}
repair_directory_metadata() {
	local dir="$1" expected_uid="$2" expected_gid="$3" owner_group="$4"
	if [ -e "$dir" ] || [ -L "$dir" ]; then
		[ -d "$dir" ] && [ ! -L "$dir" ] || return 1
	else
		mkdir "$dir" || return 1
	fi
	read_metadata "$dir" || return 1
	if [ "$META_MODE" = drwxr-x--- ] &&
	   [ "$META_UID" = "$expected_uid" ] &&
	   [ "$META_GID" = "$expected_gid" ]; then
		return 0
	fi
	chown "$owner_group" "$dir" && chmod 0750 "$dir" || return 1
	read_metadata "$dir" &&
		[ "$META_MODE" = drwxr-x--- ] &&
		[ "$META_UID" = "$expected_uid" ] &&
		[ "$META_GID" = "$expected_gid" ]
}
repair_managed_rom_metadata() {
	local rom="$1" expected_gid="$2"
	read_metadata "$rom" || return 0
	[ "$META_NLINK" = 1 ] || return 0
	if [ "$META_MODE" = -rw-r----- ] &&
	   [ "$META_UID" = 0 ] && [ "$META_GID" = "$expected_gid" ]; then
		return 0
	fi
	chown root:nesd "$rom" && chmod 0640 "$rom" || return 1
	read_metadata "$rom" &&
		[ "$META_MODE" = -rw-r----- ] &&
		[ "$META_NLINK" = 1 ] &&
		[ "$META_UID" = 0 ] &&
		[ "$META_GID" = "$expected_gid" ]
}
# Stop every persistent or on-demand instance before replacing metadata or the
# executable. default_postinst starts it again only when configuration allows.
/etc/init.d/nes-emulator stop >/dev/null 2>&1 || true
nesd_uid="$(id -u nesd 2>/dev/null)" || exit 1
nesd_gid="$(id -g nesd 2>/dev/null)" || exit 1
case "$nesd_uid:$nesd_gid" in
	''|:*|*:|*[!0-9:]*) exit 1 ;;
esac
repair_config_metadata || exit 1
# One-time router-safety migration; keep mirrored in scripts/build-apks.sh.
migration="$(uci -q get nes-emulator.main.safety_migration 2>/dev/null)"
stream_fps="$(uci -q get nes-emulator.main.stream_fps 2>/dev/null)"
stream_format="$(uci -q get nes-emulator.main.stream_format 2>/dev/null)"
port="$(uci -q get nes-emulator.main.port 2>/dev/null)"
migrate_port() {
	case "$port" in
		9090|''|*[!0-9]*)
			uci -q set nes-emulator.main.port='29876' || return 1
			;;
		*)
			[ "$port" -ge 1 ] 2>/dev/null &&
				[ "$port" -le 65535 ] 2>/dev/null ||
				uci -q set nes-emulator.main.port='29876' || return 1
			;;
	esac
}
migrate_extra_rom_dirs_enabled() {
	# Existing list entries are preserved, but old configurations require an
	# explicit opt-in before external storage is scanned again.
	uci -q set nes-emulator.main.extra_rom_dirs_enabled='0'
}
case "$migration" in
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
	case "$stream_fps" in
		1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|51|52|53|54|55|56|57|58|59|60) ;;
		*) uci -q set nes-emulator.main.stream_fps='2' || exit 1 ;;
	esac
	if [ "$stream_format" = raw ] && [ "$stream_fps" = 5 ]; then
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
	case "$stream_fps" in
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
repair_directory_metadata /etc/nes-emulator 0 "$nesd_gid" root:nesd ||
	exit 1
for dir in \
	/etc/nes-emulator/roms \
	/etc/nes-emulator/saves \
	/etc/nes-emulator/system
do
	repair_directory_metadata "$dir" "$nesd_uid" "$nesd_gid" \
		nesd:nesd || exit 1
done
for rom in /etc/nes-emulator/roms/*; do
	[ -e "$rom" ] || [ -L "$rom" ] || continue
	[ -f "$rom" ] && [ ! -L "$rom" ] || continue
	case "$(printf '%s' "${rom##*.}" |
		tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')" in
		nes|fds|unf|unif) ;;
		*) continue ;;
	esac
	repair_managed_rom_metadata "$rom" "$nesd_gid" || exit 1
done
exit 0
EOF

LUCI_CUSTOM_POST="$RUN_DIR/luci-custom-post.sh"
cat > "$LUCI_CUSTOM_POST" <<'EOF'
#!/bin/sh
[ -n "${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache 2>/dev/null
rm -rf /tmp/luci-modulecache 2>/dev/null
/etc/init.d/rpcd reload >/dev/null 2>&1 || true
exit 0
EOF

LUCI_ROOT="$RUN_DIR/root-luci"
LUCI_SCRIPTS="$RUN_DIR/scripts-luci"
mkdir -p \
	"$LUCI_ROOT/usr/share/luci/menu.d" \
	"$LUCI_ROOT/usr/share/rpcd/acl.d" \
	"$LUCI_ROOT/usr/libexec/rpcd" \
	"$LUCI_ROOT/usr/share/licenses/luci-app-nes-emulator" \
	"$LUCI_ROOT/www/luci-static/resources/view/nes-emulator" \
	"$LUCI_ROOT/lib/apk/packages"
install -m 0644 "$LUCI_DIR/root/usr/share/luci/menu.d/"*.json \
	"$LUCI_ROOT/usr/share/luci/menu.d/"
install -m 0644 "$LUCI_DIR/root/usr/share/rpcd/acl.d/"*.json \
	"$LUCI_ROOT/usr/share/rpcd/acl.d/"
install -m 0755 "$LUCI_DIR/root/usr/libexec/rpcd/nes-emulator" \
	"$LUCI_ROOT/usr/libexec/rpcd/nes-emulator"
install -m 0644 "$LUCI_DIR/htdocs/luci-static/resources/view/nes-emulator/"*.js \
	"$LUCI_ROOT/www/luci-static/resources/view/nes-emulator/"
install -m 0644 "$LUCI_DIR/files/LICENSE-MIT" \
	"$LUCI_ROOT/usr/share/licenses/luci-app-nes-emulator/MIT"
write_file_list "$LUCI_ROOT" "luci-app-nes-emulator"
write_openwrt_post_scripts \
	"luci-app-nes-emulator" "$LUCI_SCRIPTS" "$LUCI_CUSTOM_POST"

LUCI_APK="$RUN_DIR/luci-app-nes-emulator-$APK_VERSION.apk"
mk_apk \
	"luci-app-nes-emulator" \
	"noarch" \
	"Authenticated LuCI client for the NES emulator" \
	"luci-base rpcd nes-emulator=$APK_VERSION" \
	"MIT" \
	"$LUCI_ROOT" \
	"$LUCI_APK" \
	"post-install:$LUCI_SCRIPTS/post-install" \
	"post-upgrade:$LUCI_SCRIPTS/post-upgrade" \
	"pre-deinstall:$LUCI_SCRIPTS/pre-deinstall"

for openwrt_arch in "${OPENWRT_ARCHES[@]}"; do
	binary="${BIN_CACHE[$openwrt_arch]}"
	NES_ROOT="$RUN_DIR/root-nes-$openwrt_arch"
	NES_SCRIPTS="$RUN_DIR/scripts-nes-$openwrt_arch"
	mkdir -p \
		"$NES_ROOT/usr/bin" \
		"$NES_ROOT/etc/init.d" \
		"$NES_ROOT/etc/config" \
		"$NES_ROOT/etc/nes-emulator/roms" \
		"$NES_ROOT/etc/nes-emulator/saves" \
		"$NES_ROOT/etc/nes-emulator/system" \
		"$NES_ROOT/usr/share/licenses/nes-emulator" \
		"$NES_ROOT/usr/share/doc/nes-emulator" \
		"$NES_ROOT/lib/apk/packages"

	install -m 0755 "$binary" "$NES_ROOT/usr/bin/nesd"
	install -m 0755 "$NES_PACKAGE_DIR/files/nes-emulator.init" \
		"$NES_ROOT/etc/init.d/nes-emulator"
	install -m 0600 "$NES_PACKAGE_DIR/files/nes-emulator.config" \
		"$NES_ROOT/etc/config/nes-emulator"
	chmod 0750 \
		"$NES_ROOT/etc/nes-emulator" \
		"$NES_ROOT/etc/nes-emulator/roms" \
		"$NES_ROOT/etc/nes-emulator/saves" \
		"$NES_ROOT/etc/nes-emulator/system"
	install -m 0644 "$FCEUMM_TREE/Copying" \
		"$NES_ROOT/usr/share/licenses/nes-emulator/FCEUmm-Copying"
	install -m 0644 "$NES_PACKAGE_DIR/files/LICENSE-MIT" \
		"$NES_ROOT/usr/share/licenses/nes-emulator/Host-MIT"
	{
		printf 'FCEUmm source: %s\n' \
			'https://github.com/libretro/libretro-fceumm'
		printf 'Commit: %s\n' "$FCEUMM_COMMIT"
		printf 'Commit date: %s\n' "$FCEUMM_COMMIT_DATE"
		printf 'Source archive SHA-256: %s\n' "$FCEUMM_SOURCE_SHA256"
		printf 'Savestate parser hardening patch SHA-256: %s\n' \
			"$FCEUMM_STATE_PATCH_SHA256"
		printf 'Immutable ROM buffer patch SHA-256: %s\n' \
			"$FCEUMM_ROM_BUFFER_PATCH_SHA256"
	} > "$NES_ROOT/usr/share/doc/nes-emulator/FCEUMM-PROVENANCE"

	printf '%s\n' 'nesd:nesd' \
		> "$NES_ROOT/lib/apk/packages/nes-emulator.rusers"
	write_file_list "$NES_ROOT" "nes-emulator"
	printf '%s\n' '/etc/config/nes-emulator' \
		> "$NES_ROOT/lib/apk/packages/nes-emulator.conffiles"
	config_hash="$(sha256sum "$NES_ROOT/etc/config/nes-emulator" | cut -d' ' -f1)"
	printf '/etc/config/nes-emulator %s\n' "$config_hash" \
		> "$NES_ROOT/lib/apk/packages/nes-emulator.conffiles_static"
	write_openwrt_post_scripts \
		"nes-emulator" "$NES_SCRIPTS" "$NES_CUSTOM_POST"

	FEED_DIR="$OUT/$openwrt_arch"
	mkdir -p "$FEED_DIR"
	NES_APK="$FEED_DIR/nes-emulator-$APK_VERSION.apk"
	mk_apk \
		"nes-emulator" \
		"$openwrt_arch" \
		"NES emulator daemon with statically linked FCEUmm" \
		"" \
		"GPL-2.0-only AND MIT" \
		"$NES_ROOT" \
		"$NES_APK" \
		"post-install:$NES_SCRIPTS/post-install" \
		"post-upgrade:$NES_SCRIPTS/post-upgrade" \
		"pre-deinstall:$NES_SCRIPTS/pre-deinstall"
	cp "$LUCI_APK" "$FEED_DIR/luci-app-nes-emulator-$APK_VERSION.apk"
done

if [[ -n "$SIGNING_PUBKEY" ]]; then
	mkdir -p "$OUT/keys"
	cp "$SIGNING_PUBKEY" "$OUT/keys/$(basename "$SIGNING_PUBKEY")"
	chmod 0644 "$OUT/keys/$(basename "$SIGNING_PUBKEY")"
fi

verify_feed_index() {
	local index_file="$1" arch="$2" dump_file
	dump_file="$RUN_DIR/index-$arch.txt"
	"$APK" adbdump "$index_file" > "$dump_file"
	[[ "$(grep -Ec '^  - name: ' "$dump_file" || true)" == "2" ]] ||
		die "feed index for $arch does not contain exactly two packages"
	[[ "$(grep -Fxc '  - name: nes-emulator' "$dump_file" || true)" == "1" ]] ||
		die "feed index for $arch has an invalid nes-emulator entry"
	[[ "$(grep -Fxc '  - name: luci-app-nes-emulator' \
		"$dump_file" || true)" == "1" ]] ||
		die "feed index for $arch has an invalid LuCI entry"
	[[ "$(grep -Fxc "    version: $APK_VERSION" \
		"$dump_file" || true)" == "2" ]] ||
		die "feed index for $arch has an unexpected package version"
	[[ "$(grep -Fxc "    arch: $arch" "$dump_file" || true)" == "1" ]] ||
		die "feed index for $arch has an invalid native architecture"
	[[ "$(grep -Fxc '    arch: noarch' "$dump_file" || true)" == "1" ]] ||
		die "feed index for $arch has an invalid LuCI architecture"
	[[ "$(grep -Ec '^    depends:' "$dump_file" || true)" == "1" ]] ||
		die "feed index for $arch has unexpected dependency metadata"
	for dependency in luci-base rpcd; do
		[[ "$(grep -Ec "^[[:space:]]+- $dependency$" \
			"$dump_file" || true)" == "1" ]] ||
			die "feed index for $arch has invalid dependency: $dependency"
	done
	[[ "$(grep -Fxc "      - nes-emulator=$APK_VERSION" \
		"$dump_file" || true)" == "1" ]] ||
		die "feed index for $arch does not require the matching native package"
}

for openwrt_arch in "${OPENWRT_ARCHES[@]}"; do
	FEED_DIR="$OUT/$openwrt_arch"
	INDEX_TEMP="$RUN_DIR/packages-$openwrt_arch.adb"
	index_args=(
		"$APK"
	)
	if [[ -n "$SIGNING_KEY" ]]; then
		index_args+=(--keys-dir "$SIGNING_TRUST_DIR")
	else
		index_args+=(--allow-untrusted)
	fi
	# Expanded by apk when it names entries in the generated index.
	# shellcheck disable=SC2016
	index_args+=(
		mkndx
		--description "OpenWrt NES Emulator $APK_VERSION ($openwrt_arch)"
		--pkgname-spec '${name}-${version}.apk'
		--output "$INDEX_TEMP"
		"$FEED_DIR/nes-emulator-$APK_VERSION.apk"
		"$FEED_DIR/luci-app-nes-emulator-$APK_VERSION.apk"
	)
	[[ -n "$SIGNING_KEY" ]] && index_args+=(--sign-key "$SIGNING_KEY")
	"${index_args[@]}"
	verify_feed_index "$INDEX_TEMP" "$openwrt_arch"
	mv -f -- "$INDEX_TEMP" "$FEED_DIR/packages.adb"
	(
		cd "$FEED_DIR"
		sha256sum \
			"nes-emulator-$APK_VERSION.apk" \
			"luci-app-nes-emulator-$APK_VERSION.apk" \
			packages.adb > SHA256SUMS.tmp
		mv -f SHA256SUMS.tmp SHA256SUMS
	)
done

SOURCE_STAGE="$RUN_DIR/openwrt-nes-emulator-$APK_VERSION-source"
mkdir -p "$SOURCE_STAGE"
copy_clean_source_tree "$PROJECT_SNAPSHOT" "$SOURCE_STAGE"
mkdir -p "$SOURCE_STAGE/third_party"
cp -a "$FCEUMM_PRISTINE_TREE" \
	"$SOURCE_STAGE/third_party/libretro-fceumm"
[[ "$(tree_sha256 "$SOURCE_STAGE/third_party/libretro-fceumm")" == \
	"$FCEUMM_TREE_SHA256" ]] ||
	die "the bundled pristine FCEUmm tree does not match its provenance"
assert_no_build_products "$SOURCE_STAGE"
assert_regular_tree "$SOURCE_STAGE"
{
	printf 'OpenWrt NES Emulator version: %s\n' "$APK_VERSION"
	printf 'Source date epoch: %s\n' "$SOURCE_DATE_EPOCH"
	printf 'FCEUmm repository: %s\n' \
		'https://github.com/libretro/libretro-fceumm'
	printf 'FCEUmm commit: %s\n' "$FCEUMM_COMMIT"
	printf 'FCEUmm commit date: %s\n' "$FCEUMM_COMMIT_DATE"
	printf 'FCEUmm codeload URL: %s\n' "$FCEUMM_SOURCE_URL"
	printf 'FCEUmm codeload SHA-256: %s\n' "$FCEUMM_SOURCE_SHA256"
	printf 'FCEUmm pristine extracted-tree SHA-256: %s\n' "$FCEUMM_TREE_SHA256"
	printf 'FCEUmm savestate parser patch SHA-256: %s\n' \
		"$FCEUMM_STATE_PATCH_SHA256"
	printf 'FCEUmm immutable ROM buffer patch SHA-256: %s\n' \
		"$FCEUMM_ROM_BUFFER_PATCH_SHA256"
	printf 'FCEUmm patched build-tree SHA-256: %s\n' \
		"$FCEUMM_PATCHED_TREE_SHA256"
	printf 'FCEUmm license: GPL-2.0-only (see third_party/libretro-fceumm/Copying)\n'
	printf 'Host and LuCI license: MIT (see LICENSE)\n'
	printf 'Combined statically linked nesd license: GPL-2.0-only\n'
	printf 'OpenWrt package architecture matrix: %s\n' "$OPENWRT_ARCH_RELEASE"
	printf 'Pinned OpenWrt toolchain release: %s\n' \
		"$OPENWRT_TOOLCHAIN_RELEASE"
	printf 'Zig version used by this build: %s\n' "$ZIG_VERSION"
	printf 'apk-tools version used by this build: %s\n' \
		"$("$APK" --version 2>&1 | head -n 1)"
	printf 'Selected architecture profiles:\n'
	for openwrt_arch in "${OPENWRT_ARCHES[@]}"; do
		printf '  %s: builder=%s target=%s flags=%s endian=%s abi=%s\n' \
			"$openwrt_arch" \
			"${PROFILE_BUILDER[$openwrt_arch]}" \
			"${PROFILE_TARGET[$openwrt_arch]}" \
			"${PROFILE_FLAGS[$openwrt_arch]}" \
			"${PROFILE_ENDIAN[$openwrt_arch]}" \
			"${PROFILE_ABI[$openwrt_arch]}"
		printf '    compiler-id=%s\n' \
			"${COMPILER_ID[$openwrt_arch]}"
		if [[ "${PROFILE_BUILDER[$openwrt_arch]}" == "openwrt-gcc" ]]; then
			printf '    toolchain-prefix=%s\n' \
				"${TOOLCHAIN_PREFIX[$openwrt_arch]}"
			printf '    toolchain-url=%s\n' \
				"${TOOLCHAIN_URL[$openwrt_arch]}"
			printf '    toolchain-sha256=%s\n' \
				"${TOOLCHAIN_SHA256[$openwrt_arch]}"
		fi
	done
} > "$SOURCE_STAGE/PROVENANCE.txt"
(
	cd "$SOURCE_STAGE"
	find . -type f -print0 |
		sort -z |
		xargs -0 sha256sum
) > "$RUN_DIR/SOURCE-SHA256SUMS"
mv "$RUN_DIR/SOURCE-SHA256SUMS" "$SOURCE_STAGE/SOURCE-SHA256SUMS"

mkdir -p "$OUT/sources"
SOURCE_BUNDLE="$OUT/sources/openwrt-nes-emulator-$APK_VERSION-source.tar.gz"
SOURCE_BUNDLE_TEMP="$RUN_DIR/source-bundle.tar.gz"
tar \
	--sort=name \
	--mtime="@$SOURCE_DATE_EPOCH" \
	--owner=0 \
	--group=0 \
	--numeric-owner \
	-C "$RUN_DIR" \
	-cf - "$(basename "$SOURCE_STAGE")" |
	gzip -n -9 > "$SOURCE_BUNDLE_TEMP"
mv -f -- "$SOURCE_BUNDLE_TEMP" "$SOURCE_BUNDLE"
(
	cd "$OUT/sources"
	sha256sum "$(basename "$SOURCE_BUNDLE")" > SHA256SUMS.tmp
	mv -f SHA256SUMS.tmp SHA256SUMS
)

cat > "$OUT/INSTALL.txt" <<EOF
OpenWrt NES Emulator $APK_VERSION
================================

The output contains a separate APK v3 feed for each architecture:

  <arch>/nes-emulator-$APK_VERSION.apk
  <arch>/luci-app-nes-emulator-$APK_VERSION.apk
  <arch>/packages.adb
  <arch>/SHA256SUMS

A full build without ARCHES covers all 35 package ABIs in the official
OpenWrt $OPENWRT_ARCH_RELEASE release: 35 directories and 70 APKs. To find
the correct directory for a router, run:

  apk --print-arch

FCEUmm commit $FCEUMM_SHORT_COMMIT is statically linked into /usr/bin/nesd.
No separate fceumm_libretro.so is required.

ABI coverage does not guarantee that a particular older router has enough
flash storage, RAM, or performance for emulation and software video encoding.
EOF

if [[ -n "$SIGNING_PUBKEY" ]]; then
	public_key_basename="$(basename "$SIGNING_PUBKEY")"
	cat >> "$OUT/INSTALL.txt" <<EOF

Signed local installation of this build:

  cp keys/$public_key_basename /etc/apk/keys/$public_key_basename
  chmod 0644 /etc/apk/keys/$public_key_basename
  cd <arch>
  sha256sum -c SHA256SUMS
  apk add ./nes-emulator-$APK_VERSION.apk ./luci-app-nes-emulator-$APK_VERSION.apk

The private key is never copied into the build output.
EOF
else
	cat >> "$OUT/INSTALL.txt" <<EOF

Unsigned local installation:

  cd <arch>
  sha256sum -c SHA256SUMS
  apk --allow-untrusted add ./nes-emulator-$APK_VERSION.apk ./luci-app-nes-emulator-$APK_VERSION.apk
EOF
fi

cat >> "$OUT/INSTALL.txt" <<EOF

Complete corresponding source:

  sources/$(basename "$SOURCE_BUNDLE")
  sources/SHA256SUMS

OUT is published as one atomic snapshot. Previous APKs, indexes, and keys in
that directory are removed; move any historical artifacts elsewhere first.
A non-empty custom OUT is replaced only when it has the ownership marker made
by this script. System root directories are forbidden as OUT.
EOF

publish_output
SOURCE_BUNDLE="$OUT/sources/$(basename "$SOURCE_BUNDLE")"

printf '\nBuild complete: %s\n' "$OUT"
printf 'Version: %s\n' "$APK_VERSION"
printf 'FCEUmm: %s (%s)\n' "$FCEUMM_COMMIT" "$FCEUMM_COMMIT_DATE"
printf 'Signing: %s\n' "$([[ -n "$SIGNING_KEY" ]] && echo enabled || echo disabled)"
printf 'Source bundle: %s\n' "$SOURCE_BUNDLE"
