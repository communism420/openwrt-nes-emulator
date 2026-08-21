#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

PYTHONDONTWRITEBYTECODE=1 python3 tests/repository_checks.py
PYTHONDONTWRITEBYTECODE=1 python3 -c \
	'import ast, pathlib; [ast.parse(p.read_text("utf-8"), str(p)) for p in map(pathlib.Path, ("tests/integration.py", "tests/rpc_client_contract.py", "tests/demo_rom_contract.py", "tests/upstream_export_contract.py", "scripts/embed-play-html.py", "scripts/make-demo-rom.py", "scripts/export-openwrt-upstream.py"))]'
PYTHONDONTWRITEBYTECODE=1 python3 tests/demo_rom_contract.py
PYTHONDONTWRITEBYTECODE=1 python3 scripts/export-openwrt-upstream.py --check-templates
PYTHONDONTWRITEBYTECODE=1 python3 tests/upstream_export_contract.py

if command -v "${CC:-cc}" >/dev/null 2>&1; then
	sh tests/host_audio_contract.sh
	sh tests/http_audio_metadata_contract.sh
	sh tests/http_jpeg_worker_contract.sh
	PYTHONDONTWRITEBYTECODE=1 python3 tests/rpc_client_contract.py
	PYTHONDONTWRITEBYTECODE=1 python3 tests/sha256_contract.py
fi

sh -n package/nes-emulator/files/nes-emulator.init
sh -n package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator
sh -n install.sh
sh -n tests/extra_rom_dirs_contract.sh
sh -n tests/host_audio_contract.sh
sh -n tests/http_audio_metadata_contract.sh
sh -n tests/http_jpeg_worker_contract.sh
sh -n tests/install_contract.sh
sh tests/extra_rom_dirs_contract.sh
sh tests/install_contract.sh
sh tests/rpcd_permission_contract.sh
sh tests/rpcd_transport_contract.sh
sh tests/rpcd_resource_contract.sh
sh tests/postinst_resource_contract.sh
bash -n scripts/build-apks.sh

if command -v node >/dev/null 2>&1; then
	node tests/luci_rpc_contract.js
	node tests/play_lifecycle_contract.js
	node tests/settings_contract.js
	for file in package/luci-app-nes-emulator/htdocs/luci-static/resources/view/nes-emulator/*.js; do
		node --check "$file"
	done
	sed -n '/<script>/,/<\/script>/p' package/nes-emulator/src/play.html |
		sed '1d;$d' |
		node --check
fi

echo "all static checks passed"
