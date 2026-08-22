#!/usr/bin/env python3
"""Fast, host-independent repository consistency and security checks."""

from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
import unicodedata


ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.0.0"
PACKAGE_RELEASE = "19"
PROJECT_SOURCE_DATE_EPOCH = "1787270400"
VALID_STREAM_FPS_CASE = "|".join(str(value) for value in range(1, 61)) + ") ;;"
OFFICIAL_OPENWRT_25_12_ARCHES = (
    "aarch64_cortex-a53",
    "aarch64_cortex-a72",
    "aarch64_cortex-a76",
    "aarch64_generic",
    "arm_arm1176jzf-s_vfp",
    "arm_arm926ej-s",
    "arm_cortex-a15_neon-vfpv4",
    "arm_cortex-a5_vfpv4",
    "arm_cortex-a7",
    "arm_cortex-a7_neon-vfpv4",
    "arm_cortex-a7_vfpv4",
    "arm_cortex-a8_vfpv3",
    "arm_cortex-a9",
    "arm_cortex-a9_neon",
    "arm_cortex-a9_vfpv3-d16",
    "arm_fa526",
    "arm_xscale",
    "armeb_xscale",
    "i386_pentium-mmx",
    "i386_pentium4",
    "loongarch64_generic",
    "mips64_mips64r2",
    "mips64_octeonplus",
    "mips64el_mips64r2",
    "mips_24kc",
    "mips_mips32",
    "mipsel_24kc",
    "mipsel_24kc_24kf",
    "mipsel_74kc",
    "mipsel_mips32",
    "powerpc64_e5500",
    "powerpc_464fp",
    "powerpc_8548",
    "riscv64_generic",
    "x86_64",
)
BIG_ENDIAN_ARCHES = {
    "armeb_xscale",
    "mips64_mips64r2",
    "mips64_octeonplus",
    "mips_24kc",
    "mips_mips32",
    "powerpc64_e5500",
    "powerpc_464fp",
    "powerpc_8548",
}


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_publication_surface() -> None:
    readme = read("README.md")
    apk_version = f"{VERSION}-r{PACKAGE_RELEASE}"
    badge_version = apk_version.replace("-", "--")
    require(
        f"version-{badge_version}" in readme
        and f"APK%20architectures-{len(OFFICIAL_OPENWRT_25_12_ARCHES)}" in readme,
        "README badges are stale relative to release metadata",
    )
    require(
        "A joke that became a fully functional" in readme
        and "```mermaid" in readme
        and "docs/assets/openwrt-nes-emulator.png" in readme,
        "README is missing the public landing-page presentation",
    )
    require(
        len(readme.split()) <= 2200,
        "README landing page has grown too large; move details to docs/TECHNICAL.md",
    )

    required = (
        ".gitattributes",
        "CHANGELOG.md",
        "CODE_OF_CONDUCT.md",
        "CONTRIBUTING.md",
        "SECURITY.md",
        "THIRD_PARTY_NOTICES.md",
        "UPSTREAMING.md",
        ".github/ISSUE_TEMPLATE/bug-report.yml",
        ".github/ISSUE_TEMPLATE/feature-request.yml",
        ".github/ISSUE_TEMPLATE/config.yml",
        ".github/pull_request_template.md",
        "docs/TECHNICAL.md",
        "docs/PUBLISHING.md",
        "docs/assets/openwrt-nes-emulator.png",
        "docs/assets/social-preview.svg",
        "docs/assets/social-preview.png",
        "install.sh",
        "scripts/make-demo-rom.py",
        "scripts/export-openwrt-upstream.py",
        "tests/demo_rom_contract.py",
        "tests/install_contract.sh",
        "tests/upstream_export_contract.py",
        "package/luci-app-nes-emulator/Makefile.upstream",
        "package/luci-app-nes-emulator/po/templates/nes-emulator.pot",
        "upstream/openwrt-packages/multimedia/nes-emulator/Makefile",
    )
    for relative in required:
        path = ROOT / relative
        require(path.is_file() and path.stat().st_size > 0, f"missing {relative}")

    image = (ROOT / "docs/assets/openwrt-nes-emulator.png").read_bytes()
    require(image[:8] == b"\x89PNG\r\n\x1a\n", "README screenshot is not PNG")
    width = int.from_bytes(image[16:20], "big")
    height = int.from_bytes(image[20:24], "big")
    require(
        width >= 960 and height >= 540 and len(image) <= 2 * 1024 * 1024,
        "README screenshot has unsuitable dimensions or file size",
    )
    social = (ROOT / "docs/assets/social-preview.png").read_bytes()
    require(social[:8] == b"\x89PNG\r\n\x1a\n", "social preview is not PNG")
    require(
        int.from_bytes(social[16:20], "big") == 1280
        and int.from_bytes(social[20:24], "big") == 640
        and len(social) <= 1024 * 1024,
        "GitHub social preview must be 1280x640 and below 1 MiB",
    )

    public_markdown = (
        "README.md",
        "CHANGELOG.md",
        "CODE_OF_CONDUCT.md",
        "CONTRIBUTING.md",
        "SECURITY.md",
        "THIRD_PARTY_NOTICES.md",
        "UPSTREAMING.md",
        "docs/TECHNICAL.md",
        "docs/PUBLISHING.md",
    )
    for relative in public_markdown:
        document = read(relative)
        require(
            "OWNER/REPOSITORY" not in document,
            f"{relative} contains an unresolved repository placeholder",
        )
        base = (ROOT / relative).parent
        targets = re.findall(r"\]\((?!https?://|#|mailto:)([^)]+)\)", document)
        targets += re.findall(r'(?:src|href)="(?!https?://|#)([^"]+)"', document)
        for target in targets:
            target_path = target.split("#", 1)[0]
            if not target_path:
                continue
            require(
                (base / target_path).resolve().exists(),
                f"{relative} links to missing local target {target_path}",
            )

    ignore = read(".gitignore")
    for pattern in ("*.nes", "*.fds", "*.unf", "*.unif", "auth.token", "*-private.pem"):
        require(pattern in ignore, f".gitignore does not protect {pattern}")
    attributes = read(".gitattributes")
    require(
        "* text=auto eol=lf" in attributes
        and "play.html.h linguist-generated=true" in attributes
        and "*.png binary" in attributes,
        ".gitattributes does not protect line endings and generated/binary files",
    )

    for junk in ("data", "", ""):
        require(not (ROOT / junk).exists(), f"accidental root artifact remains: {junk}")
    rom_suffixes = {".nes", ".fds", ".unf", ".unif", ".srm", ".nss"}
    require(
        not any(path.suffix.lower() in rom_suffixes for path in ROOT.rglob("*")),
        "repository contains a ROM or emulator save file",
    )


def check_installer() -> None:
    installer = read("install.sh")
    readme = read("README.md")
    publishing = read("docs/PUBLISHING.md")
    technical = read("docs/TECHNICAL.md")
    build_script = read("scripts/build-apks.sh")
    check_script = read("scripts/check.sh")
    installer_contract = read("tests/install_contract.sh")
    workflow = read(".github/workflows/ci.yml")

    for marker in (
        "#!/bin/sh",
        "set -eu",
        "set -f",
        "umask 077",
        "LC_ALL=C",
        'release_package_arch="$DISTRIB_ARCH"',
        'APK_ARCH_FILE="${OPENWRT_NES_APK_ARCH_FILE:-/etc/apk/arch}"',
        '"$package_arch" = "$release_package_arch"',
        'say "OpenWrt package architecture: $package_arch"',
        '"-$package_arch.apk"',
        "/releases/latest/download",
        "SHA256SUMS",
        "manifest_entry",
        "sha256sum",
        'apk --allow-untrusted verify "$native_path" "$luci_path"',
        "MAX_MANIFEST_BYTES=1048576",
        "MAX_APK_BYTES=8388608",
        'APK_CACHE_DIR="${OPENWRT_NES_APK_CACHE_DIR:-/etc/apk/cache/openwrt-nes-emulator}"',
        'mkdir -p "$APK_CACHE_DIR"',
        'chmod 0755 "$APK_CACHE_DIR"',
        "apk --update-cache --wait 120 add luci-base rpcd jshn jsonfilter cgi-io",
        "apk --repositories-file /dev/null --no-network",
        '--cache-dir "$APK_CACHE_DIR" --cache-packages',
        '--allow-untrusted --wait 120 add \\\n\t"$native_path" "$luci_path"',
    ):
        require(marker in installer, f"installer is missing required contract: {marker}")

    for forbidden in (
        "--no-check-certificate",
        "curl -k",
        "wget --no-check",
        "eval ",
        "apk upgrade",
        "--force-depends",
        "--force-downgrade",
        "--update-cache --allow-untrusted",
        "apk --print-arch",
        "--no-cache",
    ):
        require(forbidden not in installer, f"installer contains unsafe behavior: {forbidden}")

    require(
        not re.search(
            r"apk [^\n]*(?:--update-cache[^\n]*--allow-untrusted|"
            r"--allow-untrusted[^\n]*--update-cache)",
            installer,
        ),
        "installer applies allow-untrusted while using configured repositories",
    )

    require(
        "raw.githubusercontent.com/communism420/openwrt-nes-emulator/main/install.sh"
        in readme
        and "mktemp /tmp/openwrt-nes-installer.XXXXXX" in readme
        and "uclient-fetch -q -T 30 -O" in readme
        and "apk --repositories-file /dev/null --no-network" in readme
        and "--cache-dir /etc/apk/cache/openwrt-nes-emulator --cache-packages"
        in readme
        and "--no-cache" not in readme
        and "| sh" not in readme,
        "README automatic installation command is missing or masks download failures",
    )
    require(
        ". /etc/openwrt_release" in readme
        and 'ABI="$DISTRIB_ARCH"' in readme
        and "DISTRIB_ARCH" in technical
        and "DISTRIB_ARCH" in publishing
        and ". /etc/openwrt_release" in build_script
        and "apk --print-arch" not in readme
        and "apk --print-arch" not in publishing
        and "apk --print-arch" not in build_script,
        "installation docs do not use the exact OpenWrt package architecture",
    )
    require(
        "aarch64_cortex-a53" in installer_contract
        and "printf '%s\\n' 'aarch64'" in installer_contract
        and "generic apk-tools architecture was selected" in installer_contract
        and "does not declare a package architecture" in installer_contract,
        "installer tests do not cover generic apk ABI versus OpenWrt package profile",
    )
    require(
        "stable interface" in publishing
        and "tests/install_contract.sh" in publishing,
        "publication guide does not preserve the installer's release-asset contract",
    )
    require(
        '"$ROOT_DIR/install.sh"' in build_script,
        "corresponding-source snapshot omits install.sh",
    )
    require(
        "sh -n install.sh" in check_script
        and "sh tests/install_contract.sh" in check_script,
        "static checks do not execute the installer contract",
    )
    require(
        "shellcheck install.sh tests/install_contract.sh" in workflow,
        "CI does not run ShellCheck over install.sh",
    )


def extract_safety_migration(contents: str) -> str:
    migration_match = re.search(
        r"# One-time router-safety migration;.*?"
        r"# End one-time router-safety migration\.",
        contents.replace("$$", "$"),
        re.DOTALL,
    )
    require(migration_match is not None, "one-time router-safety migration is missing")
    return migration_match.group(0)


