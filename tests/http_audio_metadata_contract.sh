#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/nes-http-audio-contract.XXXXXX")

cleanup() {
	rm -rf -- "$work_dir"
}
trap cleanup EXIT HUP INT TERM

${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections \
	-I"$repo_dir/package/nes-emulator/src" \
	"$repo_dir/tests/http_audio_metadata_contract.c" \
	-Wl,--gc-sections -pthread -ldl -lm \
	-o "$work_dir/http_audio_metadata_contract"
"$work_dir/http_audio_metadata_contract"
echo "HTTP audio transport contract: OK"
