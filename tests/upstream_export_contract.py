#!/usr/bin/env python3
"""Black-box contracts for the official OpenWrt two-tree exporter."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
EXPORTER = ROOT / "scripts/export-openwrt-upstream.py"
NATIVE_RELATIVE = Path("openwrt-packages/multimedia/nes-emulator")
LUCI_RELATIVE = Path("openwrt-luci/applications/luci-app-nes-emulator")
TEST_MAINTAINER = "Yaroslav Vereshchagin <yarik.vereshchagin1996@gmail.com>"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_exporter(*arguments: str, success: bool = True) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["PYTHONUTF8"] = "1"
    result = subprocess.run(
        [sys.executable, str(EXPORTER), *arguments],
        cwd=ROOT,
        env=environment,
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if success:
        require(
            result.returncode == 0,
            f"exporter unexpectedly failed:\n{result.stdout}{result.stderr}",
        )
    else:
        require(
            result.returncode != 0,
            f"exporter unexpectedly succeeded:\n{result.stdout}",
        )
    return result


def files_under(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return sorted(
        (path for path in root.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(ROOT).as_posix(),
    )


def guarded_source_snapshot() -> dict[str, str]:
    guarded = (
        ROOT / "package/nes-emulator",
        ROOT / "package/luci-app-nes-emulator",
        ROOT / "upstream/openwrt-packages/multimedia/nes-emulator",
    )
    files = [path for directory in guarded for path in files_under(directory)]
    files += [ROOT / "install.sh", ROOT / "scripts/build-apks.sh"]
    return {
        path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(set(files))
    }


def exported_snapshot(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(path for path in root.rglob("*") if path.is_file())
    }


def check_clean_export(output: Path) -> None:
    native = output / NATIVE_RELATIVE
    luci = output / LUCI_RELATIVE
    require(native.is_dir(), "export is missing the openwrt/packages tree")
    require(luci.is_dir(), "export is missing the openwrt/luci tree")

    marker = json.loads(
        (output / ".openwrt-nes-upstream-export.json").read_text(encoding="utf-8")
    )
    require(
        marker.get("maintainer") == TEST_MAINTAINER,
        "validated maintainer identity is missing from the export marker",
    )
    source_revision = marker.get("source_revision")
    require(
        source_revision == "unknown"
        or (
            isinstance(source_revision, str)
            and re.fullmatch(r"[0-9a-f]{40}(?:-dirty)?", source_revision)
            is not None
        ),
        "export marker contains an invalid source revision",
    )
    git_status = subprocess.run(
        ["git", "status", "--porcelain=v1", "--untracked-files=normal"],
        cwd=ROOT,
        check=False,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if git_status.returncode == 0 and source_revision != "unknown":
        require(
            source_revision.endswith("-dirty") == bool(git_status.stdout),
            "export marker does not report the current worktree state",
        )
    require(
        marker.get("trees")
        == [NATIVE_RELATIVE.as_posix(), LUCI_RELATIVE.as_posix()],
        "export marker does not identify exactly the two PR trees",
    )

    native_makefile = (native / "Makefile").read_text(encoding="utf-8")
    luci_makefile = (luci / "Makefile").read_text(encoding="utf-8")
    combined = native_makefile + "\n" + luci_makefile
    require("@OPENWRT_MAINTAINER@" not in combined, "placeholder leaked into export")
    require(
        f"PKG_MAINTAINER:={TEST_MAINTAINER}" in native_makefile,
        "native export lacks the validated maintainer",
    )
    require(
        f"PKG_MAINTAINER:={TEST_MAINTAINER}" in luci_makefile
        and "LUCI_MAINTAINER:=$(PKG_MAINTAINER)" in luci_makefile,
        "LuCI export lacks formal or runtime maintainer metadata",
    )
    require(
        not any(path.endswith("/Makefile.upstream") for path in exported_snapshot(output)),
        "template name leaked",
    )

    dependency_line = next(
        (line for line in luci_makefile.splitlines() if line.startswith("LUCI_DEPENDS:=")),
        "",
    )
    dependencies = set(dependency_line.partition(":=")[2].split())
    require(
        dependencies
        >= {
            "+luci-base",
            "+rpcd",
            "+jshn",
            "+jsonfilter",
            "+cgi-io",
            "+nes-emulator",
        },
        "LuCI export lacks a direct runtime/build-selection dependency",
    )
    require("include ../../luci.mk" in luci_makefile, "LuCI export does not use luci.mk")
    require(
        "PKG_VERSION:=" not in luci_makefile
        and "PKG_RELEASE:=" not in luci_makefile
        and "PKG_LICENSE_FILES:=" not in luci_makefile
        and not (luci / "files/LICENSE-MIT").exists(),
        "LuCI export retains ignored version/release or dead license metadata",
    )
    require(
        "# call BuildPackage - OpenWrt buildroot signature" in luci_makefile,
        "LuCI export lacks the package metadata scanner signature",
    )
    require("EXTRA_DEPENDS" not in luci_makefile, "standalone version lock leaked upstream")
    require("luci-app-nes-emulator" not in native_makefile, "native/LuCI cycle was introduced")
    native_init = native / "files/nes-emulator.init"
    native_config = native / "files/nes-emulator.config"
    require(
        native_init.read_bytes()
        == (ROOT / "package/nes-emulator/files/nes-emulator.init").read_bytes()
        and native_config.read_bytes()
        == (ROOT / "package/nes-emulator/files/nes-emulator.config").read_bytes(),
        "native export does not expose the exact reviewed init/UCI payload",
    )
    translation_template = (
        luci / "po/templates/nes-emulator.pot"
    ).read_text(encoding="utf-8")
    require(
        translation_template.startswith(
            'msgid ""\nmsgstr "Content-Type: text/plain; charset=UTF-8"\n'
        )
        and translation_template.count("\nmsgid ") >= 20
        and "applications/luci-app-nes-emulator/" in translation_template
        and "package/luci-app-nes-emulator/" not in translation_template,
        "LuCI export lacks its generated upstream translation template",
    )
    require(
        "define Package/nes-emulator/postinst" in native_makefile
        and '[ -n "$${IPKG_INSTROOT:-}" ] && exit 0' in native_makefile
        and "$${PKG_INSTROOT" not in native_makefile
        and "/etc/init.d/nes-emulator preflight" in native_makefile
        and "\nexit 0\nendef" in native_makefile,
        "native post-install is not image-root safe and best-effort",
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
    ):
        require(
            forbidden_variable not in native_makefile,
            f"native export retains redundant variable {forbidden_variable}",
        )
    require(
        "PKG_ASLR_PIE_REGULAR:=1" in native_makefile,
        "native upstream daemon does not opt into OpenWrt PIE hardening",
    )

    modes = (output / "FILE_MODES").read_text(encoding="ascii")
    require(
        "100644  openwrt-packages/multimedia/nes-emulator/Makefile"
        in modes,
        "native recipe mode is not preserved",
    )
    require(
        "100755  openwrt-packages/multimedia/nes-emulator/"
        "files/nes-emulator.init" in modes
        and "100644  openwrt-packages/multimedia/nes-emulator/"
        "files/nes-emulator.config" in modes,
        "native runtime file modes are not preserved",
    )
    require(
        "100755  openwrt-luci/applications/luci-app-nes-emulator/"
        "root/usr/libexec/rpcd/nes-emulator" in modes,
        "RPCD bridge mode is not preserved",
    )

    standalone_makefile = (
        ROOT / "package/luci-app-nes-emulator/Makefile"
    ).read_text(encoding="utf-8")
    require(
        "EXTRA_DEPENDS:=nes-emulator (=$(PKG_VERSION)-r$(PKG_RELEASE))"
        in standalone_makefile,
        "standalone LuCI/native exact-version lock was lost",
    )
    build_script = (ROOT / "scripts/build-apks.sh").read_text(encoding="utf-8")
    require(
        '"luci-base rpcd jshn jsonfilter cgi-io nes-emulator=$APK_VERSION"'
        in build_script,
        "standalone APK metadata lost direct dependencies or its exact pair lock",
    )


def main() -> int:
    before = guarded_source_snapshot()
    run_exporter("--check-templates")

    with tempfile.TemporaryDirectory(prefix="openwrt-nes-upstream-contract-") as temporary:
        temporary_root = Path(temporary)
        first = temporary_root / "first"
        second = temporary_root / "second"
        missing_identity = run_exporter(
            "--output", str(first), success=False
        )
        require(
            "--maintainer is required" in missing_identity.stderr
            and not first.exists(),
            "normal export did not fail closed without a maintainer identity",
        )
        run_exporter("--maintainer", TEST_MAINTAINER, "--output", str(first))
        run_exporter("--validate-only", str(first))
        check_clean_export(first)

        run_exporter("--maintainer", TEST_MAINTAINER, "--output", str(second))
        check_clean_export(second)
        require(
            exported_snapshot(first) == exported_snapshot(second),
            "identical inputs do not produce deterministic exported trees",
        )

        marker_path = first / ".openwrt-nes-upstream-export.json"
        marker_contents = marker_path.read_text(encoding="utf-8")
        marker_path.write_text("[]\n", encoding="utf-8", newline="\n")
        failure = run_exporter("--validate-only", str(first), success=False)
        require(
            "must be a JSON object" in failure.stderr
            and "Traceback" not in failure.stderr,
            "malformed export metadata is not rejected cleanly",
        )
        marker_path.write_text(marker_contents, encoding="utf-8", newline="\n")

        failure = run_exporter("--validate-only", "", success=False)
        require(
            "unsafe export destination" in failure.stderr,
            "empty --validate-only argument fell through to export mode",
        )

        existing = temporary_root / "existing"
        existing.mkdir()
        sentinel = existing / "do-not-overwrite"
        sentinel.write_text("preserve me\n", encoding="utf-8")
        failure = run_exporter(
            "--maintainer", TEST_MAINTAINER, "--output", str(existing), success=False
        )
        require("already exists" in failure.stderr, "existing-output failure is unclear")
        require(sentinel.read_text(encoding="utf-8") == "preserve me\n", "output was overwritten")

        invalid_identity_output = temporary_root / "invalid-identity"
        failure = run_exporter(
            "--maintainer",
            "Placeholder User <user@users.noreply.github.com>",
            "--output",
            str(invalid_identity_output),
            success=False,
        )
        require("non-noreply" in failure.stderr, "noreply identity rejection is unclear")
        require(not invalid_identity_output.exists(), "invalid identity created an export")

        luci_makefile = second / LUCI_RELATIVE / "Makefile"
        luci_makefile.write_text(
            luci_makefile.read_text(encoding="utf-8").replace(" +nes-emulator", ""),
            encoding="utf-8",
            newline="\n",
        )
        failure = run_exporter("--validate-only", str(second), success=False)
        require(
            "directly depend" in failure.stderr,
            "dependency tampering was not rejected at the semantic boundary",
        )

    require(
        guarded_source_snapshot() == before,
        "exporter modified a standalone or template source file",
    )
    print("upstream export contract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