def check_upgrade_migration() -> None:
    """Execute the packaged shell migration against a fake UCI backend."""
    package_migration = extract_safety_migration(
        read("package/nes-emulator/Makefile")
    )
    standalone_migration = extract_safety_migration(read("scripts/build-apks.sh"))
    require(
        package_migration == standalone_migration,
        "OpenWrt and standalone APK safety migrations differ",
    )
    shell = shutil.which("sh") or shutil.which("bash")
    require(shell is not None, "no POSIX shell is available for migration tests")

    harness = r"""
uci() {
	case "$*" in
		'-q get nes-emulator.main.safety_migration')
			[ "${TEST_MIGRATION+x}" = x ] || return 1
			printf '%s\n' "$TEST_MIGRATION"
			;;
		'-q get nes-emulator.main.stream_fps')
			[ "${TEST_STREAM_FPS+x}" = x ] || return 1
			printf '%s\n' "$TEST_STREAM_FPS"
			;;
		'-q get nes-emulator.main.stream_format')
			[ "${TEST_STREAM_FORMAT+x}" = x ] || return 1
			printf '%s\n' "$TEST_STREAM_FORMAT"
			;;
		'-q get nes-emulator.main.port')
			[ "${TEST_PORT+x}" = x ] || return 1
			printf '%s\n' "$TEST_PORT"
			;;
		'-q set '*|'-q commit '*)
			printf 'uci:%s\n' "$*"
			;;
		*) return 1 ;;
	esac
}
repair_config_metadata() { printf '%s\n' repair-config-metadata; }
"""

    def execute(
        marker: str | None,
        stream_fps: str,
        stream_format: str = "raw",
        port: str = "9090",
    ) -> list[str]:
        assignments = [
            f"TEST_STREAM_FPS={shlex.quote(stream_fps)}",
            f"TEST_STREAM_FORMAT={shlex.quote(stream_format)}",
            f"TEST_PORT={shlex.quote(port)}",
        ]
        if marker is None:
            assignments.append("unset TEST_MIGRATION")
        else:
            assignments.append(f"TEST_MIGRATION={shlex.quote(marker)}")
        script = (
            "\n".join(assignments)
            + "\n"
            + harness
            + "\n"
            + standalone_migration
            + "\n"
        )
        result = subprocess.run(
            [shell],
            cwd=ROOT,
            input=script.encode(),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        output = result.stdout.decode(errors="replace")
        require(
            result.returncode == 0,
            f"router-safety migration shell failed:\n{output}",
        )
        return output.splitlines()

    current_actions = execute("4", "60")
    require(
        current_actions == [],
        f"current marker was migrated again: {current_actions}",
    )

    future_actions = execute("5", "60")
    require(
        future_actions == [],
        f"future marker was downgraded or rewritten: {future_actions}",
    )

    marker_three_actions = execute("3", "5")
    require(
        marker_three_actions
        == [
            "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
            "uci:-q set nes-emulator.main.safety_migration=4",
            "uci:-q commit nes-emulator",
            "repair-config-metadata",
        ],
        "marker-3 migration did not preserve paths behind a disabled gate: "
        f"{marker_three_actions}",
    )

    marker_two_default_port = execute("2", "5")
    require(
        marker_two_default_port
        == [
            "uci:-q set nes-emulator.main.port=29876",
            "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
            "uci:-q set nes-emulator.main.safety_migration=4",
            "uci:-q commit nes-emulator",
            "repair-config-metadata",
        ],
        f"marker-2 default-port migration is wrong: {marker_two_default_port}",
    )

    marker_two_custom_port = execute("2", "5", port="12345")
    require(
        marker_two_custom_port
        == [
            "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
            "uci:-q set nes-emulator.main.safety_migration=4",
            "uci:-q commit nes-emulator",
            "repair-config-metadata",
        ],
        f"marker-2 custom port was changed: {marker_two_custom_port}",
    )

    for invalid_port in ("", "0", "65536", "invalid"):
        invalid_port_actions = execute("2", "5", port=invalid_port)
        require(
            invalid_port_actions == marker_two_default_port,
            f"invalid port migration is wrong for {invalid_port!r}: "
            f"{invalid_port_actions}",
        )

    legacy_raw_five = execute("1", "5")
    expected_legacy_raw_five = [
        "uci:-q set nes-emulator.main.stream_fps=2",
        "uci:-q set nes-emulator.main.port=29876",
        "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
        "uci:-q set nes-emulator.main.safety_migration=4",
        "uci:-q commit nes-emulator",
        "repair-config-metadata",
    ]
    require(
        legacy_raw_five == expected_legacy_raw_five,
        f"marker-1 raw/5 migration is wrong: {legacy_raw_five}",
    )

    for stream_format, stream_fps in (("jpeg", "20"), ("raw", "60")):
        preserved_actions = execute(
            "1", stream_fps, stream_format, port="12345"
        )
        require(
            preserved_actions
            == [
                "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
                "uci:-q set nes-emulator.main.safety_migration=4",
                "uci:-q commit nes-emulator",
                "repair-config-metadata",
            ],
            "marker-1 migration changed a valid non-default FPS: "
            f"{stream_format}/{stream_fps}: {preserved_actions}",
        )

    marker_one_invalid = execute("1", "61", port="12345")
    require(
        marker_one_invalid
        == [
            "uci:-q set nes-emulator.main.stream_fps=2",
            "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
            "uci:-q set nes-emulator.main.safety_migration=4",
            "uci:-q commit nes-emulator",
            "repair-config-metadata",
        ],
        f"marker-1 invalid FPS migration is wrong: {marker_one_invalid}",
    )

    older_actions = execute(None, "61")
    expected_older_actions = [
        "uci:-q set nes-emulator.main.enabled=0",
        "uci:-q set nes-emulator.main.stream_fps=2",
        "uci:-q set nes-emulator.main.port=29876",
        "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
        "uci:-q set nes-emulator.main.safety_migration=4",
        "uci:-q commit nes-emulator",
        "repair-config-metadata",
    ]
    require(
        older_actions == expected_older_actions,
        f"older invalid-FPS migration is wrong: {older_actions}",
    )

    safe_fps_actions = execute("0", "60", port="12345")
    require(
        safe_fps_actions
        == [
            "uci:-q set nes-emulator.main.enabled=0",
            "uci:-q set nes-emulator.main.extra_rom_dirs_enabled=0",
            "uci:-q set nes-emulator.main.safety_migration=4",
            "uci:-q commit nes-emulator",
            "repair-config-metadata",
        ],
        f"older migration did not preserve safe stream FPS: {safe_fps_actions}",
    )


def check_init_stream_fps() -> None:
    """Execute the init script's router-safe stream-FPS normalizer."""
    init = read("package/nes-emulator/files/nes-emulator.init")
    match = re.search(
        r"normalize_stream_fps\(\) \{\n.*?\n\}",
        init,
        re.DOTALL,
    )
    require(match is not None, "init stream-FPS normalizer is missing")
    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        require(wsl is not None, "WSL is required for init FPS tests on Windows")
        shell_command = [wsl, "--exec", "sh", "-c"]
    else:
        shell = shutil.which("sh") or shutil.which("bash")
        require(shell is not None, "no POSIX shell is available for init FPS tests")
        shell_command = [shell, "-c"]

    cases = {
        "": "2",
        "0": "2",
        "1": "1",
        "2": "2",
        "30": "30",
        "60": "60",
        "61": "2",
        "999": "2",
        "-1": "2",
        "invalid": "2",
        " 2": "2",
    }
    for value, expected in cases.items():
        script = match.group(0) + f"\nnormalize_stream_fps {shlex.quote(value)}"
        result = subprocess.run(
            [*shell_command, script],
            cwd=ROOT,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        require(
            result.returncode == 0 and result.stdout.strip() == expected,
            f"init stream FPS {value!r} normalized incorrectly: {result.stdout!r}",
        )


def check_versions() -> None:
    package_files = (
        "package/nes-emulator/Makefile",
        "package/libretro-fceumm/Makefile",
        "package/luci-app-nes-emulator/Makefile",
    )
    for relative in package_files:
        contents = read(relative)
        version_match = re.search(r"^PKG_VERSION:=(\S+)$", contents, re.MULTILINE)
        release_match = re.search(r"^PKG_RELEASE:=(\S+)$", contents, re.MULTILINE)
        require(version_match is not None, f"{relative}: PKG_VERSION is missing")
        require(
            version_match.group(1) == VERSION,
            f"{relative}: stale version {version_match.group(1)}",
        )
        require(release_match is not None, f"{relative}: PKG_RELEASE is missing")
        require(
            release_match.group(1) == PACKAGE_RELEASE,
            f"{relative}: stale package release {release_match.group(1)}",
        )
        require(
            "PKG_MAINTAINER:=OpenWrt NES Emulator contributors" in contents,
            f"{relative}: public maintainer metadata is stale",
        )
    require(
        f'#define NESD_VERSION "{VERSION}"' in read("package/nes-emulator/src/main.c"),
        "main.c version differs from package version",
    )
    build_script = read("scripts/build-apks.sh")
    require(
        f'PROJECT_VERSION="{VERSION}"' in build_script,
        "build-apks.sh version differs from package version",
    )
    require(
        f'PACKAGE_RELEASE="{PACKAGE_RELEASE}"' in build_script,
        "build-apks.sh release differs from package release",
    )
    require(
        f"PROJECT_SOURCE_DATE_EPOCH:={PROJECT_SOURCE_DATE_EPOCH}"
        in read("package/nes-emulator/Makefile")
        and f'PROJECT_SOURCE_DATE_EPOCH="{PROJECT_SOURCE_DATE_EPOCH}"'
        in build_script,
        "release source epoch differs between package metadata and build script",
    )
    apk_version = f"{VERSION}-r{PACKAGE_RELEASE}"
    require(
        f"The current package version is **{apk_version}**." in read("README.md"),
        "README version differs from package version",
    )
    technical = read("docs/TECHNICAL.md")
    previous_release = str(int(PACKAGE_RELEASE) - 1)
    require(
        f"The current package version is **{apk_version}**." in technical
        and f"`r{previous_release}` and `r{PACKAGE_RELEASE}` intentionally "
        "cannot be mixed." in technical
        and f"## {apk_version} —" in read("CHANGELOG.md"),
        "public release documentation differs from package metadata",
    )


def check_json() -> None:
    for path in (ROOT / "package/luci-app-nes-emulator/root/usr/share").rglob("*.json"):
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise AssertionError(f"{path.relative_to(ROOT)}: {error}") from error


def check_generated_client() -> None:
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts/embed-play-html.py"), "--check"],
        cwd=ROOT,
        check=False,
    )
    require(result.returncode == 0, "embedded play.html.h is stale")
    html = read("package/nes-emulator/src/play.html")
    connect_source = html.split("function connect()", 1)[1].split(
        "function startStream()", 1
    )[0]
    heartbeat_source = html.split("function heartbeat(candidate)", 1)[1].split(
        "function connect()", 1
    )[0]
    require('name="referrer" content="no-referrer"' in html, "client may leak token referrers")
    require(
        "function bootstrapAuthToken()" in html
        and "const suppliedToken = hashParams.get('token') || '';" in html
        and "params.get('token')" not in html
        and "params.has('token') || hashParams.has('token')" in html
        and "Query tokens are scrubbed but deliberately never accepted" in html
        and "history.replaceState" in html
        and "const authToken = bootstrapAuthToken();" in html
        and "sessionStorage.setItem" in html
        and "sessionStorage.getItem" in html
        and "Keep the token in memory when browser storage is blocked" in html,
        "client token bootstrap is not restricted to scrubbed fragments and session storage",
    )
    require(
        "'Authorization': 'Bearer ' + authToken" in html
        and "new URL('/ws', location.href)" in html
        and "url.searchParams.set('token', authToken)" in html,
        "client authentication is incomplete or WebSocket query authentication was removed",
    )
    require(
        "let streamRequested = false;" in html
        and "function startStream()" in html
        and "streamRequested = true;" in html
        and "function suspendStream()" in html
        and "if (!pageActive || document.hidden || !streamRequested" in html
        and "else if (pageActive)" in html
        and "if (streamRequested)" in html
        and "if (!document.hidden && streamRequested)" in html
        and "window.addEventListener('online'" in html
        and "window.addEventListener('pageshow'" in html,
        "game client opens or reconnects a media stream without an explicit action",
    )
    require(
        "document.addEventListener('visibilitychange'" in html
        and "suspendStream();" in html
        and "else if (pageActive)" in html
        and "if (streamRequested)" in html
        and "stream paused while this tab is hidden" in html,
        "hidden game tabs consume bandwidth or cannot resume the requested stream",
    )
    lifecycle = read("tests/play_lifecycle_contract.js")
    require(
        "query token bootstrapped the client instead of session storage" in lifecycle
        and "query-only token bootstrapped an unauthenticated client" in lifecycle
        and "fragment token was not retained, stored and scrubbed" in lifecycle
        and "WebSocket query authentication was not preserved" in lifecycle,
        "fragment-only bootstrap or WebSocket authentication regressions are not covered",
    )
    require(
        'id="fps-counter"' not in html
        and "#fps-counter" not in html
        and "fpsNode" not in html,
        "legacy browser FPS widget is still present",
    )
    fps_osd_tag = re.search(r'<canvas\s+id="fps-osd"[^>]*>', html)
    fps_format_start = html.find("function formatFpsValue(measured)")
    fps_format_end = html.find("\n\t\tfunction ", fps_format_start + 1)
    fps_format_source = (
        html[fps_format_start:fps_format_end]
        if fps_format_start >= 0 and fps_format_end > fps_format_start
        else ""
    )
    fps_sync_start = html.find("function syncCanvasDimensions(width, height)")
    fps_sync_end = html.find("\n\t\tfunction ", fps_sync_start + 1)
    fps_sync_source = (
        html[fps_sync_start:fps_sync_end]
        if fps_sync_start >= 0 and fps_sync_end > fps_sync_start
        else ""
    )
    fps_draw_start = html.find("function drawFpsOsd()")
    fps_draw_end = html.find("\n\t\tfunction ", fps_draw_start + 1)
    fps_draw_source = (
        html[fps_draw_start:fps_draw_end]
        if fps_draw_start >= 0 and fps_draw_end > fps_draw_start
        else ""
    )
    fps_glyph_start = html.find("const FPS_GLYPHS")
    fps_glyph_end = html.find("\n\t\t});", fps_glyph_start + 1)
    fps_glyph_source = (
        html[fps_glyph_start:fps_glyph_end]
        if fps_glyph_start >= 0 and fps_glyph_end > fps_glyph_start
        else ""
    )
    fps_sampler_start = html.find("function startFpsSampling()")
    fps_sampler_end = html.find("\n\t\tfunction ", fps_sampler_start + 1)
    fps_sampler_source = (
        html[fps_sampler_start:fps_sampler_end]
        if fps_sampler_start >= 0 and fps_sampler_end > fps_sampler_start
        else ""
    )
    require(
        fps_osd_tag is not None
        and 'aria-hidden="true"' in fps_osd_tag.group(0)
        and re.search(
            r"#screen,\s*#fps-osd\s*\{[^}]*border:\s*0;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#fps-osd\s*\{[^}]*z-index:\s*1;"
            r"[^}]*background:\s*transparent;"
            r"[^}]*pointer-events:\s*none;",
            html,
            re.DOTALL,
        ) is not None
        and "getContext('2d', { alpha: true })" in html
        and "const FPS_GLYPHS" in html
        and all(f"'{digit}':" in fps_glyph_source for digit in "0123456789")
        and "'.':" in fps_glyph_source
        and re.search(r"['\"][A-Za-z]", fps_glyph_source) is None
        and "clearRect(" in fps_draw_source
        and "fillRect(" in fps_draw_source
        and "fillText(" not in html
        and ".font" not in fps_draw_source
        and ".toFixed(1)" in fps_format_source
        and "FPS:" not in fps_format_source
        and "FPS:" not in fps_draw_source
        and "Math.max(0, fpsOsd.width - 40) + 2" in fps_draw_source
        and "const originY = 6;" in fps_draw_source
        and re.search(r"canvas\.width\s*=\s*width", fps_sync_source)
        is not None
        and re.search(r"canvas\.height\s*=\s*height", fps_sync_source)
        is not None
        and re.search(r"fps\w*\.width\s*=\s*width", fps_sync_source, re.IGNORECASE)
        is not None
        and re.search(r"fps\w*\.height\s*=\s*height", fps_sync_source, re.IGNORECASE)
        is not None
        and "if (resized)\n\t\t\t\tdrawFpsOsd();" in fps_sync_source
        and html.count("syncCanvasDimensions(width, height);") >= 2
        and "function updateFpsCounter(generation)" in html
        and "performance.now()" in html
        and "setInterval(" in fps_sampler_source
        and "() => updateFpsCounter(generation)" in fps_sampler_source
        and re.search(r",\s*1000\s*\);", fps_sampler_source) is not None
        and "if (generation !== fpsSampleGeneration)" in html
        and "fpsSampleGeneration++;" in html
        and "clearInterval(fpsSampleTimer)" in html
        and "context.putImageData(imageData, 0, 0);\n"
        "\t\t\trecordDisplayedFrame();" in html
        and "context.drawImage(bitmap, 0, 0, width, height);\n"
        "\t\t\t\trecordDisplayedFrame();" in html
        and "typeof status.show_fps === 'boolean'" in html,
        "canvas FPS OSD is not a numeric, transparent, painted-frame pixel overlay",
    )
    require(
        "openwrt-nes-key-bindings-v1" in html
        and "function normalizeStoredBindings(value)" in html
        and "slots.length !== 2" in html
        and "seen.has(code)" in html
        and "function beginBindingCapture(name, slot)" in html
        and "function captureKey(code)" in html
        and "window.localStorage.setItem(" in html
        and "Saved controls were invalid and have been reset to defaults." in html
        and "Restore defaults" in html
        and "document.querySelectorAll('[data-control]')" in html
        and "const id = CONTROL_IDS[button.dataset.control];" in html
        and "const activePointerButtons = new Map();" in html
        and "window.addEventListener('pointerup', releasePointer);" in html,
        "custom keyboard controls are not validated, persistent, or wired to NES IDs",
    )
    stage_start = html.find('<section id="game-stage"')
    stage_end = html.find("</section>", stage_start)
    stage = html[stage_start:stage_end] if stage_start >= 0 and stage_end > stage_start else ""
    toolbar_start = stage.find('<div class="display-toolbar">')
    toolbar_end = stage.find("</div>", toolbar_start)
    toolbar = (
        stage[toolbar_start:toolbar_end]
        if toolbar_start >= 0 and toolbar_end > toolbar_start
        else ""
    )
    require(
        'id="screen"' in stage
        and 'data-control="up"' in stage
        and 'data-control="a"' in stage
        and 'id="btn-fullscreen"' in stage,
        "fullscreen game stage does not contain video and touch controls",
    )
    require(
        'id="display-mode"' in html
        and 'value="4-3"' in html
        and 'value="16-9"' in html
        and 'data-display-mode="4-3"' in html
        and '.screen-frame[data-display-mode="16-9"]' in html
        and "openwrt-nes-display-mode-v1" in html
        and "function normalizeStoredDisplayMode(value)" in html
        and "JSON.stringify({ version: 1, mode: normalized })" in html
        and "screenFrame.dataset.displayMode = normalized;" in html,
        "4:3/16:9 picture mode is missing, unvalidated, or not persistent",
    )
    style_match = re.search(r"<style>([\s\S]*?)</style>", html)
    require(style_match is not None, "game client stylesheet is missing")
    style = re.sub(r"/\*[\s\S]*?\*/", "", style_match.group(1))
    selector_preludes = re.findall(r"([^{}]+)\{", style)
    require(
        not any(
            ":-webkit-full-screen" in prelude and "," in prelude
            for prelude in selector_preludes
        ),
        "vendor fullscreen pseudo-class is grouped in an unforgiving selector list",
    )
    require(
        re.search(
            r"\.screen-frame\s*\{[^}]*width:\s*96vw;"
            r"[^}]*max-width:\s*512px;[^}]*height:\s*72vw;"
            r"[^}]*max-height:\s*384px;"
            r"[^}]*aspect-ratio:\s*4\s*/\s*3;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r'\.screen-frame\[data-display-mode="16-9"\]\s*'
            r"\{[^}]*height:\s*54vw;[^}]*max-height:\s*288px;"
            r"[^}]*aspect-ratio:\s*16\s*/\s*9;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#screen,\s*#fps-osd\s*\{[^}]*position:\s*absolute;"
            r"[^}]*inset:\s*0;"
            r"[^}]*width:\s*100%;[^}]*height:\s*100%;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage\.fullscreen-active\s*\{[^}]*position:\s*fixed;"
            r"[^}]*inset:\s*0;[^}]*width:\s*100%;[^}]*height:\s*100%;"
            r"[^}]*min-width:\s*0;[^}]*min-height:\s*0;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage:fullscreen\s*\{[^}]*position:\s*fixed;"
            r"[^}]*inset:\s*0;[^}]*width:\s*100%;[^}]*height:\s*100%;"
            r"[^}]*min-width:\s*0;[^}]*min-height:\s*0;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage:-webkit-full-screen\s*\{[^}]*position:\s*fixed;"
            r"[^}]*inset:\s*0;[^}]*width:\s*100%;[^}]*height:\s*100%;"
            r"[^}]*min-width:\s*0;[^}]*min-height:\s*0;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage\.fullscreen-active\s+\.screen-frame\s*"
            r"\{[^}]*width:\s*96vw;[^}]*max-width:\s*512px;"
            r"[^}]*height:\s*72vw;[^}]*max-height:\s*384px;"
            r"[^}]*flex:\s*none;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage:fullscreen\s+\.screen-frame\s*"
            r"\{[^}]*width:\s*96vw;[^}]*max-width:\s*512px;"
            r"[^}]*height:\s*72vw;[^}]*max-height:\s*384px;"
            r"[^}]*flex:\s*none;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage:-webkit-full-screen\s+\.screen-frame\s*"
            r"\{[^}]*width:\s*96vw;[^}]*max-width:\s*512px;"
            r"[^}]*height:\s*72vw;[^}]*max-height:\s*384px;"
            r"[^}]*flex:\s*none;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r'#game-stage\.fullscreen-active\s+\.screen-frame'
            r'\[data-display-mode="16-9"\]\s*'
            r"\{[^}]*height:\s*54vw;[^}]*max-height:\s*288px;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r'#game-stage:fullscreen\s+\.screen-frame'
            r'\[data-display-mode="16-9"\]\s*'
            r"\{[^}]*height:\s*54vw;[^}]*max-height:\s*288px;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r'#game-stage:-webkit-full-screen\s+\.screen-frame'
            r'\[data-display-mode="16-9"\]\s*'
            r"\{[^}]*height:\s*54vw;[^}]*max-height:\s*288px;",
            html,
            re.DOTALL,
        ) is not None
        and "function fitDisplayBox(availableWidth, availableHeight, mode)" in html
        and "gameStage.clientWidth -" in html
        and "pixels(style.paddingLeft) - pixels(style.paddingRight)" in html
        and "gameStage.clientHeight -" in html
        and "pixels(style.paddingTop) - pixels(style.paddingBottom)" in html
        and "screenFrame.style.setProperty('width', width);" in html
        and "screenFrame.style.setProperty('max-width', width);" in html
        and "screenFrame.style.setProperty('height', height);" in html
        and "screenFrame.style.setProperty('max-height', height);" in html
        and "function setFullscreenLayout(active)" in html
        and "gameStage.classList.add('fullscreen-active');" in html
        and "gameStage.classList.remove('fullscreen-active');" in html
        and "function cancelFullscreenFit()" in html
        and "if (!pageActive || currentFullscreenElement() !== gameStage)" in html
        and "pageActive = false;" in html
        and "setFullscreenLayout(false);\n\t\t\tcancelFullscreenFit();" in html
        and "pageActive = true;\n\t\t\tscheduleFullscreenFit();" in html
        and re.search(
            r"#game-stage\.fullscreen-active\s+\.display-toolbar\s*"
            r"\{[^}]*position:\s*absolute;",
            html,
            re.DOTALL,
        ) is not None
        and re.search(
            r"#game-stage\.fullscreen-active\s+\.touch-controls\s*"
            r"\{[^}]*position:\s*absolute;",
            html,
            re.DOTALL,
        ) is not None
        and "window.addEventListener('resize', scheduleFullscreenFit);" in html
        and "window.addEventListener('orientationchange', scheduleFullscreenFit);" in html
        and "window.visualViewport.addEventListener('resize', scheduleFullscreenFit);" in html,
        "picture modes lack a safe-area-aware, non-aspect-ratio fullscreen fit",
    )
    require(
        "function fullscreenApi()" in html
        and "gameStage.requestFullscreen()" in html
        and "document.exitFullscreen()" in html
        and "gameStage.webkitRequestFullscreen()" in html
        and "document.webkitExitFullscreen()" in html
        and "document.fullscreenElement" in html
        and "document.webkitFullscreenElement" in html
        and "document.addEventListener('fullscreenchange'" in html
        and "document.addEventListener('webkitfullscreenchange'" in html
        and "document.addEventListener('fullscreenerror'" in html
        and "document.addEventListener('webkitfullscreenerror'" in html
        and "releaseAllControls();\n\t\t\tfullscreenTransition = true;" in html
        and "const FULLSCREEN_TIMEOUT_MS = 5000;" in html
        and "fullscreenTransitionTimer = setTimeout" in html
        and 'id="display-message" role="status"' in toolbar
        and 'aria-live="polite"' in toolbar
        and "#display-message:empty { display: none }" in html
        and re.search(
            r"#display-message\s*\{[^}]*pointer-events:\s*none;",
            html,
            re.DOTALL,
        ) is not None,
        "fullscreen handling lacks compatibility, input release, or accessible errors",
    )
    fullscreen_toolbar_selectors = (
        "#game-stage.fullscreen-active .display-toolbar",
        "#game-stage:fullscreen .display-toolbar",
        "#game-stage:-webkit-full-screen .display-toolbar",
    )
    visible_touch_toolbar_selectors = (
        '#game-stage.fullscreen-active[data-touch-controls="visible"] .display-toolbar',
        '#game-stage:fullscreen[data-touch-controls="visible"] .display-toolbar',
        '#game-stage:-webkit-full-screen[data-touch-controls="visible"] .display-toolbar',
    )
    require(
        'id="game-stage" data-touch-controls="visible"' in html
        and "gameStage.dataset.touchControls = hidden ? 'hidden' : 'visible';"
        in html
        and all(
            re.search(
                re.escape(selector)
                + r"\s*\{[^}]*top:\s*auto;[^}]*right:\s*auto;"
                + r"[^}]*bottom:\s*max\(8px,\s*env\(safe-area-inset-bottom\)\);"
                + r"[^}]*left:\s*50%;[^}]*transform:\s*translateX\(-50%\);",
                html,
                re.DOTALL,
            )
            is not None
            for selector in fullscreen_toolbar_selectors
        )
        and all(
            re.search(
                re.escape(selector)
                + r"\s*\{[^}]*top:\s*max\(8px,\s*env\(safe-area-inset-top\)\);"
                + r"[^}]*right:\s*auto;[^}]*bottom:\s*auto;"
                + r"[^}]*left:\s*max\(8px,\s*env\(safe-area-inset-left\)\);"
                + r"[^}]*transform:\s*none;[^}]*flex-direction:\s*column;"
                + r"[^}]*align-items:\s*stretch;"
                + r"[^}]*width:\s*calc\(100vw\s*-\s*64px\);"
                + r"[^}]*max-width:\s*13rem;[^}]*box-sizing:\s*border-box;",
                html,
                re.DOTALL,
            )
            is not None
            for selector in visible_touch_toolbar_selectors
        ),
        "fullscreen toolbar can obscure the FCEUX FPS OSD or touch controls",
    )
    lifecycle = read("tests/play_lifecycle_contract.js")
    require(
        "valid stored 16:9 picture mode was not restored" in lifecycle
        and "malformed stored picture mode was not repaired to 4:3" in lifecycle
        and "unknown stored picture-mode schema was accepted" in lifecycle
        and "picture-mode storage read failure did not use 4:3 safely" in lifecycle
        and "blocked picture-mode storage prevented the live selection" in lifecycle
        and "external fullscreen exit left stale button state" in lifecycle
        and "fullscreen rejection corrupted UI or connection status" in lifecycle
        and "hung fullscreen promise did not recover after its deadline" in lifecycle
        and "WebKit-prefixed fullscreen fallback is not functional" in lifecycle
        and "fullscreen entry did not release held controller input once" in lifecycle
        and "portrait 4:3 fullscreen fit distorted or escaped its content box" in lifecycle
        and "portrait 16:9 fullscreen fit distorted or escaped its content box" in lifecycle
        and "landscape 4:3 fullscreen fit distorted or escaped its content box" in lifecycle
        and "wide 16:9 fullscreen fit distorted or escaped its content box" in lifecycle
        and "invalid fullscreen geometry did not fail closed" in lifecycle
        and "runtime fullscreen selector must be an independent rule" in lifecycle
        and "class-only fullscreen layout clips the picture vertically" in lifecycle
        and "1920x1080 4:3 regression" in lifecycle
        and "fullscreen resize did not refit the active 4:3 picture" in lifecycle
        and "fullscreen orientation change retained stale geometry" in lifecycle
        and "live 16:9 fullscreen selection retained 4:3 geometry" in lifecycle
        and "late fullscreen fit RAF restored stale geometry after pagehide" in lifecycle,
        "fullscreen and picture-mode browser lifecycle regressions are not covered",
    )
    require(
        "FPS OSD exposed a browser widget" in lifecycle
        and "FPS OSD rendered a label instead of only the numeric value" in lifecycle
        and "FPS OSD did not use a transparent pixel renderer" in lifecycle
        and "FPS OSD dimensions did not follow the video canvas" in lifecycle
        and "FPS OSD counted received raw packets instead of one paint" in lifecycle
        and "disabled FPS OSD remained visible or kept sampling" in lifecycle
        and "FPS OSD did not count the two displayed JPEG frames" in lifecycle
        and "stale FPS sampler resurrected the OSD" in lifecycle,
        "canvas FPS OSD lifecycle regressions are not covered",
    )
    require(
        "let pendingJpeg = null;" in html
        and "let pendingRaw = null;" in html
        and "let rawPaintFrame = 0;" in html
        and "const MAX_OUTSTANDING_JPEG_DECODES = 2;" in html
        and "let outstandingJpegDecodes = 0;" in html
        and "const jpegWorkerEpochs = new Set();" in html
        and "const quarantinedJpegEpochs = new Set();" in html
        and "async function decodeJpegWithDeadline(data, epoch)" in html
        and "async function drainJpegQueue(epoch)" in html
        and "while (pendingJpeg && pageActive && epoch === videoEpoch" in html
        and "function queueJPEG(data, width, height)" in html
        and "let videoEpoch = 0;" in html
        and "let videoCodec = null;" in html
        and "epoch !== videoEpoch" in html
        and "const JPEG_DECODE_TIMEOUT_MS = 2000;" in html
        and "Promise.race([completion, deadline])" in html
        and "quarantinedJpegEpochs.add(epoch);" in html
        and "outstandingJpegDecodes < MAX_OUTSTANDING_JPEG_DECODES" in html
        and "function queueRaw(packet, byteOffset, width, height)" in html
        and "rawPaintFrame = requestAnimationFrame(paintPendingRaw);" in html
        and "cancelAnimationFrame(rawPaintFrame);" in html
        and "new DataView(packet, byteOffset, pixels * 2)" in html
        and "playPCM(buffer, 12, channels, rate, frames);" in html
        and "buffer.slice(12)" not in html
        and "function invalidateVideoFrames()" in html
        and "socket !== nextSocket" in html
        and "const generation = ++videoGeneration;" not in html,
        "browser media decoding can queue stale frames or copy every packet",
    )
    require(
        "const CONNECT_TIMEOUT_MS = 9000;" in html
        and "const HEARTBEAT_INTERVAL_MS = 1000;" in html
        and "const SERVER_SILENCE_TIMEOUT_MS = 9000;" in html
        and "const HEARTBEAT_ACK_FRESHNESS_MS = 3500;" in html
        and "const MAX_PENDING_HEARTBEATS = 8;" in html
        and "const STABLE_CONNECTION_MS = 5000;" in html
        and "const MAX_SOCKET_BUFFERED_BYTES = 64 * 1024;" in html
        and "const OFFLINE_RECONNECT_MIN_MS = 6000;" in html
        and "const OFFLINE_RECONNECT_MAX_MS = 8000;" in html
        and "const UPLOAD_TIMEOUT_BASE_MS = 40000;" in html
        and "const UPLOAD_TIMEOUT_BYTES_PER_SECOND = 8 * 1024;" in html
        and "const UPLOAD_TIMEOUT_MAX_MS = 1810000;" in html
        and "let lastHeartbeatAck = 0;" in html
        and "let heartbeatSequence = 0;" in html
        and "const pendingHeartbeats = new Map();" in html
        and "function abandonTransport(candidate" in html
        and "function heartbeat(candidate)" in html
        and "function acceptHeartbeatAck(candidate, seq)" in html
        and "if (status.t === 'heartbeat')" in html
        and "acceptHeartbeatAck(nextSocket, status.seq);" in html
        and "t: 'heartbeat'" in html
        and "mask: inputMask()" in html
        and "seq" in html
        and "heartbeatSequence >= Number.MAX_SAFE_INTEGER" in html
        and "pendingHeartbeats.set(seq, now);" in html
        and "candidate.bufferedAmount" in html
        and "buffered > MAX_SOCKET_BUFFERED_BYTES" in html
        and "connection stalled · reconnecting…" in html
        and "connection timed out · retrying…" in html
        and ".75 + Math.random() * .5" in html
        and "window.addEventListener('offline'" in html
        and "window.addEventListener('online'" in html
        and "networkOnline = navigator.onLine !== false;" in html
        and "browser reports offline · probing the router…" in html
        and "function uploadTimeoutMs(size)" in html
        and "uploadTimeoutMs(file.size)" in html
        and "130000" not in html
        and "!networkOnline" not in connect_source
        and "!networkOnline" not in heartbeat_source
        and "function resumeExistingAudio()" in html
        and "audioSuspendPending" in html
        and "audioResumePending" in html
        and "audioLifecycleGeneration" in html
        and "record.reconcile" in html
        and "clearScheduledAudio();" in html
        and "const AUDIO_INITIAL_LEAD_SECONDS = .06;" in html
        and "const AUDIO_RECOVERY_LEAD_SECONDS = .02;" in html
        and "const AUDIO_MIN_SCHEDULE_LEAD_SECONDS = .005;" in html
        and "const AUDIO_MAX_SCHEDULE_LEAD_SECONDS = .18;" in html
        and "audio.currentTime + AUDIO_MIN_SCHEDULE_LEAD_SECONDS" in html
        and "nextAudioTime = scheduledAt + buffer.duration;" in html,
        "browser transport watchdog, jittered reconnect, input renewal, or audio reset is missing",
    )
    require(
        all(
            marker in lifecycle
            for marker in (
                "JPEG decode queue starved progress or retained an old pending frame",
                "first JPEG timeout did not force one bounded transport recovery",
                "old quarantined JPEG decode blocked the new transport epoch",
                "second permanent JPEG hang did not stop at the explicit cap",
                "JPEG global cap leaked decoders or entered a reconnect storm",
                "fresh lagging heartbeat ACK was incorrectly rejected",
                "out-of-order heartbeat ACK moved liveness backwards",
                "missing heartbeat ACK survived downstream frames or retained audio",
                "delayed ACK from an old socket contaminated a new generation",
                "CONNECTING deadline did not force-abandon the hung socket",
                "one early reply incorrectly reset reconnect backoff",
                "WebSocket send exception left the failed transport active",
                "excessive WebSocket bufferedAmount was allowed to grow",
                "false offline hint released live LAN controller input",
                "false offline hint tore down a proven LAN transport or audio",
                "offline transition without a transport did not probe immediately",
                "repeated offline hint replaced an active CONNECTING LAN probe",
                "reachable LAN transport was rejected while navigator stayed offline",
                "offline reconnect probe ignored its low-frequency floor",
                "offline reconnect probe exceeded its capped delay",
                "visible transition ignored the resampled offline audio intent",
                "visibility restore did not create one stream and one audio resume",
                "stale audio resume did not reconcile the latest visible intent",
                "rapid hide/show sequence left audio or transport in a stale state",
                "persisted pageshow did not restore exactly one stream, preference probe and audio context",
                "excessive audio lead overlapped stale scheduled sources",
                "audio underrun was scheduled in the past or cut a live source",
                "audio recovery overlapped or separated consecutive PCM blocks",
                "mute did not cancel already scheduled audio",
                "fullscreen transition accepted a reentrant request",
                "BFCache pagehide did not settle fullscreen or preserve suspended audio",
            )
        ),
        "weak-network and stalled-browser lifecycle regressions are not covered",
    )
    require(
        "let gamepadMask = 0;" in html
        and "let windowFocused = typeof document.hasFocus !== 'function' ||"
        in html
        and "const GAMEPAD_CONTROLS = Object.freeze([" in html
        and "if (nextMask !== gamepadMask)" in html
        and "if (!windowFocused || document.hidden)" in html
        and "windowFocused = false;\n\t\t\treleaseAllControls();" in html
        and "Array.from(navigator.getGamepads" not in html,
        "gamepad polling allocates or retransmits unchanged input every frame",
    )


def check_licenses() -> None:
    root_license = read("LICENSE")
    notices = read("THIRD_PARTY_NOTICES.md")
    build_script = read("scripts/build-apks.sh")
    require(
        root_license.startswith("MIT License\n\n")
        and "Permission is hereby granted, free of charge" in root_license,
        "LICENSE is not a standard MIT license suitable for GitHub detection",
    )
    require(
        "statically\nlinks FCEUmm" in notices
        and "combined executable is distributed under GPL-2.0-only" in notices
        and "Each binary release must include the exact corresponding-source archive"
        in notices,
        "THIRD_PARTY_NOTICES.md does not explain the combined nesd license",
    )
    for relative in (
        "package/nes-emulator/files/LICENSE-MIT",
        "package/luci-app-nes-emulator/files/LICENSE-MIT",
    ):
        package_license = read(relative)
        require(
            package_license.startswith(root_license)
            and "FCEUmm is mandatory in release builds" in package_license
            and "combined\nexecutable is distributed under GPL-2.0-only"
            in package_license,
            f"{relative} does not preserve the MIT terms and package license scope",
        )
    require(
        'license_bytes="$(wc -c < "$project_root/LICENSE")"' in build_script
        and 'head -c "$license_bytes" "$package_license"' in build_script
        and "grep -Fq 'FCEUmm is mandatory in release builds'" in build_script
        and "grep -Fq 'GPL-2.0-only'" in build_script,
        "release metadata validation does not preserve MIT and GPL license scope",
    )


def check_security_regressions() -> None:
    source_roots = (
        ROOT / "package",
        ROOT / "scripts",
    )
    text_files: list[Path] = []
    for source_root in source_roots:
        for path in source_root.rglob("*"):
            if path.is_file() and path.suffix.lower() in {
                "", ".c", ".h", ".html", ".js", ".json", ".mk", ".sh"
            }:
                text_files.append(path)
    combined = "\n".join(
        path.read_text(encoding="utf-8", errors="replace") for path in text_files
    )
    forbidden = {
        "world-writable ROM directory": r"\bchmod\s+1777\b",
        "unscoped daemon kill": r"\bkillall\b",
        "wildcard CORS": r"Access-Control-Allow-Origin:\s*\*",
        "unpinned source branch": r"FCEUMM_(?:REF|VERSION)\s*=\s*(?:master|main)\b",
    }
    for label, pattern in forbidden.items():
        require(not re.search(pattern, combined, re.IGNORECASE), f"found {label}")

    custom_cgi = ROOT / "package/luci-app-nes-emulator/root/www/cgi-bin/nes-rom-upload"
    require(not custom_cgi.exists(), "unauthenticated custom upload CGI still exists")

    config = read("package/nes-emulator/files/nes-emulator.config")
    init = read("package/nes-emulator/files/nes-emulator.init")
    rpcd = read(
        "package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator"
    )
    settings = read(
        "package/luci-app-nes-emulator/htdocs/luci-static/resources/view/"
        "nes-emulator/settings.js"
    )
    main_source = read("package/nes-emulator/src/main.c")
    http_source = read("package/nes-emulator/src/http.c")
    http_header = read("package/nes-emulator/src/http.h")
    host_source = read("package/nes-emulator/src/host.c")
    integration = read("tests/integration.py")
    overview = read(
        "package/luci-app-nes-emulator/htdocs/luci-static/resources/view/"
        "nes-emulator/overview.js"
    )
    play_html = read("package/nes-emulator/src/play.html")
    require("option core " not in config, "runtime core path survived static migration")
    require(
        "option auth_token" not in config,
        "authentication token remains readable through UCI ACLs",
    )
    for source, label in ((init, "init"), (rpcd, "rpcd")):
        require(
            "TOKEN_FILE=/etc/nes-emulator/auth.token" in source
            and "-rw-r-----" in source
            and "root:nesd" in source
            and "0640" in source
            and "nes-emulator.main.auth_token" in source
            and "delete nes-emulator.main.auth_token" in source,
            f"{label} does not securely store/migrate the authentication token",
        )
        require(
            "TOKEN_LOCK_FILE=/var/lock/nes-emulator-token.lock" in source
            and "flock -n 8" in source
            and '.lock/owner' not in source,
            f"{label} authentication-token locking is not crash-safe",
        )
        require(
            not re.search(r"\bod\b", source)
            and "stat -c" not in source
            and "hexdump -v -n " in source,
            f"{label} relies on applets disabled in the stock OpenWrt BusyBox",
        )
    require(
        "repair_regular_metadata()" in init
        and '[ "$PATH_METADATA_NLINK" = 1 ]' in init
        and 'repair_regular_metadata "$TOKEN_FILE" -rw-r-----' in init,
        "init does not repair token metadata conditionally or reject hardlinks",
    )
    require(
        "validate_regular_metadata()" in rpcd
        and "validate_directory_metadata()" in rpcd
        and 'validate_regular_metadata "$TOKEN_FILE" -rw-r-----' in rpcd
        and 'elif [ ! -e "$TOKEN_FILE" ] && [ ! -L "$TOKEN_FILE" ]; then'
        in rpcd
        and rpcd.index(
            'elif [ ! -e "$TOKEN_FILE" ] && [ ! -L "$TOKEN_FILE" ]; then'
        )
        < rpcd.index('token="$(new_token)"', rpcd.index("ensure_auth_token()")),
        "rpcd mutates unsafe token metadata or cannot restore a missing token",
    )
    require(
        "tr '[:upper:]' '[:lower:]'" not in rpcd
        and "tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz'" in rpcd
        and "du -b" not in rpcd
        and "mv -fT" not in rpcd
        and "file_size_bytes()" in rpcd
        and "wc -c <\"$1\"" in rpcd
        and "-exec wc -c '{}' ';'" in rpcd
        and "-mmin +30 -o -newer" in rpcd
        and 'touch -d "@$future"' in rpcd,
        "rpcd relies on incomplete BusyBox features or cannot expire staged uploads",
    )
    require(
        '--auth-token-file "$TOKEN_FILE"' in init
        and "--auth-token-file" in main_source
        and "O_NOFOLLOW" in main_source
        and "status.st_uid != 0" in main_source
        and "status.st_nlink != 1" in main_source,
        "daemon token is exposed in argv or token-file validation is incomplete",
    )
    require("procd_set_param user nesd" in init, "daemon does not drop root privileges")
    require(
        "option enabled '0'" in config
        and "config_get_bool enabled main enabled 0" in init
        and "procd_set_param respawn" not in init
        and "NESD_ON_DEMAND=1 NESD_SKIP_AUTOLOAD=1" in rpcd
        and "nes-emulator.main.enabled=1" not in rpcd,
        "fresh installs can auto-start or repeatedly respawn the emulator",
    )
    require(
        "option extra_rom_dirs_enabled '0'" in config
        and "load_extra_rom_dirs()" in init
        and "config_get_bool extra_rom_dirs_enabled main "
        "extra_rom_dirs_enabled 0" in init
        and "scan_configured_extra_rom_roots()" in rpcd
        and "config_get_bool extra_rom_dirs_enabled main "
        "extra_rom_dirs_enabled 0" in rpcd
        and "if config_load nes-emulator; then" in rpcd
        and "'extra_rom_dirs_enabled'" in settings
        and "option.depends('extra_rom_dirs_enabled', '1');" in settings
        and "option.retain = true;" in settings
        and "option.validate = validateOptionalDataDir;" in settings
        and "value.includes(';')" in settings
        and "value.includes(',')" in settings
        and "*';'*" in init
        and "*';'*" in rpcd
        and "|*,*)" in init
        and "|*,*)" in rpcd
        and 'strtok_r(extras, ":;,"' in http_source
        and "delete nes-emulator.main.extra_rom_dir" not in init
        and "delete nes-emulator.main.extra_rom_dir" not in rpcd,
        "extra ROM directory gating or separator validation is inconsistent",
    )
    require(
        "option show_fps '1'" in config
        and "config_get_bool show_fps main show_fps 1" in init
        and "procd_append_param command --show-fps" in init
        and "procd_append_param command --hide-fps" in init
        and "'show_fps'" in settings
        and "_('Show FPS counter')" in settings
        and "FCEUX-like pixel OSD directly over the NES canvas" in settings
        and "without a separate browser widget" in settings
        and "delivery, decode and paint slowdowns" in settings
        and "option.default = option.enabled;" in settings
        and "OPT_SHOW_FPS" in main_source
        and "OPT_HIDE_FPS" in main_source
        and ".show_fps = 1" in main_source
        and "int show_fps;" in http_header
        and '\\"show_fps\\":%s' in http_source
        and 'srv->stream.show_fps ? "true" : "false"' in http_source,
        "FPS counter preference is not default-on or fully plumbed to the client",
    )
    require(
        "option show_touch_controls '1'" in config
        and re.search(
            r"section\.option\(\s*form\.Flag,\s*'show_touch_controls',\s*"
            r"_\('Show on-screen controls'\).*?\);\s*"
            r"option\.default = option\.enabled;\s*"
            r"option\.rmempty = false;",
            settings,
            re.DOTALL,
        )
        and "keyboard and gamepad controls remain available" in settings
        and "Save & Apply" in settings,
        "on-screen control preference is not an explicit default-on LuCI flag",
    )
    require(
        "config_get_bool show_touch_controls main show_touch_controls 1" in init
        and "procd_append_param command --show-touch-controls" in init
        and "procd_append_param command --hide-touch-controls" in init
        and "OPT_SHOW_TOUCH_CONTROLS" in main_source
        and "OPT_HIDE_TOUCH_CONTROLS" in main_source
        and ".show_touch_controls = 1" in main_source
        and "int show_touch_controls;" in http_header
        and '\\"show_touch_controls\\":%s' in http_source
        and 'srv->stream.show_touch_controls ? "true" : "false"'
        in http_source
        and ".touch-controls[hidden] { display: none !important }" in play_html
        and "function setTouchControlsVisible(visible)" in play_html
        and "gameStage.dataset.touchControls = hidden ? 'hidden' : 'visible';"
        in play_html
        and "function refreshDisplayPreferences()" in play_html
        and "fetchWithTimeout('/api/status'" in play_html
        and "typeof status.show_touch_controls === 'boolean'" in play_html
        and "displayPreferenceGeneration" in play_html
        and "if (touchControlsNode.hidden)" in play_html
        and "releasePointerControls();" in play_html,
        "on-screen control preference is not fully plumbed to the game client",
    )
    require(
        'if [ "${NESD_SKIP_AUTOLOAD:-0}" != 1 ]' in init
        and "startup_game_loaded = false" in main_source
        and "continuing without a loaded game" in main_source
        and "(allow_demo || startup_game_loaded)" in main_source,
        "a stale or invalid autoload ROM can still prevent on-demand recovery",
    )
    require(
        "select_http_client()" in rpcd
        and "NESD_BIN=/usr/bin/nesd" in rpcd
        and '"$NESD_BIN" --rpc-client' in rpcd
        and '--rpc-token-file "$TOKEN_FILE"' in rpcd
        and "uclient-fetch" not in rpcd
        and "daemon_is_ready()" in rpcd
        and "router-cpu-thin-client" in rpcd
        and '[ "$i" -ge 15 ] && break' in rpcd
        and "/etc/init.d/nes-emulator running" in rpcd
        and "/etc/init.d/nes-emulator preflight" in rpcd
        and "service_instance_state()" in rpcd
        and "set_native_start_error()" in rpcd
        and "START_ERROR=" in rpcd,
        "RPC startup still confuses missing transport, slow start, and daemon exit",
    )
    require(
        "START_LOCK_FILE=\"$UPLOAD_DIR/.start.lock\"" in rpcd
        and "acquire_start_lock()" in rpcd
        and "release_start_lock()" in rpcd
        and "flock -n 7" in rpcd
        and "validate_regular_metadata \"$START_LOCK_FILE\" -rw------- 0 0"
        in rpcd
        and "procd_open_instance nesd" in init
        and 'EXTRA_COMMANDS="preflight"' in init
        and '[ "${NESD_PREFLIGHT:-0}" = 1 ] && return 0' in init,
        "parallel startup or direct init preflight is not safely implemented",
    )
    for code in range(64, 72):
        require(
            re.search(rf"^\s*{code}\)$", rpcd, re.MULTILINE) is not None,
            f"rpcd does not map native startup exit code {code}",
        )
    require(
        "''|0.0.0.0) host=127.0.0.1" in rpcd
        and "0.0.0.0|127.*" not in rpcd,
        "RPC rewrites an exact loopback bind to a different address",
    )
    require(
        "path_is_readable_by_nesd()" in rpcd
        and "mode_allows_nesd()" in rpcd
        and 'json_add_boolean readable "$readable"' in rpcd,
        "offline root scan still advertises ROMs that nesd cannot read",
    )
    require(
        "expect: { '': { access: false } }" in overview
        and "expect: { '': { roms: [] } }" in overview
        and "expect: {}" not in overview,
        "LuCI RPC result extraction can hide permissions or ROMs",
    )
    require(
        "status.stream_fps" in overview
        and "status.fps ?" not in overview,
        "LuCI status confuses native emulation timing with media stream FPS",
    )
    require(
        "withTimeout(action(), 30000)" in overview
        and "withTimeout(callReserveUpload(), 20000)" in overview
        and "withTimeout(callRoms(), 15000)" in overview
        and "const result = await callImport(" in overview
        and not re.search(r"withTimeout\s*\(\s*callImport\s*\(", overview)
        and 'if [ "$attempts" -ge 3 ]; then' in rpcd,
        "LuCI timeouts can abandon non-cancellable actions or upload reservations",
    )
    play_view = read(
        "package/luci-app-nes-emulator/htdocs/luci-static/resources/view/"
        "nes-emulator/play.js"
    )
    require(
        "const accessError = access.error || '';" in play_view
        and "Cannot open the game client: %s" in play_view,
        "Play view hides daemon startup errors as permission failures",
    )
    require(
        "prepare_data_dir /etc/nes-emulator" in init
        and "/etc/nes-emulator)" in init
        and 'repair_directory_metadata "$d" 0 "$NESD_GID" root:nesd' in init
        and "/etc/nes-emulator/*)" in init,
        "package-owned parent directory is not repaired for offline installs",
    )
    require(
        'readlink -f "$d"' in init,
        "data directory ancestry is not checked for symlink traversal",
    )
    require(
        init.index('prepare_data_dir "$rom_dir"')
        < init.index('[ "$enabled" -eq 1 ]')
        and "normalize_managed_roms" not in init
        and "normalize_managed_roms" not in rpcd,
        "first-boot repair is late or read-only RPC mutates the ROM tree as root",
    )
    config = read("package/nes-emulator/files/nes-emulator.config")
    require(
        "option safety_migration '4'" in config,
        "fresh installs do not carry the current router-safety marker",
    )
    migration_blocks = []
    for relative in (
        "package/nes-emulator/Makefile",
        "scripts/build-apks.sh",
    ):
        packaging = read(relative).replace("$$", "$")
        require(
            "[ -f /etc/config/nes-emulator ]" in packaging
            and "[ ! -L /etc/config/nes-emulator ]" in packaging
            and '[ "$META_NLINK" = 1 ]' in packaging
            and "chown root:root /etc/config/nes-emulator" in packaging
            and "chmod 0600 /etc/config/nes-emulator" in packaging,
            f"{relative}: online post-install does not conditionally secure config",
        )
        require(
            'if [ -e "$dir" ] || [ -L "$dir" ]; then' in packaging
            and '[ -d "$dir" ] && [ ! -L "$dir" ]' in packaging
            and 'mkdir "$dir"' in packaging,
            f"{relative}: online post-install follows a data-directory symlink",
        )
        require(
            "repair_managed_rom_metadata()" in packaging
            and 'read_metadata "$rom" || return 0' in packaging
            and '[ "$META_NLINK" = 1 ] || return 0' in packaging
            and 'chown root:nesd "$rom" && chmod 0640 "$rom"' in packaging,
            f"{relative}: managed ROM metadata repair is unsafe or unconditional",
        )
        migration = extract_safety_migration(packaging)
        migration_blocks.append(migration)
        require(
            'migration="$(uci -q get '
            'nes-emulator.main.safety_migration 2>/dev/null)"' in migration
            and 'stream_format="$(uci -q get '
            'nes-emulator.main.stream_format 2>/dev/null)"' in migration
            and 'port="$(uci -q get '
            'nes-emulator.main.port 2>/dev/null)"' in migration
            and "migrate_port()" in migration
            and "migrate_extra_rom_dirs_enabled()" in migration
            and 'case "$migration" in' in migration
            and "4)" in migration
            and "3)" in migration
            and "2)" in migration
            and "1)" in migration
            and "9090|''|*[!0-9]*)" in migration
            and "uci -q set nes-emulator.main.port='29876'" in migration
            and "uci -q set nes-emulator.main.enabled='0'" in migration
            and VALID_STREAM_FPS_CASE in migration
            and "uci -q set nes-emulator.main.stream_fps='2'" in migration
            and '[ "$stream_format" = raw ] && [ "$stream_fps" = 5 ]'
            in migration
            and "uci -q set "
            "nes-emulator.main.extra_rom_dirs_enabled='0'" in migration
            and "uci -q set nes-emulator.main.safety_migration='4'" in migration
            and "delete nes-emulator.main.extra_rom_dir" not in migration
            and "Preserve configurations written by a newer package" in migration
            and "uci -q commit nes-emulator" in migration,
            f"{relative}: router-safety migration semantics regressed",
        )
        require(
            packaging.index("/etc/init.d/nes-emulator stop")
            < packaging.index("repair_config_metadata || exit 1")
            < packaging.index("# One-time router-safety migration;")
            < packaging.index("for rom in /etc/nes-emulator/roms/*"),
            f"{relative}: service is not stopped before metadata repair",
        )
    require(
        migration_blocks[0] == migration_blocks[1],
        "OpenWrt and standalone APK safety migrations differ",
    )
    readme = read("README.md") + "\n" + read("docs/TECHNICAL.md")
    require(
        "`safety_migration=4`" in readme
        and "previous marker `3`" in readme
        and "marker is already `4`" in readme
        and "`extra_rom_dirs_enabled=0`" in readme
        and "`9090`" in readme
        and "`29876`" in readme,
        "public documentation does not explain the current one-time safety migration",
    )
    require(
        "rate to 1–60 FPS" in readme
        and "59.0 Mbit/s at 60 FPS" in readme
        and "60.5 Mbit/s at 60 FPS" in readme
        and "Any other valid value from 1 through 60 is preserved" in readme
        and "next absolute video" in readme
        and "effective delivery ceiling never exceeds" in readme
        and re.search(
            r"Pending video never grows\s+beyond one frame", readme
        ) is not None
        and "strictly increasing safe-integer sequence number" in readme
        and "matching, recently issued acknowledgement" in readme
        and "`TCP_NOTSENT_LOWAT`" in readme
        and "uses `nice=5`" in readme
        and "bounded latest-only slot" in readme
        and "ordered three-slot FIFO capped at 120 ms" in readme
        and "periodic flash I/O" in readme,
        "public documentation does not cover the FPS ceiling, pacing, or migration",
    )
    require(
        "FCEUX-like pixel OSD" in readme
        and re.search(
            r"directly over the NES\s+canvas in the top-right corner;.*?"
            r"rather than a\s+separate DOM widget",
            readme,
            re.DOTALL,
        ) is not None
        and "delivery, decode, paint, or browser slowdowns" in readme
        and re.search(r"one-second sampling\s+timer", readme) is not None,
        "public documentation does not explain the canvas FPS OSD semantics",
    )
    require(
        not any(
            "CYRILLIC" in unicodedata.name(character, "")
            for character in readme
        ),
        "public documentation contains Cyrillic text instead of English",
    )
    for broad_root in ("/home", "/opt", "/run"):
        require(
            all(
                broad_root in source
                for source in (init, rpcd, settings, main_source, http_source)
            ),
            f"broad-root policy differs for {broad_root}",
        )
    require(
        "MAX_ROM_FIND_DEPTH=7" in rpcd
        and 'find "$ROM_DIR" -maxdepth 1' not in rpcd,
        "rpcd quota scan is shallower than the daemon scan",
    )
    require(
        "mkfifo \"$fifo\"" in rpcd
        and "tree_entry_count_is_bounded" in rpcd
        and "tree_has_boundary_directory" in rpcd
        and "nes-rom-list." not in rpcd,
        "rpcd can fill router tmpfs before enforcing ROM scan limits",
    )
    require(
        'readlink -f "$root" 2>/dev/null' in rpcd
        and 'valid_data_dir "$resolved"' in rpcd,
        "offline ROM scan can follow a configured symlink into a broad root",
    )
    require(
        "if ! rm -f \"$staged\" 9>&-; then" in rpcd
        and rpcd.index("if ! rm -f \"$staged\" 9>&-; then")
        < rpcd.index("if ! sync 9>&-; then"),
        "a durability warning can leave an upload reservation blocking future uploads",
    )
    require(
        "atomic_exchange_explicit(&h->viewers" in host_source
        and "wake_generation" in host_source,
        "unchanged viewer counts can break native core pacing",
    )
    require(
        'worker_load_core(h, "builtin")' in host_source
        and 'host_init(host, allow_demo ? "builtin" : NULL' in main_source
        and "(allow_demo || startup_game_loaded)" in main_source,
        "FCEUmm or demo emulation is still started eagerly without a ROM",
    )
    require(
        "atomic_init(&h->demo_mode, false)" in host_source
        and "if (!atomic_load_explicit(&h->game_loaded" in host_source
        and "enable_demo_mode(h)" in host_source,
        "lazy host state can misreport demo mode or run the core without a game",
    )
    require(
        "sched_setaffinity" not in host_source
        and 'open("/proc/self/oom_score_adj"' in host_source
        and '"800\\n"' in host_source
        and "#define NES_ROUTER_NICE 5" in host_source
        and "nice(NES_ROUTER_NICE - current_nice)" in host_source
        and "procd_set_param nice 5" in init
        and "procd_set_param nice 15" not in init
        and "nice(15)" not in host_source,
        "nesd can contend with router IRQ placement or survive ahead of network daemons",
    )
    require(
        "#define NES_SRAM_THREAD_STACK_BYTES (256u * 1024u)" in host_source
        and "pthread_t sram_thread;" in host_source
        and "static void *sram_thread_fn(void *argument)" in host_source
        and "result = write_sram_snapshot(h, name, data, size);" in host_source
        and host_source.count(
            "result = write_sram_snapshot(h, name, data, size);"
        )
        == 1
        and "free(p->sram_pending_data);" in host_source
        and "p->sram_pending_data = snapshot;" in host_source
        and "p->sram_completed_seq = sequence;" in host_source
        and "return wait ? wait_for_sram_sequence(p, sequence) : 0;"
        in host_source
        and "(void)worker_flush_sram(h, false, false);" in host_source
        and "worker_flush_sram(h, true, true)" in host_source
        and "sram_join_error = stop_sram_thread(h);" in host_source,
        "periodic SRAM persistence can block emulation or lose shutdown durability",
    )
    require(
        "#define MAX_CLIENTS 8" in http_source
        and "#define MAX_STREAM_CLIENTS 1" in http_source
        and "another game stream is already active" in http_source
        and "load a ROM before starting" in http_source
        and "#define NES_STREAM_FPS_MIN 1" in http_header
        and "#define NES_STREAM_FPS_MAX 60" in http_header
        and "#define NES_STREAM_FPS_DEFAULT 2" in http_header
        and "srv->stream.stream_fps = NES_STREAM_FPS_DEFAULT" in http_source
        and "srv->stream.stream_fps < NES_STREAM_FPS_MIN" in http_source
        and "srv->stream.stream_fps > NES_STREAM_FPS_MAX" in http_source
        and "streamed FPS limit 1-60 (default 2)" in main_source
        and "parse_int(optarg, NES_STREAM_FPS_MIN," in main_source
        and "NES_STREAM_FPS_MAX, &stream.stream_fps" in main_source
        and "normalize_stream_fps()" in init
        and '[ "$value" -le 60 ]' in init
        and 'stream_fps="$(normalize_stream_fps "$stream_fps")"' in init
        and "option.datatype = 'range(1,60)';" in settings
        and "Allowed range: 1–60 FPS." in settings
        and "59.0 Mbit/s at 60 FPS" in settings
        and "60.5 Mbit/s total" in settings
        and (
            "if (srv->stream.stream_fps < NES_STREAM_FPS_MIN ||\n"
            "\t    srv->stream.stream_fps > NES_STREAM_FPS_MAX)\n"
            "\t\tsrv->stream.stream_fps = NES_STREAM_FPS_DEFAULT;"
        )
        in http_source
        and "#define MAX_UPLOAD_DEFAULT NES_MAX_ROM_BYTES" in http_source
        and "#define NES_MAX_ROM_BYTES (16u * 1024u * 1024u)" in read(
            "package/nes-emulator/src/host.h"
        )
        and "MIN_FREE_DEFAULT (8ull * 1024ull * 1024ull)" in http_source,
        "client, upload, streaming, bandwidth, or free-space limits regressed",
    )
    require(
        '\\"readable\\":%s' in http_source
        and "ROM is not readable by nesd" in http_source
        and "path_result == -2" in http_source
        and "item.readable === false" in overview
        and "item.readable === false" in play_html
        and 'unreadable_rom = rom_dir / "root-only.nes"' in integration
        and "status == 403" in integration,
        "unreadable ROMs can disappear again or be offered as loadable",
    )
    require(
        "clock_gettime(CLOCK_MONOTONIC, &ts)" in http_source
        and "struct video_pacer video_pacer" in http_source
        and "static int effective_stream_fps(struct nes_http *srv)" in http_source
        and "video_pacer_due(pacer, stream_fps, viewers)" in http_source
        and "#define WS_MEDIA_POLL_MAX_MS 20" in http_source
        and "#define IDLE_POLL_MAX_MS 250" in http_source
        and "video_pacer_poll_timeout_ms(&video_pacer," in http_source
        and "if (!pacer->next_deadline_ns)\n\t\treturn fallback_ms;"
        in http_source
        and "remaining_ns % 1000000ull" in http_source
        and "1000000000ull + (uint64_t)stream_fps - 1ull" in http_source
        and "(now - pacer->next_deadline_ns) / period + 1ull" in http_source
        and "missed slot instead of emitting a catch-up burst" in http_source
        and "frame_id != pacer->last_video_id" in http_source
        and "static int websocket_queue_video(" in http_source
        and "free(c->video_out);" in http_source
        and "c->video_out = frame;" in http_source
        and "websocket_activate(c, frame, frame_len, 0x2, WS_OUT_VIDEO);"
        in http_source
        and "owner_pid != getpid()" in http_source
        and "cleanup_abandoned_uploads(srv->rom_dir)" in http_source
        and "AT_SYMLINK_NOFOLLOW" in http_source
        and "st.st_nlink != 1" in http_source
        and "st.st_uid != geteuid()" in http_source,
        "stream cadence, frame backpressure, or abandoned-upload cleanup regressed",
    )
    require(
        "#define WS_SOCKET_SNDBUF (64 * 1024)" in http_source
        and "#define WS_FLUSH_BUDGET_BYTES (128u * 1024u)" in http_source
        and "#define WS_FLUSH_BUDGET_SYSCALLS 4u" in http_source
        and "static void websocket_tune_socket(int fd)" in http_source
        and "TCP_NODELAY" in http_source
        and "TCP_USER_TIMEOUT" in http_source
        and "TCP_NOTSENT_LOWAT" in http_source
        and "c->out_progress_ms = progressed_at;" in http_source
        and "WS_OUTPUT_STALL_TIMEOUT_MS" in http_source
        and "WS_OUTPUT_MAX_AGE_MS" in http_source
        and "websocket_expire_stale_media(&clients[i], now);" in http_source
        and "(!clients[i].out || clients[i].out_kind != WS_OUT_VIDEO)" in http_source
        and "pollfds[poll_count].events = POLLIN |" in http_source
        and "if (!clients[i].dead && (events & POLLIN))" in http_source
        and "if (events & POLLOUT)" in http_source,
        "slow-reader backpressure or input-before-output scheduling regressed",
    )
    require(
        "#define WS_HEARTBEAT_SEQ_MAX 9007199254740991ull" in http_source
        and "#define WS_INPUT_LEASE_MS 3500ull" in http_source
        and "#define WS_APPLICATION_HEARTBEAT_TIMEOUT_MS 6000ull" in http_source
        and "uint64_t last_heartbeat_seq;" in http_source
        and "int application_heartbeat_required;" in http_source
        and "static int websocket_queue_heartbeat(" in http_source
        and '\\"t\\":\\"heartbeat\\",\\"seq\\":%llu' in http_source
        and 'strcmp(type, "heartbeat") == 0' in http_source
        and 'json_get_uint(text, "seq", &sequence) != 1' in http_source
        and "sequence <= c->last_heartbeat_seq" in http_source
        and "websocket_queue_heartbeat(c, sequence)" in http_source
        and "c->application_heartbeat_required = 1;" in http_source
        and "clients[i].application_heartbeat_required" in http_source
        and "static int expire_input_leases(" in http_source
        and "WS_PONG_TIMEOUT_MS" in http_source
        and "c->ping_outstanding = 0;" in http_source,
        "bidirectional heartbeat, input lease, or matched Pong handling regressed",
    )
    require(
        "c->out_kind = WS_OUT_HTTP;" in http_source
        and "HTTP_OUTPUT_STALL_TIMEOUT_MS" in http_source
        and "HTTP_OUTPUT_MAX_AGE_MS" in http_source
        and "ROM_SCAN_ACTIVE_BUDGET_NS" in http_source
        and "ROM_SCAN_IDLE_BUDGET_NS" in http_source
        and '\\"truncated\\":%s' in http_source
        and "UPLOAD_INACTIVITY_TIMEOUT_MS" in http_source
        and "UPLOAD_MIN_PROGRESS_BYTES_PER_SEC (8ull * 1024ull)" in http_source
        and "UPLOAD_MAX_TIMEOUT_MS 1800000ull" in http_source
        and "c->upload_absolute_deadline_ms" in http_source
        and "c->last_rx_ms +" in http_source
        and "#define AUDIO_PULL_FRAMES 4800u" in http_source
        and "#define AUDIO_PACKET_PERIOD_NS (40ull * 1000ull * 1000ull)"
        in http_source
        and "#define WS_AUDIO_QUEUE_SLOTS 3u" in http_source
        and "#define WS_AUDIO_QUEUE_MAX_AGE_MS 120ull" in http_source
        and "#define WS_AUDIO_QUEUE_MAX_DURATION_US (120ull * 1000ull)"
        in http_source
        and "static void websocket_audio_drop_oldest(" in http_source
        and "static int websocket_audio_take_oldest(" in http_source
        and "static size_t pull_latest_audio(" in http_source
        and "retained_channels != *channels || retained_rate != *rate"
        in http_source
        and "*rate = retained_rate;" in http_source
        and "*channels = retained_channels;" in http_source,
        "nonblocking HTTP, bounded ROM scan/upload, or fresh audio cadence regressed",
    )
    require(
        all(
            marker in integration
            for marker in (
                "heartbeat overwrote the reset event",
                "slow HTTP/WebSocket output stalled the control plane",
                "PCM packet cadence regressed into browser-GC churn",
                "heartbeat did not renew the held input lease",
                "JPEG stream starved heartbeat reply",
                "PAL output retained an irregular 60 Hz sampling phase",
                "slow client received a second pre-captured video",
                "test_browser_heartbeat_reclaims_stream",
                "application heartbeat deadline did not reclaim the stale",
                "replacement stream did not receive heartbeat ACK",
                'b\'{"t":"heartbeat","mask":0}\'',
                'b\'{"t":"heartbeat","mask":0,"seq":9007199254740992}\'',
            )
        ),
        "weak-network server integration regressions are not covered",
    )
    host_header = read("package/nes-emulator/src/host.h")
    host_audio_contract = read("tests/host_audio_contract.c")
    http_audio_contract = read("tests/http_audio_metadata_contract.c")
    jpeg_worker_contract = read("tests/http_jpeg_worker_contract.c")
    check_script = read("scripts/check.sh")
    status_json_source = http_source.split(
        "static char *make_status_json", 1
    )[1].split("static void handle_status", 1)[0]
    require(
        "#define NES_AUDIO_RECENT_MS 100u" in host_header
        and "atomic_uint joy[2];" in host_header
        and "unsigned sram_temp_serial;" in host_source
        and "unsigned state_temp_serial;" in host_source
        and "unsigned temp_serial;" not in host_source
        and "atomic_load_explicit(&h->joy[port], memory_order_relaxed)"
        in host_source
        and "atomic_store_explicit(&h->joy[port], mask, memory_order_relaxed)"
        in host_source
        and "h->audio_count > recent_frames" in host_source
        and "h->audio_count = recent_frames;" in host_source
        and "pthread_mutex_lock(&h->priv->state_mu);" in host_source
        and "pthread_mutex_lock(&h->audio_mu);" in host_source
        and "10000 - 4800" in host_audio_contract
        and "test_empty_pull_does_not_relabel_pcm" in http_audio_contract
        and "test_latest_full_chunk_owns_metadata" in http_audio_contract
        and "test_normal_pacer_jitter_keeps_contiguous_pcm" in http_audio_contract
        and "test_pal_sixty_ms_batch_keeps_contiguous_pcm"
        in http_audio_contract
        and "test_audio_fifo_preserves_order_behind_partial_video"
        in http_audio_contract
        and "test_audio_fifo_drops_oldest_at_budget" in http_audio_contract
        and "test_audio_fifo_keeps_full_priority_order"
        in http_audio_contract
        and "test_audio_fifo_expires_only_stale_prefix"
        in http_audio_contract
        and "sh tests/host_audio_contract.sh" in check_script
        and "sh tests/http_audio_metadata_contract.sh" in check_script,
        "host audio backlog, hot input path, or concurrent temp names regressed",
    )
    require(
        "struct nes_jpeg_worker {" in http_source
        and "jpeg_length = worker->encode(" in http_source
        and "jpeg_encode_rgb565(frame_scratch" not in http_source
        and "(!worker->pending_ready || worker->completed_ready)" in http_source
        and "worker->pending_ready = 1;" in http_source
        and "jpeg_worker_signal_network(worker);" in http_source
        and "jpeg_worker_enable_optional" in http_source
        and "falling back to raw RGB565" in http_source
        and "srv->stream.use_jpeg = 0;" in http_source
        and "NES_HTTP_MAX_LISTENERS + MAX_CLIENTS + 1" in http_source
        and "jpeg_worker_drain_wake(&jpeg_worker);" in http_source
        and "jpeg_worker_reset(srv ? srv->jpeg_worker : NULL);" in http_source
        and "assert_control_queues_remain_responsive" in jpeg_worker_contract
        and "worker.pending_frame_id == 3" in jpeg_worker_contract
        and "jpeg_worker_set_session(&worker, 0);" in jpeg_worker_contract
        and "packet[0] == NES_PKT_VIDEO_RAW" in jpeg_worker_contract
        and "destroy_worker_thread" in jpeg_worker_contract
        and "failing_worker_init" in jpeg_worker_contract
        and "server.stream.use_jpeg == 0" in jpeg_worker_contract
        and "sh tests/http_jpeg_worker_contract.sh" in check_script,
        "asynchronous latest-only JPEG worker or its regression coverage regressed",
    )
    require(
        "uint64_t frame_id;" in host_header
        and "status->frame_id = h->frame_id;" in host_source
        and "status.frame_id" in status_json_source
        and "host_copy_frame" not in status_json_source,
        "status generation again copies a complete video frame",
    )
    require(
        'idle.get("running") is False' in integration
        and 'idle.get("demo") is False' in integration
        and '"no-ROM WebSocket emitted A/V"' in integration
        and "status == 503" in integration
        and 'idle.get("stream_fps") == 60' in integration
        and '[str(binary), "--stream-fps", "61"]' in integration
        and "rejected_fps.returncode == 2" in integration
        and "pacing_elapsed * 40" in integration
        and "video_packets <=" in integration
        and "short_video_intervals" in integration
        and "interval < 0.013" in integration
        and "same_pid_abandoned_upload" in integration
        and "upload_hardlink" in integration,
        "integration tests do not protect router-safe idle and streaming behavior",
    )
    require(
        "test_idle_exit" in integration
        and '"--idle-exit-seconds", "1"' in integration
        and "time spent connected was incorrectly charged to idle timeout"
        in integration
        and "loaded game was terminated as idle" in integration
        and "video_pacer_poll_timeout_ms(&video_pacer," in http_source
        and "media_active ? WS_MEDIA_POLL_MAX_MS : IDLE_POLL_MAX_MS"
        in http_source
        and "(connected || game_loaded || clients_destroyed)" in http_source,
        "on-demand idle exit or its full-disconnect-timeout regression test is missing",
    )
    require(
        "test_invalid_startup_rom_recovers" in integration
        and '"--rom", str(invalid_rom)' in integration
        and '"/api/load"' in integration,
        "integration tests do not cover recovery from a stale autoload ROM",
    )
    require(
        "test_startup_diagnostics_and_small_stack" in integration
        and "resource.RLIMIT_STACK" in integration
        and "collision.returncode == 67" in integration
        and "missing_token.returncode == 64" in integration
        and "host = calloc(1, sizeof(*host))" in main_source
        and "struct nes_host host;" not in main_source
        and "NES_EMU_THREAD_STACK_BYTES" in host_source,
        "startup memory and stable diagnostic regressions are not protected",
    )
    require(
        "29876" in config
        and "config_get port main port '29876'" in init
        and "int port = 29876;" in main_source
        and "port > 0 ? port : 29876" in http_source
        and "PORT=29876" in rpcd
        and "option.default = '29876';" in settings
        and "access.port || '29876'" in play_view,
        "default daemon port differs across config, native code, rpcd, or LuCI",
    )
    build_script = read("scripts/build-apks.sh")
    require(
        "printf '%s\\n' '('"
        in build_script
        and "printf '%s\\n' ') || exit $?'"
        in build_script
        and "printf '%s\\n' 'default_postinst'"
        in build_script,
        "standalone APK custom post-install does not precede default service startup",
    )
    require(
        "nesd:nesd" in read("package/nes-emulator/Makefile")
        and "'nesd:nesd'" in build_script
        and "nesd=9090" not in build_script,
        "fixed service uid/gid can collide with vendor OpenWrt accounts",
    )
    luci_package = read("package/luci-app-nes-emulator/Makefile")
    require(
        "/etc/init.d/rpcd reload" in luci_package
        and "/etc/init.d/rpcd restart" not in luci_package
        and "/etc/init.d/uhttpd restart" not in luci_package,
        "LuCI installation still interrupts rpcd/uhttpd instead of reloading safely",
    )
    runtime_package_sources = "\n".join((luci_package, init, rpcd))
    for service in ("network", "firewall", "uhttpd"):
        require(
            f"/etc/init.d/{service} restart" not in runtime_package_sources
            and f"/etc/init.d/{service} reload" not in runtime_package_sources,
            f"package lifecycle unexpectedly mutates the global {service} service",
        )
    require(
        "/etc/config/network" not in runtime_package_sources
        and "/etc/config/firewall" not in runtime_package_sources
        and "/etc/config/uhttpd" not in runtime_package_sources,
        "package lifecycle unexpectedly mutates a global router configuration",
    )
    require(
        "move_output_directory()" in build_script
        and "remove_output_directory()" in build_script
        and "PUBLISH_MOVE_ATTEMPTS=8" in build_script
        and 'mv -T -n -- "$source" "$destination"' in build_script
        and build_script.count("move_output_directory ") >= 4,
        "APK output publication does not tolerate transient DrvFS rename failures",
    )
    require(
        "E('iframe'" not in play_view,
        "LuCI embeds a page that nesd intentionally denies framing",
    )
    require(
        "Connection help" in play_view
        and "The game client opens in a separate local window" in play_view
        and "This LuCI session uses HTTPS" not in play_view,
        "healthy HTTPS Play access is still presented as a service warning",
    )
    require(
        "Connection help" in overview
        and "nesd serves plain HTTP on the LAN" not in overview,
        "Overview still presents optional connection help as a service warning",
    )
    readme = read("README.md") + "\n" + read("docs/TECHNICAL.md")
    require(
        "The normal LuCI client uses same-origin access." not in readme
        and "The standalone game client uses same-origin access" in readme,
        "public documentation misstates the client origin boundary",
    )

    acl_document = json.loads(
        read(
            "package/luci-app-nes-emulator/root/usr/share/rpcd/acl.d/"
            "luci-app-nes-emulator.json"
        )
    )
    emulator_acl = acl_document["luci-app-nes-emulator"]
    read_acl = emulator_acl["read"]
    write = emulator_acl["write"]
    require(
        "access" not in read_acl.get("ubus", {}).get("nes-emulator", []),
        "read-only LuCI ACL can disclose the daemon bearer token",
    )
    require(
        read_acl.get("ubus", {}).get("nes-emulator") == ["status", "roms"],
        "read-only LuCI ACL grants more than status and ROM listing",
    )
    require(
        read_acl.get("uci") == ["nes-emulator"],
        "read-only LuCI sessions cannot render the token-free UCI model",
    )
    require(
        "access" in write.get("ubus", {}).get("nes-emulator", []),
        "game client token is not restricted to control permission",
    )
    require("service" not in write.get("ubus", {}), "ACL grants generic service control")
    require(
        write.get("file") == {"/tmp/nes-emulator-upload/*": ["write"]},
        "upload ACL is not restricted to the private staging directory",
    )
    require(
        all(
            method in write.get("ubus", {}).get("nes-emulator", [])
            for method in ("reserve_upload", "discard_upload", "import")
        )
        and "staged_path_is_valid" in rpcd
        and "reserve_upload()" in rpcd,
        "unique LuCI upload reservations are not enforced end to end",
    )
    require(
        "acquire_upload_lock" in rpcd
        and "release_upload_lock" in rpcd
        and "reserve_upload_locked" in rpcd
        and "discard_upload_locked" in rpcd
        and "import_rom_locked" in rpcd
        and "UPLOAD_LOCK_FILE=/var/lock/nes-emulator-upload.lock" in rpcd
        and "flock -n 9" in rpcd
        and 'storage_is_available "$dest" 0' in rpcd
        and "if ! sync 9>&-; then" in rpcd,
        "parallel LuCI imports can race the ROM quota or one reservation",
    )
    require(
        "json_load_without_upload_fd" in rpcd
        and "json_dump_without_upload_fd" in rpcd
        and rpcd.count("exec 9>&-") >= 15
        and rpcd.count('2>/dev/null 9>&- &') >= 3
        and "sleep 1 9>&-" in rpcd
        and "sync 9>&-" in rpcd,
        "a child process can inherit fd 9 and keep the upload flock alive",
    )
    require(
        "id -u nesd" in rpcd
        and '[ "$nlink" = 1 ]' in rpcd
        and "-rw-------|-rw-r-----" in rpcd
        and '0|"$nesd_uid"' in rpcd
        and '[ -f "$tmp" ] && [ ! -L "$tmp" ]' in rpcd,
        "interrupted import cleanup can remove an untrusted filesystem entry",
    )
    require(
        'strcmp(c->path, "/api/input")' not in http_source
        and "!clients[i].closing" in http_source,
        "client input can remain held after its WebSocket starts closing",
    )
    require(
        "#define MAX_ACTIVE_UPLOADS 1" in http_source
        and "active_upload_count(clients, client_count)" in http_source
        and "CLIENT_HTTP_DISCARD" in http_source
        and "read_http_discard" in http_source
        and http_source.count("reject_http_request(srv, c, &request") >= 8
        and "#define REJECT_DRAIN_MAX MAX_UPLOAD_DEFAULT" in http_source
        and "shutdown(c->fd, SHUT_WR)" in http_source
        and '[ "$count" -lt 1 ]' in rpcd,
        "concurrent uploads can exhaust storage or lose the bounded 409 response",
    )
    require(
        all(
            marker in integration
            for marker in (
                "unauthorized.nes",
                "one.nes&name=two.nes",
                "MAX_UPLOAD_BYTES + 1",
                "64 * 1024 + 1",
            )
        ),
        "early HTTP rejection body-drain regressions are not integration-tested",
    )
    require(
        '[ "$size" -ge 32 ] && [ "$magic" = 554e4946 ]' in rpcd
        and 'mv -f "$tmp" "$dest"' in rpcd
        and '[ -f "$dest" ] && [ ! -L "$dest" ]' in rpcd
        and rpcd.index('[ -f "$dest" ] && [ ! -L "$dest" ]')
        < rpcd.index('mv -f "$tmp" "$dest"'),
        "rpcd import accepts truncated UNIF files or unsafe destinations",
    )
    require(
        "nesd is the final path-security boundary" in rpcd
        and "canonical_path_allowed" in http_source
        and "realpath(path, resolved)" in http_source
        and "path_under_root(resolved, srv->rom_roots[i])" in http_source
        and "O_RDONLY | O_CLOEXEC | O_NOFOLLOW" in http_source
        and "rom_fd_valid(fd, canonical" in http_source
        and "outside-root.nes" in integration
        and "outside-link.nes" in integration,
        "forwarded LuCI load paths are not demonstrably confined by nesd",
    )
    require(
        'dd if="$staged" of="$tmp" bs=4096 count=4097' in rpcd
        and '[ "$actual_size" -ne "$size" ]' in rpcd
        and 'rom_is_valid "$tmp" "$ext" "$actual_size"' in rpcd,
        "rpcd import does not snapshot and revalidate the staged ROM safely",
    )
    require(
        "development-only and restricted" in main_source
        and "memset(auth_token_argument, 0, token_length)" in main_source,
        "argv authentication tokens are permitted on LAN or left in process args",
    )
    require(
        "*cursor <= 0x20 || *cursor >= 0x7f" in http_source,
        "configured CORS Origin can inject control bytes into HTTP headers",
    )
    require(
        "#define ACCEPT_BUDGET " in http_source
        and "accepted++ < ACCEPT_BUDGET" in http_source,
        "unbounded accept loop can starve established WebSocket clients",
    )
    require(
        "status_out" in http_source
        and "opcode & 0x08" in http_source
        and "opcode == 0x0a" in http_source,
        "WebSocket status traffic can displace required Ping/Pong controls",
    )
    require(
        "priority && c->out_off == 0" not in http_source,
        "queued status or control traffic can replace an active WebSocket frame",
    )


def check_static_build() -> None:
    package = read("package/nes-emulator/Makefile")
    source_makefile = read("package/nes-emulator/src/Makefile")
    build_script = read("scripts/build-apks.sh")
    commit = re.search(r"^FCEUMM_COMMIT:=([0-9a-f]{40})$", package, re.MULTILINE)
    checksum = re.search(r"^PKG_HASH:=([0-9a-f]{64})$", package, re.MULTILINE)
    require(commit is not None, "FCEUmm is not pinned to a full commit")
    require(checksum is not None, "FCEUmm source archive hash is missing")
    require("fceumm_bind.c" in source_makefile, "static FCEUmm binding is not linked")
    require(
        "rpc_client.c" in source_makefile,
        "self-contained local RPC client is not linked into nesd",
    )
    require("libfceumm.a" in source_makefile, "static FCEUmm archive is not built")
    require("FCEUMM_DIR" in source_makefile, "static core source tree is not required")
    require(
        'LDFLAGS="$(TARGET_LDFLAGS) -static -Wl,--gc-sections' in package
        and "-Wl,-z,stack-size=2097152" in package,
        "OpenWrt SDK builds are not fully static with a router-safe stack",
    )
    require(
        'LDLIBS="-ldl -lpthread -lm -latomic"' in package,
        "OpenWrt SDK builds cannot resolve C11 atomics on legacy targets",
    )
    require(
        '"luci-base rpcd jshn jsonfilter cgi-io nes-emulator=$APK_VERSION"'
        in build_script
        and "uclient-fetch" not in read("package/luci-app-nes-emulator/Makefile")
        and "EXTRA_DEPENDS:=nes-emulator "
        "(=$(PKG_VERSION)-r$(PKG_RELEASE))"
        in read("package/luci-app-nes-emulator/Makefile")
        and 'grep -Fxc "      - nes-emulator=$APK_VERSION"' in build_script
        and "for dependency in luci-base rpcd jshn jsonfilter cgi-io" in build_script
        and '"libc luci-base rpcd nes-emulator"' not in build_script
        and re.search(
            r'"NES emulator daemon with statically linked FCEUmm"\s*\\\n\s*""\s*\\',
            build_script,
        ),
        "standalone static APK metadata still requires a shared libc package",
    )
    require(
        "apk add ./nes-emulator-$APK_VERSION.apk "
        "./luci-app-nes-emulator-$APK_VERSION.apk"
        in build_script
        and "apk --update-cache --wait 120 add luci-base rpcd jshn jsonfilter cgi-io"
        in build_script
        and "apk --repositories-file /dev/null --no-network \\"
        in build_script
        and "--cache-dir /etc/apk/cache/openwrt-nes-emulator --cache-packages \\"
        in build_script
        and "--allow-untrusted --wait 120 add \\" in build_script
        and "./nes-emulator-$APK_VERSION.apk ./luci-app-nes-emulator-$APK_VERSION.apk"
        in build_script
        and re.search(
            r"apk --repositories-file /dev/null --no-network\s+\\\n"
            r"\s*--cache-dir /etc/apk/cache/openwrt-nes-emulator "
            r"--cache-packages\s+\\\n"
            r"\s*--allow-untrusted --wait 120 add\s+\\\n"
            rf"\s*(?:\./|/tmp/)nes-emulator-{re.escape(VERSION)}-r"
            rf"{re.escape(PACKAGE_RELEASE)}\.apk\s+\\\n"
            rf"\s*(?:\./|/tmp/)luci-app-nes-emulator-{re.escape(VERSION)}-r"
            rf"{re.escape(PACKAGE_RELEASE)}\.apk",
            read("README.md") + "\n" + read("docs/TECHNICAL.md"),
        ),
        "exact-version native and LuCI upgrades are not documented as one transaction",
    )
    require(
        '"ldflags=-static -s -no-pie '
        '-Wl,--gc-sections,-z,stack-size=2097152"' in build_script
        and "-Wl,--gc-sections,-z,stack-size=2097152" in build_script
        and 'awk \'$1 == "GNU_STACK" { print $6; exit }\'' in build_script
        and "stack_size <= 0x200000" in build_script,
        "standalone builds do not constrain and verify PT_GNU_STACK",
    )


def parse_shell_row_array(script: str, name: str) -> list[list[str]]:
    match = re.search(
        rf"^{re.escape(name)}=\(\n(?P<body>.*?)^\)\n",
        script,
        re.MULTILINE | re.DOTALL,
    )
    require(match is not None, f"{name} is missing from build-apks.sh")
    rows: list[list[str]] = []
    for line in match.group("body").splitlines():
        stripped = line.strip()
        require(
            len(stripped) >= 2
            and stripped.startswith("'")
            and stripped.endswith("'"),
            f"{name} contains a non-canonical row: {line}",
        )
        rows.append(stripped[1:-1].split("|"))
    return rows


def check_architecture_matrix() -> None:
    build_script = read("scripts/build-apks.sh")
    rows = parse_shell_row_array(build_script, "ARCH_PROFILE_ROWS")
    require(len(rows) == 35, "architecture matrix does not have exactly 35 rows")
    require(
        all(len(row) == 8 and all(row) for row in rows),
        "architecture matrix has an incomplete row",
    )
    arches = tuple(row[0] for row in rows)
    require(
        arches == OFFICIAL_OPENWRT_25_12_ARCHES,
        "architecture matrix differs from the official OpenWrt 25.12 set",
    )
    require(len(set(arches)) == len(arches), "architecture matrix has duplicates")
    require(
        "riscv64_riscv64" not in build_script,
        "stale pre-25.12 RISC-V package architecture survived",
    )
    require(
        {row[0] for row in rows if row[4] == "big"} == BIG_ENDIAN_ARCHES,
        "big-endian architecture classification is incomplete",
    )
    require(
        all(row[4] in {"little", "big"} for row in rows),
        "architecture matrix has an invalid byte order",
    )
    require(
        all(row[1] in {"zig", "openwrt-gcc"} for row in rows),
        "architecture matrix has an unknown compiler backend",
    )

    toolchain_rows = parse_shell_row_array(
        build_script, "TOOLCHAIN_PROFILE_ROWS"
    )
    require(
        all(
            len(row) == 4
            and re.fullmatch(r"[0-9a-f]{64}", row[2]) is not None
            for row in toolchain_rows
        ),
        "OpenWrt toolchain metadata is incomplete or unpinned",
    )
    gcc_arches = {row[0] for row in rows if row[1] == "openwrt-gcc"}
    gcc_rows = [row for row in rows if row[1] == "openwrt-gcc"]
    gcc_abis = {row[0]: row[7] for row in gcc_rows}
    require(
        all(row[2] != "-" and row[3] != "-" and row[7] != "none" for row in gcc_rows),
        "GCC architecture provenance omits its target, flags, or ABI",
    )
    require(
        gcc_abis.get("powerpc_464fp") == "powerpc-hard"
        and gcc_abis.get("powerpc_8548") == "powerpc-soft",
        "PowerPC GCC profiles do not record their float ABI",
    )
    require(
        {row[0] for row in toolchain_rows} == gcc_arches,
        "GCC profiles and pinned OpenWrt toolchains differ",
    )
    require(
        len(toolchain_rows) == len(gcc_arches),
        "OpenWrt toolchain metadata has duplicate architectures",
    )

    required_build_guards = (
        'fingerprint="$(source_fingerprint "$arch")"',
        'cache_dir="$CACHE_ROOT/binaries/$arch/$fingerprint"',
        'BIN_CACHE[$arch]="$cache_bin"',
        '"openwrt-arch=$arch"',
        '"target-flags=${PROFILE_FLAGS[$arch]}"',
        '"endian=${PROFILE_ENDIAN[$arch]}"',
        '"elf-abi=${PROFILE_ABI[$arch]}"',
        '[[ "${PROFILE_ENDIAN[$arch]}" == "big" ]]',
        'verify_nesd_elf "$cache_bin" "$arch"',
        'verify_nesd_elf "$build_dir/nesd" "$arch"',
        'readelf -hW "$binary"',
        'readelf -lW "$binary"',
        'readelf -dW "$binary"',
    )
    for guard in required_build_guards:
        require(guard in build_script, f"architecture build guard is missing: {guard}")
    require(
        "ZIG_TARGET" not in build_script,
        "legacy target-only cache matrix is still present",
    )


def check_source_bundle_filter() -> None:
    build_script = read("scripts/build-apks.sh")
    gitignore = read(".gitignore")
    require(
        not re.search(r"^\*\.d$", gitignore, re.MULTILINE),
        "global *.d ignore hides LuCI menu.d and acl.d directories",
    )
    require(
        'cp -a "$HOST_SRC_DIR/."' not in build_script,
        "standalone build snapshot still copies local build products",
    )
    require(
        'assert_no_build_products "$HOST_BUILD_SRC"' in build_script,
        "standalone source fingerprint is not guarded against build products",
    )
    require(
        'assert_no_build_products "$SOURCE_STAGE"' in build_script,
        "corresponding-source bundle is not guarded against build products",
    )
    require(
        "\\( -type f \\( -name nesd -o -name '*.o' -o -name '*.d'" in build_script
        and "-type d \\( -name .git -o -name .build" in build_script,
        "source filtering confuses legitimate *.d directories with dependency files",
    )
    require(
        "! -type d ! -type f -print -quit" in build_script
        and 'assert_regular_tree "$PROJECT_SNAPSHOT"' in build_script
        and 'assert_regular_tree "$SOURCE_STAGE"' in build_script,
        "source snapshots permit symlinks, FIFOs, sockets, or device nodes",
    )
    require(
        'copy_clean_source_tree "$PROJECT_SNAPSHOT" "$SOURCE_STAGE"' in build_script
        and 'HOST_SRC_DIR="$PROJECT_SNAPSHOT/' in build_script
        and 'LUCI_DIR="$PROJECT_SNAPSHOT/' in build_script,
        "build payload and corresponding source do not share one immutable snapshot",
    )
    for relative in (".github", "docs", "scripts", "tests", "upstream"):
        require(
            f'copy_clean_source_tree "$ROOT_DIR/{relative}" '
            f'"$PROJECT_SNAPSHOT/{relative}"' in build_script,
            f"corresponding-source bundle omits the {relative} tree",
        )
    for relative in (
        ".gitattributes",
        ".gitignore",
        "CHANGELOG.md",
        "CODE_OF_CONDUCT.md",
        "CONTRIBUTING.md",
        "install.sh",
        "LICENSE",
        "README.md",
        "SECURITY.md",
        "THIRD_PARTY_NOTICES.md",
        "UPSTREAMING.md",
        "feeds.conf.example",
    ):
        require(
            f'"$ROOT_DIR/{relative}"' in build_script,
            f"corresponding-source bundle omits {relative}",
        )
    require(
        'FCEUMM_PRISTINE_TREE="$RUN_DIR/libretro-fceumm-pristine"'
        in build_script
        and 'cp -a "$FCEUMM_TREE" "$FCEUMM_PRISTINE_TREE"' in build_script
        and 'cp -a "$FCEUMM_PRISTINE_TREE"' in build_script
        and 'tree_sha256 "$SOURCE_STAGE/third_party/libretro-fceumm"'
        in build_script,
        "corresponding source bundles a patched tree that cannot be rebuilt",
    )
    require(
        "normalize_project_source_modes()" in build_script
        and 'normalize_project_source_modes "$PROJECT_SNAPSHOT"' in build_script
        and "find \"$directory\" -type f -exec chmod 0644 {} +" in build_script,
        "corresponding-source bundle does not normalize DrvFS executable bits",
    )
    require(
        'SIGNING_TRUST_DIR="$RUN_DIR/signing/trusted"' in build_script
        and "do not form a key pair" in build_script
        and '--keys-dir "$SIGNING_TRUST_DIR"' in build_script,
        "APK signing does not validate and isolate the supplied public key",
    )
    require(
        'OUT_STAGE="$(mktemp -d ' in build_script
        and "publish_output" in build_script
        and "preserve_legacy_flat_packages" not in build_script
        and "OUTPUT_MARKER_VALUE=" in build_script
        and "validate_output_target" in build_script
        and "rollback_output_publish" in build_script
        and "OUT must not be a broad system directory" in build_script,
        "generated feed output is not staged before replacing stale artifacts",
    )
    for pattern in (
        "-name .build",
        "-name nesd",
        "-name '*.o'",
        "-name '*.d'",
        "-name __pycache__",
        "-name '*.pyc'",
    ):
        require(pattern in build_script, f"source filter is missing {pattern}")


def check_upstream_export_contract() -> None:
    guide = read("UPSTREAMING.md")
    exporter = read("scripts/export-openwrt-upstream.py")
    contract = read("tests/upstream_export_contract.py")
    check_script = read("scripts/check.sh")
    workflow = read(".github/workflows/ci.yml")
    native = read("upstream/openwrt-packages/multimedia/nes-emulator/Makefile")
    luci = read("package/luci-app-nes-emulator/Makefile.upstream")
    standalone_luci = read("package/luci-app-nes-emulator/Makefile")
    luci_pot = read("package/luci-app-nes-emulator/po/templates/nes-emulator.pot")
    installer = read("install.sh")
    build_script = read("scripts/build-apks.sh")
    fceumm_patches = (
        read("package/nes-emulator/patches/001-propagate-savestate-parse-errors.patch"),
        read("package/nes-emulator/patches/002-load-supplied-rom-buffer.patch"),
    )

    for reference in (
        "https://github.com/openwrt/packages/blob/master/CONTRIBUTING.md",
        "https://github.com/openwrt/luci/blob/master/CONTRIBUTING.md",
        "https://github.com/openwrt/luci/blob/master/luci.mk",
        "https://github.com/openwrt/openwrt/blob/main/include/package.mk",
        "https://github.com/openwrt/openwrt/blob/main/scripts/package-metadata.pl",
        "https://github.com/openwrt/openwrt/blob/main/feeds.conf.default",
    ):
        require(reference in guide, f"upstream guide omits primary source {reference}")
    require(
        "python3 scripts/export-openwrt-upstream.py" in guide
        and "openwrt-packages/multimedia/nes-emulator" in guide
        and "openwrt-luci/applications/luci-app-nes-emulator" in guide
        and "--check-templates" in guide
        and "--validate-only" in guide
        and "git commit -s" in guide
        and "git update-index --chmod=+x" in guide
        and "./build/i18n-sync.sh applications/luci-app-nes-emulator" in guide
        and "must begin with `100755`" in guide
        and "packages PR first" in guide,
        "upstream guide lacks the export, validation, or human submission workflow",
    )

    upstream_dependencies = {
        "+luci-base",
        "+rpcd",
        "+jshn",
        "+jsonfilter",
        "+cgi-io",
        "+nes-emulator",
    }
    luci_depends = re.search(r"^LUCI_DEPENDS:=(.*)$", luci, re.MULTILINE)
    require(luci_depends is not None, "upstream LuCI template lacks LUCI_DEPENDS")
    require(
        set(luci_depends.group(1).split()) >= upstream_dependencies
        and "include ../../luci.mk" in luci
        and "# call BuildPackage - OpenWrt buildroot signature" in luci
        and "PKG_VERSION:=" not in luci
        and "PKG_RELEASE:=" not in luci
        and "PKG_LICENSE_FILES:=" not in luci
        and "EXTRA_DEPENDS" not in luci
        and "nes-emulator (=" not in luci,
        "upstream LuCI recipe does not use canonical Git-derived metadata",
    )
    require(
        luci_pot.startswith(
            'msgid ""\nmsgstr "Content-Type: text/plain; charset=UTF-8"\n'
        )
        and luci_pot.count("\nmsgid ") >= 20
        and "applications/luci-app-nes-emulator/" in luci_pot
        and "package/luci-app-nes-emulator/" not in luci_pot,
        "LuCI translation template is missing or was not generated in upstream layout",
    )
    require(
        "PKG_VERSION:=1.0.0" in native
        and "PKG_RELEASE:=1" in native
        and "PKG_ASLR_PIE_REGULAR:=1" in native
        and "PKG_SOURCE:=openwrt-nes-emulator-$(PKG_VERSION).tar.gz" in native
        and "codeload.github.com/communism420/openwrt-nes-emulator/"
        "tar.gz/v$(PKG_VERSION)?" in native
        and "PKG_HASH:=a47435bd02c5610144a105d39c8e2d0f4a9940ee9eff02452353accec038feba"
        in native
        and "/latest/" not in native
        and "luci-app-nes-emulator" not in native,
        "native upstream template is mutable, unverified, or creates a dependency cycle",
    )
    require(
        "PKG_FCEUMM_VERSION:=76f68314ce4213703174108f461c431001dcc204"
        in native
        and "define Download/fceumm" in native
        and "libretro-fceumm-$(PKG_FCEUMM_VERSION).tar.gz" in native
        and "codeload.github.com/libretro/libretro-fceumm/tar.gz/"
        "$(PKG_FCEUMM_VERSION)?" in native
        and "HASH:=b067ebd0a973751e9b5af56f5b54d74d0a6e67349549b392a4615d3f0d44f031"
        in native
        and "$(eval $(call Download,fceumm))" in native
        and "FCEUMM_BUILD_DIR:=$(PKG_BUILD_DIR)/third_party/libretro-fceumm"
        in native
        and "define Build/Prepare" in native
        and "$(call Build/Prepare/Default)" in native
        and "$(call PatchDir,$(FCEUMM_BUILD_DIR)," in native
        and "$(CURDIR)/patches-fceumm,)" in native
        and "$(if $(QUILT),touch $(FCEUMM_BUILD_DIR)/.quilt_used)" in native
        and "define Quilt/Refresh/Package" in native
        and "$(call Quilt/RefreshDir,$(FCEUMM_BUILD_DIR)," in native
        and "Build/Quilt=$(call Quilt/Template,$(FCEUMM_BUILD_DIR),,,Package)"
        in native
        and 'FCEUMM_GIT_VERSION="$(PKG_FCEUMM_VERSION)"' in native,
        "native upstream template does not independently pin and patch FCEUmm",
    )
    require(
        "define Package/nes-emulator/postinst" in native
        and '[ -n "$${IPKG_INSTROOT:-}" ] && exit 0' in native
        and "$${PKG_INSTROOT" not in native
        and "NESD_ON_DEMAND=1 NESD_SKIP_AUTOLOAD=1" in native
        and "/etc/init.d/nes-emulator preflight" in native
        and "\nexit 0\nendef" in native
        and "$(INSTALL_BIN) $(CURDIR)/files/nes-emulator.init" in native
        and "$(INSTALL_CONF) $(CURDIR)/files/nes-emulator.config" in native,
        "native upstream runtime payload or best-effort post-install is incomplete",
    )
    for forbidden_variable in (
        "PROJECT_SOURCE_RELEASE",
        "PROJECT_SOURCE_DATE_EPOCH",
        "PROJECT_URL",
        "FCEUMM_SHORT_COMMIT",
        "NESD_SOURCE_DIR",
        "NESD_FILES_DIR",
        "SOURCE_DATE_EPOCH",
        "> $(PKG_BUILD_DIR)/version.date",
        "-r19-source.tar.gz",
        "/releases/download/",
        'FCEUMM_GIT_VERSION="76f68314ce42"',
        "$(INSTALL_DIR) $(1)/etc/nes-emulator",
        "chmod 0750",
    ):
        require(
            forbidden_variable not in native,
            f"native upstream recipe retains redundant variable {forbidden_variable}",
        )
    for fceumm_patch in fceumm_patches:
        require(
            re.match(
                r"^From [0-9a-f]{40} Mon Sep 17 00:00:00 2001\n"
                r"From: Yaroslav Vereshchagin "
                r"<yarik\.vereshchagin1996@gmail\.com>\n",
                fceumm_patch,
            )
            is not None
            and re.search(
                r"^Date: .+\nSubject: \[PATCH [12]/2\] ",
                fceumm_patch,
                re.MULTILINE,
            )
            is not None
            and "Upstream-Status:" in fceumm_patch
            and "Signed-off-by: Yaroslav Vereshchagin "
            "<yarik.vereshchagin1996@gmail.com>\n---\n" in fceumm_patch
            and "--- a/" in fceumm_patch
            and "+++ b/" in fceumm_patch,
            "FCEUmm patch lacks reviewable authorship or upstream status",
        )
    require(
        "Upstream-Status: Submitted "
        "[https://github.com/libretro/libretro-fceumm/pull/653]"
        in fceumm_patches[0]
        and "Upstream-Status: Inappropriate [nesd frontend specific]"
        in fceumm_patches[1]
        and "need_fullpath=true" in fceumm_patches[1]
        and "hash/load invariant" in fceumm_patches[1],
        "FCEUmm patches do not document their exact upstream disposition",
    )
    require(
        'NATIVE_FCEUMM_BACKPORT_PATCH = '
        '"001-propagate-savestate-parse-errors.patch"' in exporter
        and '"Upstream-Status: Submitted "' in exporter
        and '"Upstream-Status: Backport "' in exporter
        and "3db086eabeb6608706df330e7991b1bce8d25fba" in exporter
        and "patch.count(NATIVE_FCEUMM_SOURCE_STATUS) == 1" in exporter
        and "including the exported Backport metadata" in guide
        and "newer core revisions raise `FCEU_VERSION_NUMERIC`" in guide
        and "quilt refresh preserves that status header" in guide
        and "3db086eabeb6608706df330e7991b1bce8d25fba" in contract,
        "official export does not materialize the merged FCEUmm backport status",
    )

    require(
        "EXTRA_DEPENDS:=nes-emulator (=$(PKG_VERSION)-r$(PKG_RELEASE))"
        in standalone_luci
        and "+jshn" in standalone_luci
        and "+jsonfilter" in standalone_luci
        and "+cgi-io" in standalone_luci
        and '"luci-base rpcd jshn jsonfilter cgi-io nes-emulator=$APK_VERSION"'
        in build_script
        and "apk --update-cache --wait 120 add luci-base rpcd jshn jsonfilter cgi-io"
        in installer,
        "standalone exact-version or direct-runtime dependency semantics changed",
    )

    for marker in (
        "--check-templates",
        "--validate-only",
        "--maintainer",
        "materialize_identity",
        "MAINTAINER_PLACEHOLDER not in materialized",
        "export destination already exists",
        "SHA256SUMS",
        "FILE_MODES",
        "shutil.rmtree(staging, ignore_errors=True)",
    ):
        require(marker in exporter, f"upstream exporter lacks safety contract: {marker}")
    require(
        'require(\n            args.maintainer is not None,' in exporter
        and '"--maintainer is required for an upstream submission export"'
        in exporter
        and "maintainer metadata is omitted" not in exporter
        and "users.noreply.github.com" in exporter,
        "upstream exporter does not require a validated real maintainer identity",
    )
    require(
        "PKG_MAINTAINER:=@OPENWRT_MAINTAINER@" in luci
        and "LUCI_MAINTAINER:=$(PKG_MAINTAINER)" in luci,
        "LuCI template does not satisfy formal and packaged maintainer metadata",
    )
    require(
        "guarded_source_snapshot" in contract
        and "deterministic exported trees" in contract
        and "do-not-overwrite" in contract
        and "users.noreply.github.com" in contract
        and "dependency tampering" in contract
        and "tests/upstream_export_contract.py" in check_script
        and "scripts/export-openwrt-upstream.py --check-templates" in check_script
        and "sh scripts/check.sh" in workflow,
        "CI does not enforce the upstream exporter's black-box contracts",
    )


def check_publish_helpers() -> None:
    build_script = ROOT / "scripts/build-apks.sh"
    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        require(wsl is not None, "WSL is required for publish tests on Windows")
        translated = subprocess.run(
            [wsl, "--exec", "wslpath", "-a", str(build_script)],
            check=False,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        require(
            translated.returncode == 0 and translated.stdout.strip(),
            f"could not translate the build-script path for WSL:\n{translated.stdout}",
        )
        command = [
            wsl,
            "--exec",
            "bash",
            translated.stdout.strip(),
            "--self-test-publish",
        ]
    else:
        command = ["bash", str(build_script), "--self-test-publish"]
    result = subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    require(
        result.returncode == 0
        and "publish helper self-test: OK" in result.stdout,
        f"APK publish fault-injection self-test failed:\n{result.stdout}",
    )


def check_savestates() -> None:
    host_header = read("package/nes-emulator/src/host.h")
    host_source = read("package/nes-emulator/src/host.c")
    http_source = read("package/nes-emulator/src/http.c")
    client = read("package/nes-emulator/src/play.html")
    build_makefile = read("package/nes-emulator/src/Makefile")
    build_script = read("scripts/build-apks.sh")
    ci_workflow = read(".github/workflows/ci.yml")
    integration = read("tests/integration.py")
    patch = read(
        "package/nes-emulator/patches/001-propagate-savestate-parse-errors.patch"
    )
    patch_digest = hashlib.sha256(
        (
            ROOT
            / "package/nes-emulator/patches/001-propagate-savestate-parse-errors.patch"
        ).read_bytes()
    ).hexdigest()
    rom_buffer_patch = read(
        "package/nes-emulator/patches/002-load-supplied-rom-buffer.patch"
    )
    rom_buffer_patch_digest = hashlib.sha256(
        (
            ROOT
            / "package/nes-emulator/patches/002-load-supplied-rom-buffer.patch"
        ).read_bytes()
    ).hexdigest()

    require(
        "NES_CORE_CONTENT_PATH_BYTES 2048u" in host_header
        and "strlen(rom_path) >= NES_CORE_CONTENT_PATH_BYTES" in host_source
        and "ROM path is too long for the emulation core" in http_source
        and "too-long (Europe).nes" in integration
        and "NES_STATE_SLOT_COUNT 10u" in host_header
        and "NES_MAX_STATE_BYTES (4u * 1024u * 1024u)" in host_header
        and "host_list_states" in host_header
        and "host_save_state" in host_header
        and "host_load_state" in host_header
        and "host_delete_state" in host_header,
        "public full-save-state slot API is incomplete",
    )
    require(
        all(
            command in host_source
            for command in (
                "HOST_CMD_LIST_STATES",
                "HOST_CMD_SAVE_STATE",
                "HOST_CMD_LOAD_STATE",
                "HOST_CMD_DELETE_STATE",
            )
        )
        and "retro_serialize_size" in host_source
        and "retro_serialize(" in host_source
        and "retro_unserialize(" in host_source
        and "state_restore_backup" in host_source,
        "save states bypass the emulation-thread command queue or lack rollback",
    )
    require(
        'game.path = rom_path;' in host_source
        and 'game.data = data;' in host_source
        and 'game.size = size;' in host_source
        and '"/proc/self/fd/' not in host_source
        and "h->priv->rom_region = rom_region;" in host_source
        and "state_load_le32(header + 240) != h->priv->rom_region"
        in host_source,
        "ROM snapshots or PAL/NTSC-specific save-state identity are incomplete",
    )
    require(
        "NES_STATE_TOTAL_QUOTA (16ull * 1024ull * 1024ull)" in host_source
        and "NES_STATE_FREE_RESERVE (8ull * 1024ull * 1024ull)" in host_source
        and "O_NOFOLLOW" in host_source
        and "AT_SYMLINK_NOFOLLOW" in host_source
        and "st.st_nlink != 1" in host_source
        and "renameat(dirfd, temporary, dirfd, name)" in host_source
        and "fsync(dirfd)" in host_source
        and "nes_sha256_digest" in host_source
        and "state_fcs_valid" in host_source
        and 'memcmp(payload, "FCS", 3)' in host_source
        and "state_apply_frame(h, &candidate);" in host_source
        and "state_read_file(h, dirfd, slot, &blob, &info, false)" in host_source,
        "save-state storage lacks atomicity, bounds, identity, or FCS validation",
    )
    require(
        '"/api/states"' in http_source
        and '"/api/state/save"' in http_source
        and '"/api/state/load"' in http_source
        and '"/api/state/delete"' in http_source
        and '"state-saved"' in http_source
        and '"state-loaded"' in http_source
        and '"state-deleted"' in http_source
        and "websocket_discard_state_transition" in http_source
        and "recompute_inputs(srv, clients, client_count)" in http_source,
        "authenticated save-state HTTP/WS lifecycle is incomplete",
    )
    require(
        'id="state-slot"' in client
        and 'id="state-label"' in client
        and 'id="btn-state-save"' in client
        and 'id="btn-state-load"' in client
        and 'id="btn-state-delete"' in client
        and "function refreshStates()" in client
        and "stateRefreshGeneration" in client
        and "clearScheduledAudio()" in client
        and "releaseAllControls();" in client,
        "game client save-state controls or stale-media guards are incomplete",
    )
    require(
        "sha256.c" in build_makefile
        and "require_command patch" in build_script
        and "FCEUMM_PATCHED_TREE_SHA256" in build_script
        and f'FCEUMM_STATE_PATCH_SHA256="{patch_digest}"' in build_script
        and f'FCEUMM_ROM_BUFFER_PATCH_SHA256="{rom_buffer_patch_digest}"'
        in build_script
        and f"FCEUMM_STATE_PATCH_SHA256:={patch_digest}"
        in read("package/nes-emulator/Makefile")
        and f"FCEUMM_ROM_BUFFER_PATCH_SHA256:={rom_buffer_patch_digest}"
        in read("package/nes-emulator/Makefile")
        and "patch --batch --forward" in build_script,
        "standalone builds do not reproducibly include state hashing/core fixes",
    )
    ci_patch_hash = f'"{patch_digest}" "$STATE_PATCH"'
    ci_rom_buffer_hash = (
        f'"{rom_buffer_patch_digest}" "$ROM_BUFFER_PATCH"'
    )
    ci_patch_verify = "sha256sum --check --strict"
    ci_state_apply = 'patch --batch --forward -d "$FCEUMM_DIR" -p1 < "$STATE_PATCH"'
    ci_rom_buffer_apply = (
        'patch --batch --forward -d "$FCEUMM_DIR" -p1 < "$ROM_BUFFER_PATCH"'
    )
    ci_patch_hash_pos = ci_workflow.find(ci_patch_hash)
    ci_rom_buffer_hash_pos = ci_workflow.find(ci_rom_buffer_hash)
    ci_patch_verify_pos = ci_workflow.find(
        ci_patch_verify, ci_rom_buffer_hash_pos
    )
    ci_state_apply_pos = ci_workflow.find(ci_state_apply)
    ci_rom_buffer_apply_pos = ci_workflow.find(ci_rom_buffer_apply)
    require(
        'STATE_PATCH="${{ github.workspace }}/package/nes-emulator/patches/'
        '001-propagate-savestate-parse-errors.patch"' in ci_workflow
        and 'ROM_BUFFER_PATCH="${{ github.workspace }}/package/nes-emulator/'
        'patches/002-load-supplied-rom-buffer.patch"' in ci_workflow
        and ci_workflow.count(ci_patch_verify) >= 2
        and 0 <= ci_patch_hash_pos < ci_rom_buffer_hash_pos
        < ci_patch_verify_pos < ci_state_apply_pos < ci_rom_buffer_apply_pos,
        "native CI builds pristine FCEUmm instead of the verified patched core",
    )
    require(
        "int FCEUSS_Load_Mem" in patch
        and "return ret && totalsize == 0" in patch
        and "return FCEUSS_Load_Mem(data, size) != 0" in patch,
        "FCEUmm still hides malformed-state parser failures",
    )
    require(
        "if (info->data && info->size)" in rom_buffer_patch
        and "content_data = (const uint8_t *)info->data" in rom_buffer_patch,
        "FCEUmm does not load the exact ROM buffer hashed by the host",
    )
    require(
        "def test_savestate_roundtrip" in integration
        and "loading the full state did not restore battery RAM" in integration
        and "loading did not restore volatile CPU RAM" in integration
        and "paused save-state load did not restore its display frame" in integration
        and "49.0 < float(european_status" in integration
        and "Corrupt or incompatible" in client,
        "save-state roundtrip/corruption regressions are not covered",
    )
    require(
        "Battery-backed SRAM and full save states" in read(
            "package/luci-app-nes-emulator/htdocs/luci-static/resources/view/nes-emulator/settings.js"
        ),
        "save directory help does not describe full save states",
    )
    require(
        "## Save states" in read("README.md")
        and "ten full-machine save slots" in read("README.md")
        and "tests/sha256_contract.py" in read("scripts/check.sh"),
        "save-state documentation or SHA-256 regression coverage is missing",
    )


def main() -> int:
    check_versions()
    check_publication_surface()
    check_installer()
    check_upgrade_migration()
    check_init_stream_fps()
    check_json()
    check_generated_client()
    check_licenses()
    check_security_regressions()
    check_static_build()
    check_architecture_matrix()
    check_source_bundle_filter()
    check_upstream_export_contract()
    check_savestates()
    check_publish_helpers()
    print("repository checks: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
