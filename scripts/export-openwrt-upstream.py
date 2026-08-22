#!/usr/bin/env python3
"""Export and validate the two trees submitted to the official OpenWrt feeds."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
MAINTAINER_PLACEHOLDER = "@OPENWRT_MAINTAINER@"
NATIVE_TEMPLATE = (
    ROOT / "upstream" / "openwrt-packages" / "multimedia" / "nes-emulator"
)
NATIVE_RUNTIME_FILES = ROOT / "package" / "nes-emulator" / "files"
NATIVE_FCEUMM_PATCHES = ROOT / "package" / "nes-emulator" / "patches"
NATIVE_FCEUMM_PATCH_NAMES = (
    "001-propagate-savestate-parse-errors.patch",
    "002-load-supplied-rom-buffer.patch",
)
LUCI_SOURCE = ROOT / "package" / "luci-app-nes-emulator"
LUCI_MAKEFILE_TEMPLATE = LUCI_SOURCE / "Makefile.upstream"
DEFAULT_OUTPUT = ROOT / "build" / "openwrt-upstream"
EXPORT_MARKER = ".openwrt-nes-upstream-export.json"
CHECKSUMS_FILE = "SHA256SUMS"
MODES_FILE = "FILE_MODES"

NATIVE_RELATIVE = Path("openwrt-packages/multimedia/nes-emulator")
LUCI_RELATIVE = Path("openwrt-luci/applications/luci-app-nes-emulator")
EXECUTABLE_PATHS = {
    NATIVE_RELATIVE / "files/nes-emulator.init",
    LUCI_RELATIVE / "root/usr/libexec/rpcd/nes-emulator",
}
ROM_SUFFIXES = {".nes", ".fds", ".unf", ".unif", ".srm", ".sav", ".nss"}
FORBIDDEN_FILE_SUFFIXES = {".o", ".pyc", ".pyo", ".swp"}
FORBIDDEN_NAMES = {".git", "__pycache__", ".build", "nesd"}


class ExportError(RuntimeError):
    """An actionable upstream export or validation error."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ExportError(message)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise ExportError(f"cannot read UTF-8 text file {path}: {exc}") from exc


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ExportError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def regular_files(root: Path) -> list[Path]:
    require(root.is_dir() and not root.is_symlink(), f"missing regular directory: {root}")
    files: list[Path] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(root)
        require(not path.is_symlink(), f"symbolic links are not allowed: {relative}")
        require(
            not any(part in FORBIDDEN_NAMES for part in relative.parts),
            f"build or VCS artifact is not allowed: {relative}",
        )
        if path.is_dir():
            continue
        require(path.is_file(), f"non-regular filesystem entry is not allowed: {relative}")
        require(
            path.suffix.lower() not in FORBIDDEN_FILE_SUFFIXES,
            f"build/editor artifact is not allowed: {relative}",
        )
        require(
            path.suffix.lower() not in ROM_SUFFIXES,
            f"ROM or emulator state is not allowed in an upstream tree: {relative}",
        )
        files.append(path)
    require(files, f"tree is empty: {root}")
    return files


def validate_maintainer(value: str) -> str:
    require(value == value.strip(), "maintainer identity must not have outer whitespace")
    require("\n" not in value and "\r" not in value, "maintainer identity must be one line")
    match = re.fullmatch(r"([^<>]{2,100}) <([^<>\s]{3,254})>", value)
    require(
        match is not None,
        "use --maintainer in 'Name <email>' form with your real identity",
    )
    name = match.group(1).strip()
    email = match.group(2)
    require(name == match.group(1), "maintainer name has duplicate whitespace near brackets")
    require(
        not any(character in value for character in "$#\\"),
        "maintainer identity contains a Makefile control character",
    )
    require(
        re.fullmatch(
            r"[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
            r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
            r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+",
            email,
        )
        is not None,
        "maintainer email must be a real, fully qualified address",
    )
    require(
        not email.startswith(".") and not email.partition("@")[0].endswith(".")
        and ".." not in email,
        "maintainer email has invalid dot placement",
    )
    lowered = value.casefold()
    forbidden_fragments = (
        "@openwrt_maintainer@",
        "your name",
        "real name",
        "maintainer name",
        "openwrt nes emulator contributors",
        "users.noreply.github.com",
        "@example.com",
        "@example.net",
        "@example.org",
        ".example>",
        ".test>",
        ".invalid>",
        "@localhost>",
    )
    require(
        not any(fragment in lowered for fragment in forbidden_fragments),
        "maintainer must be the submitter's real name and non-noreply email",
    )
    return f"{name} <{email}>"


def identity_assignments(makefile: str, variable: str) -> list[str]:
    return re.findall(
        rf"^{re.escape(variable)}[ \t]*[:+?]?=.*$",
        makefile,
        re.MULTILINE,
    )


def validate_template_identity(makefile: str, variable: str, label: str) -> None:
    """Allow no maintainer or one explicit exporter placeholder in a template."""
    expected = f"{variable}:={MAINTAINER_PLACEHOLDER}"
    assignments = identity_assignments(makefile, variable)
    require(
        not assignments or assignments == [expected],
        f"{label} template must omit {variable} or use exactly {expected}",
    )
    require(
        makefile.count(MAINTAINER_PLACEHOLDER) == (1 if assignments else 0),
        f"{label} template has a misplaced or duplicate maintainer placeholder",
    )


def validate_export_identity(
    makefile: str,
    variable: str,
    maintainer: str | None,
    label: str,
) -> None:
    require(
        MAINTAINER_PLACEHOLDER not in makefile,
        f"{label} still contains the maintainer placeholder",
    )
    assignments = identity_assignments(makefile, variable)
    if maintainer is None:
        require(
            not assignments,
            f"{label} must omit {variable} when --maintainer is not supplied",
        )
    else:
        require(
            assignments == [f"{variable}:={maintainer}"],
            f"{label} does not contain exactly the validated maintainer identity",
        )


def validate_native_tree(
    root: Path,
    maintainer: str | None,
    *,
    template: bool = False,
) -> None:
    files = regular_files(root)
    makefile_path = root / "Makefile"
    require(makefile_path in files, "openwrt/packages tree has no Makefile")
    makefile = read_text(makefile_path)

    for marker in (
        "include $(TOPDIR)/rules.mk",
        "PKG_NAME:=nes-emulator",
        "PKG_VERSION:=1.0.0",
        "PKG_RELEASE:=1",
        "PKG_LICENSE:=",
        "PKG_HASH:=",
        "PKG_FCEUMM_VERSION:=76f68314ce4213703174108f461c431001dcc204",
        "PKG_ASLR_PIE_REGULAR:=1",
        "include $(INCLUDE_DIR)/package.mk",
        "define Download/fceumm",
        "FILE:=libretro-fceumm-$(PKG_FCEUMM_VERSION).tar.gz",
        "URL:=https://codeload.github.com/libretro/libretro-fceumm/tar.gz/$(PKG_FCEUMM_VERSION)?",
        "HASH:=b067ebd0a973751e9b5af56f5b54d74d0a6e67349549b392a4615d3f0d44f031",
        "$(eval $(call Download,fceumm))",
        "FCEUMM_BUILD_DIR:=$(PKG_BUILD_DIR)/third_party/libretro-fceumm",
        "define Build/Prepare",
        "$(call Build/Prepare/Default)",
        "$(call PatchDir,$(FCEUMM_BUILD_DIR),",
        "$(CURDIR)/patches-fceumm,)",
        "$(if $(QUILT),touch $(FCEUMM_BUILD_DIR)/.quilt_used)",
        "define Quilt/Refresh/Package",
        "$(call Quilt/RefreshDir,$(FCEUMM_BUILD_DIR),",
        "Build/Quilt=$(call Quilt/Template,$(FCEUMM_BUILD_DIR),,,Package)",
        "define Build/Compile",
        'FCEUMM_GIT_VERSION="$(PKG_FCEUMM_VERSION)"',
        "define Package/nes-emulator/install",
        "define Package/nes-emulator/postinst",
        '[ -n "$${IPKG_INSTROOT:-}" ] && exit 0',
        "/etc/init.d/nes-emulator preflight",
        "exit 0",
        "$(INSTALL_BIN) $(CURDIR)/files/nes-emulator.init",
        "$(INSTALL_CONF) $(CURDIR)/files/nes-emulator.config",
        "$(eval $(call BuildPackage,nes-emulator))",
    ):
        require(marker in makefile, f"native upstream Makefile is missing: {marker}")
    require(
        re.search(r"^PKG_HASH:=[0-9a-f]{64}$", makefile, re.MULTILINE) is not None,
        "native project source hash is missing or not SHA-256",
    )
    require(
        "PKG_SOURCE:=openwrt-nes-emulator-$(PKG_VERSION).tar.gz" in makefile
        and "PKG_SOURCE_URL:=https://codeload.github.com/communism420/"
        "openwrt-nes-emulator/tar.gz/v$(PKG_VERSION)?" in makefile
        and "PKG_HASH:=a47435bd02c5610144a105d39c8e2d0f4a9940ee9eff02452353accec038feba"
        in makefile,
        "native project source is not the canonical hash-pinned v1.0.0 codeload",
    )
    require(
        "/latest/" not in makefile
        and "/refs/heads/" not in makefile
        and "archive/master" not in makefile
        and "archive/main" not in makefile,
        "native upstream source must be tag-scoped or commit-scoped and hash-pinned",
    )
    require(
        "luci-app-nes-emulator" not in makefile and "LUCI_" not in makefile,
        "native package must not depend on the LuCI feed",
    )
    for forbidden in (
        "PROJECT_SOURCE_RELEASE",
        "PROJECT_SOURCE_DATE_EPOCH",
        "PROJECT_URL",
        "FCEUMM_SHORT_COMMIT",
        "NESD_SOURCE_DIR",
        "NESD_FILES_DIR",
        "FCEUMM_DIR:=",
        "$${PKG_INSTROOT",
        "SOURCE_DATE_EPOCH",
        "> $(PKG_BUILD_DIR)/version.date",
        "-r19-source.tar.gz",
        "/releases/download/",
        'FCEUMM_GIT_VERSION="76f68314ce42"',
        "$(INSTALL_DIR) $(1)/etc/nes-emulator",
        "chmod 0750",
    ):
        require(
            forbidden not in makefile,
            f"native upstream recipe duplicates a standard variable or guard: {forbidden}",
        )
    init_script = root / "files/nes-emulator.init"
    default_config = root / "files/nes-emulator.config"
    require(init_script in files, "native upstream tree omits its reviewed init script")
    require(default_config in files, "native upstream tree omits its reviewed UCI defaults")
    require(
        read_text(init_script).startswith("#!/bin/sh /etc/rc.common\n")
        and 'EXTRA_COMMANDS="preflight"' in read_text(init_script),
        "native upstream init script lacks its rc.common preflight entry point",
    )
    require(
        read_text(default_config).startswith("config nes-emulator 'main'\n"),
        "native upstream UCI defaults are malformed",
    )
    patch_directory = root / "patches-fceumm"
    require(
        patch_directory.is_dir()
        and {path.name for path in patch_directory.iterdir()}
        == set(NATIVE_FCEUMM_PATCH_NAMES),
        "native upstream tree does not contain exactly the two reviewed FCEUmm patches",
    )
    patch_statuses = {
        "001-propagate-savestate-parse-errors.patch": (
            "Upstream-Status: Submitted "
            "[https://github.com/libretro/libretro-fceumm/pull/653]"
        ),
        "002-load-supplied-rom-buffer.patch": (
            "Upstream-Status: Inappropriate [nesd frontend specific]"
        ),
    }
    for patch_name in NATIVE_FCEUMM_PATCH_NAMES:
        patch_path = patch_directory / patch_name
        require(patch_path in files, f"native upstream tree omits {patch_name}")
        patch = read_text(patch_path)
        require(
            re.match(
                r"^From [0-9a-f]{40} Mon Sep 17 00:00:00 2001\n"
                r"From: Yaroslav Vereshchagin "
                r"<yarik\.vereshchagin1996@gmail\.com>\n",
                patch,
            )
            is not None
            and re.search(r"^Date: .+\nSubject: \[PATCH [12]/2\] ", patch, re.MULTILINE)
            is not None
            and patch_statuses[patch_name] in patch
            and "Signed-off-by: Yaroslav Vereshchagin "
            "<yarik.vereshchagin1996@gmail.com>\n---\n" in patch
            and "--- a/" in patch
            and "+++ b/" in patch,
            f"native FCEUmm patch lacks reviewable authorship metadata: {patch_name}",
        )
    if template:
        validate_template_identity(makefile, "PKG_MAINTAINER", "native")
    else:
        validate_export_identity(makefile, "PKG_MAINTAINER", maintainer, "native")


def dependency_tokens(makefile: str) -> set[str]:
    match = re.search(r"^LUCI_DEPENDS:=(.*)$", makefile, re.MULTILINE)
    require(match is not None, "LuCI upstream Makefile has no LUCI_DEPENDS")
    return set(match.group(1).split())


def validate_luci_tree(
    root: Path,
    maintainer: str | None,
    *,
    template: bool = False,
) -> None:
    files = regular_files(root)
    makefile_path = root / "Makefile"
    require(makefile_path in files, "openwrt/luci tree has no Makefile")
    require(not (root / "Makefile.upstream").exists(), "template filename leaked into LuCI export")
    makefile = read_text(makefile_path)

    for marker in (
        "include $(TOPDIR)/rules.mk",
        "PKG_LICENSE:=MIT",
        "include ../../luci.mk",
        "# call BuildPackage - OpenWrt buildroot signature",
    ):
        require(marker in makefile, f"LuCI upstream Makefile is missing: {marker}")
    require(
        dependency_tokens(makefile)
        >= {
            "+luci-base",
            "+rpcd",
            "+jshn",
            "+jsonfilter",
            "+cgi-io",
            "+nes-emulator",
        },
        (
            "LuCI package must directly depend on luci-base, rpcd, jshn, "
            "jsonfilter, cgi-io, and nes-emulator"
        ),
    )
    for forbidden in (
        "include $(INCLUDE_DIR)/package.mk",
        "$(eval $(call BuildPackage",
        "EXTRA_DEPENDS",
        "nes-emulator (=",
        "nes-emulator=",
        "PKG_VERSION:=",
        "PKG_RELEASE:=",
        "PKG_LICENSE_FILES:=",
    ):
        require(forbidden not in makefile, f"LuCI upstream recipe is not canonical: {forbidden}")
    if template:
        validate_template_identity(makefile, "PKG_MAINTAINER", "LuCI")
    else:
        validate_export_identity(makefile, "PKG_MAINTAINER", maintainer, "LuCI")
    require(
        "LUCI_MAINTAINER:=$(PKG_MAINTAINER)" in makefile,
        "LuCI package metadata does not inherit the formal PKG_MAINTAINER",
    )

    required = (
        "htdocs/luci-static/resources/view/nes-emulator/overview.js",
        "htdocs/luci-static/resources/view/nes-emulator/play.js",
        "htdocs/luci-static/resources/view/nes-emulator/settings.js",
        "po/templates/nes-emulator.pot",
        "root/usr/share/luci/menu.d/luci-app-nes-emulator.json",
        "root/usr/share/rpcd/acl.d/luci-app-nes-emulator.json",
        "root/usr/libexec/rpcd/nes-emulator",
    )
    for relative in required:
        require((root / relative).is_file(), f"LuCI upstream tree is missing {relative}")
    pot = read_text(root / "po/templates/nes-emulator.pot")
    require(
        pot.startswith('msgid ""\nmsgstr "Content-Type: text/plain; charset=UTF-8"\n')
        and pot.count("\nmsgid ") >= 20
        and "applications/luci-app-nes-emulator/" in pot
        and "package/luci-app-nes-emulator/" not in pot,
        "LuCI translation template is missing, empty, or has non-upstream paths",
    )
    rpcd = read_text(root / "root/usr/libexec/rpcd/nes-emulator")
    require(
        rpcd.startswith("#!/bin/sh\n")
        and "/usr/share/libubox/jshn.sh" in rpcd,
        "LuCI rpcd bridge does not declare its jshn runtime use",
    )
    require(
        re.search(r"(?:^|[;&|() \t])jsonfilter(?:[ \t]|$)", rpcd, re.MULTILINE)
        is not None,
        "LuCI rpcd bridge no longer demonstrates its jsonfilter runtime use",
    )
    for relative in (
        "root/usr/share/luci/menu.d/luci-app-nes-emulator.json",
        "root/usr/share/rpcd/acl.d/luci-app-nes-emulator.json",
    ):
        try:
            json.loads(read_text(root / relative))
        except json.JSONDecodeError as exc:
            raise ExportError(f"invalid exported JSON {relative}: {exc}") from exc


def validate_templates() -> None:
    native_makefile = NATIVE_TEMPLATE / "Makefile"
    require(native_makefile.is_file(), "missing native upstream Makefile template")
    validate_template_identity(read_text(native_makefile), "PKG_MAINTAINER", "native")
    require(LUCI_MAKEFILE_TEMPLATE.is_file(), "missing package/luci-app-nes-emulator/Makefile.upstream")
    validate_template_identity(
        read_text(LUCI_MAKEFILE_TEMPLATE),
        "PKG_MAINTAINER",
        "LuCI",
    )

    # Validate both materialized payloads without modifying the standalone
    # package recipes. Native runtime files are deliberately copied into the
    # packages-feed tree so reviewers see the exact procd and UCI definitions.
    with tempfile.TemporaryDirectory(prefix="openwrt-nes-luci-template-") as temporary:
        temporary_root = Path(temporary)
        native_destination = temporary_root / "nes-emulator"
        luci_destination = temporary_root / "luci-app-nes-emulator"
        copy_native_tree(native_destination, maintainer=None)
        validate_native_tree(native_destination, None)
        copy_luci_tree(luci_destination, maintainer=None)
        validate_luci_tree(luci_destination, None)


def copy_regular_file(source: Path, destination: Path) -> None:
    require(source.is_file() and not source.is_symlink(), f"source is not a regular file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    shutil.copystat(source, destination, follow_symlinks=False)


def copy_tree(source: Path, destination: Path, excluded: Iterable[Path] = ()) -> None:
    excluded_set = set(excluded)
    require(source.is_dir() and not source.is_symlink(), f"missing source tree: {source}")
    for path in regular_files(source):
        relative = path.relative_to(source)
        if relative in excluded_set:
            continue
        copy_regular_file(path, destination / relative)


def materialize_identity(
    template: str,
    variable: str,
    maintainer: str | None,
    insertion_anchor: str,
    label: str,
) -> str:
    """Remove a template token, or insert a user-supplied identity explicitly."""
    validate_template_identity(template, variable, label)
    placeholder_line = f"{variable}:={MAINTAINER_PLACEHOLDER}"
    replacement = f"{variable}:={maintainer}" if maintainer is not None else ""
    if placeholder_line in template:
        materialized = template.replace(placeholder_line, replacement, 1)
    elif maintainer is not None:
        require(
            insertion_anchor in template,
            f"{label} has no stable insertion point for {variable}",
        )
        materialized = template.replace(
            insertion_anchor,
            f"{variable}:={maintainer}\n\n{insertion_anchor}",
            1,
        )
    else:
        materialized = template
    require(
        MAINTAINER_PLACEHOLDER not in materialized,
        f"{label} still contains an unresolved maintainer placeholder",
    )
    validate_export_identity(materialized, variable, maintainer, label)
    return materialized


def copy_native_tree(destination: Path, maintainer: str | None) -> None:
    copy_tree(NATIVE_TEMPLATE, destination)
    for filename in ("nes-emulator.init", "nes-emulator.config"):
        copy_regular_file(NATIVE_RUNTIME_FILES / filename, destination / "files" / filename)
    for filename in NATIVE_FCEUMM_PATCH_NAMES:
        copy_regular_file(
            NATIVE_FCEUMM_PATCHES / filename,
            destination / "patches-fceumm" / filename,
        )
    makefile = destination / "Makefile"
    makefile.write_text(
        materialize_identity(
            read_text(makefile),
            "PKG_MAINTAINER",
            maintainer,
            "include $(INCLUDE_DIR)/package.mk",
            "native template",
        ),
        encoding="utf-8",
        newline="\n",
    )


def copy_luci_tree(destination: Path, maintainer: str | None) -> None:
    copy_tree(
        LUCI_SOURCE,
        destination,
        excluded={
            Path("Makefile"),
            Path("Makefile.upstream"),
            Path("files/LICENSE-MIT"),
        },
    )
    template = materialize_identity(
        read_text(LUCI_MAKEFILE_TEMPLATE),
        "PKG_MAINTAINER",
        maintainer,
        "include ../../luci.mk",
        "LuCI template",
    )
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "Makefile").write_text(template, encoding="utf-8", newline="\n")


def set_declared_modes(root: Path) -> None:
    for path in regular_files(root):
        relative = path.relative_to(root)
        mode = 0o755 if relative in EXECUTABLE_PATHS else 0o644
        try:
            path.chmod(mode)
        except OSError as exc:
            raise ExportError(f"cannot set mode {mode:o} on {path}: {exc}") from exc


def source_revision() -> str:
    try:
        revision_result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=False,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        status_result = subprocess.run(
            ["git", "status", "--porcelain=v1", "--untracked-files=normal"],
            cwd=ROOT,
            check=False,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return "unknown"
    revision = revision_result.stdout.strip()
    if (
        revision_result.returncode != 0
        or status_result.returncode != 0
        or re.fullmatch(r"[0-9a-f]{40}", revision) is None
    ):
        return "unknown"
    return revision + ("-dirty" if status_result.stdout else "")


def write_export_metadata(root: Path, maintainer: str | None) -> None:
    tree_files = [
        path
        for relative in (NATIVE_RELATIVE, LUCI_RELATIVE)
        for path in regular_files(root / relative)
    ]
    checksums: list[str] = []
    modes: list[str] = []
    for path in sorted(tree_files, key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root)
        checksums.append(f"{sha256_file(path)}  {relative.as_posix()}")
        mode = "100755" if relative in EXECUTABLE_PATHS else "100644"
        modes.append(f"{mode}  {relative.as_posix()}")
    (root / CHECKSUMS_FILE).write_text("\n".join(checksums) + "\n", encoding="ascii", newline="\n")
    (root / MODES_FILE).write_text("\n".join(modes) + "\n", encoding="ascii", newline="\n")
    marker = {
        "format": "openwrt-nes-upstream-export-v1",
        "maintainer": maintainer,
        "source_revision": source_revision(),
        "trees": [NATIVE_RELATIVE.as_posix(), LUCI_RELATIVE.as_posix()],
    }
    (root / EXPORT_MARKER).write_text(
        json.dumps(marker, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def parse_manifest_lines(path: Path, value_pattern: str) -> dict[Path, str]:
    entries: dict[Path, str] = {}
    for number, line in enumerate(read_text(path).splitlines(), 1):
        match = re.fullmatch(rf"({value_pattern})  ([^\\]+)", line)
        require(match is not None, f"invalid {path.name} line {number}")
        relative = Path(match.group(2))
        require(
            not relative.is_absolute() and ".." not in relative.parts,
            f"unsafe path in {path.name}: {relative}",
        )
        require(relative not in entries, f"duplicate path in {path.name}: {relative}")
        entries[relative] = match.group(1)
    require(entries, f"{path.name} is empty")
    return entries


def validate_export(root: Path) -> str | None:
    require(root.is_dir() and not root.is_symlink(), f"export directory does not exist: {root}")
    expected_top_level = {
        EXPORT_MARKER,
        CHECKSUMS_FILE,
        MODES_FILE,
        NATIVE_RELATIVE.parts[0],
        LUCI_RELATIVE.parts[0],
    }
    actual_top_level = {entry.name for entry in root.iterdir()}
    require(
        actual_top_level == expected_top_level,
        "export has missing or unexpected top-level entries",
    )
    expected_directory_shapes = {
        Path("openwrt-packages"): {"multimedia"},
        Path("openwrt-packages/multimedia"): {"nes-emulator"},
        Path("openwrt-luci"): {"applications"},
        Path("openwrt-luci/applications"): {"luci-app-nes-emulator"},
    }
    for relative, expected_children in expected_directory_shapes.items():
        directory = root / relative
        require(
            directory.is_dir() and not directory.is_symlink(),
            f"export directory is missing or unsafe: {relative}",
        )
        require(
            {entry.name for entry in directory.iterdir()} == expected_children,
            f"export contains an unexpected sibling below {relative}",
        )
    marker_path = root / EXPORT_MARKER
    require(marker_path.is_file() and not marker_path.is_symlink(), "export ownership marker is missing")
    for metadata_name in (CHECKSUMS_FILE, MODES_FILE):
        metadata_path = root / metadata_name
        require(
            metadata_path.is_file() and not metadata_path.is_symlink(),
            f"export metadata is missing or unsafe: {metadata_name}",
        )
    try:
        marker = json.loads(read_text(marker_path))
    except json.JSONDecodeError as exc:
        raise ExportError(f"invalid export ownership marker: {exc}") from exc
    require(isinstance(marker, dict), "export ownership marker must be a JSON object")
    require(
        marker.get("format") == "openwrt-nes-upstream-export-v1",
        "export ownership marker has an unsupported format",
    )
    require(
        set(marker) == {"format", "maintainer", "source_revision", "trees"},
        "export ownership marker has missing or unexpected fields",
    )
    require(
        marker.get("source_revision") == "unknown"
        or (
            isinstance(marker.get("source_revision"), str)
            and re.fullmatch(r"[0-9a-f]{40}(?:-dirty)?", marker["source_revision"])
            is not None
        ),
        "export marker source revision is invalid",
    )
    marker_maintainer = marker.get("maintainer")
    require(
        marker_maintainer is None or isinstance(marker_maintainer, str),
        "export marker maintainer must be null or a string",
    )
    maintainer = (
        validate_maintainer(marker_maintainer)
        if isinstance(marker_maintainer, str)
        else None
    )
    require(
        marker.get("trees") == [NATIVE_RELATIVE.as_posix(), LUCI_RELATIVE.as_posix()],
        "export marker lists unexpected trees",
    )
    validate_native_tree(root / NATIVE_RELATIVE, maintainer)
    validate_luci_tree(root / LUCI_RELATIVE, maintainer)

    actual = {
        path.relative_to(root): sha256_file(path)
        for relative in (NATIVE_RELATIVE, LUCI_RELATIVE)
        for path in regular_files(root / relative)
    }
    expected = parse_manifest_lines(root / CHECKSUMS_FILE, r"[0-9a-f]{64}")
    require(actual == expected, "exported files do not match SHA256SUMS")
    expected_modes = parse_manifest_lines(root / MODES_FILE, r"100(?:644|755)")
    declared_modes = {
        relative: ("100755" if relative in EXECUTABLE_PATHS else "100644")
        for relative in actual
    }
    require(expected_modes == declared_modes, "FILE_MODES does not match the canonical tree")
    if os.name != "nt":
        for relative, declared in expected_modes.items():
            actual_mode = stat.S_IMODE((root / relative).stat().st_mode)
            require(
                actual_mode == int(declared[-3:], 8),
                f"filesystem mode differs from FILE_MODES: {relative}",
            )
    return maintainer


def safe_output_path(value: str | None) -> Path:
    candidate = Path(value).expanduser() if value is not None else DEFAULT_OUTPUT
    require(not candidate.is_symlink(), f"refusing symbolic-link export destination: {candidate}")
    output = candidate.resolve()
    forbidden = {
        ROOT.resolve(),
        ROOT.parent.resolve(),
        Path.home().resolve(),
        Path(output.anchor).resolve(),
        (ROOT / "package").resolve(),
        (ROOT / "upstream").resolve(),
    }
    require(output not in forbidden, f"refusing unsafe export destination: {output}")
    for source in (NATIVE_TEMPLATE.resolve(), LUCI_SOURCE.resolve()):
        require(
            output != source and source not in output.parents and output not in source.parents,
            f"export destination overlaps a source tree: {output}",
        )
    return output


def export_trees(output: Path, maintainer: str | None) -> None:
    require(not output.exists(), f"export destination already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{output.name}.stage-", dir=output.parent)
    )
    try:
        copy_native_tree(staging / NATIVE_RELATIVE, maintainer)
        copy_luci_tree(staging / LUCI_RELATIVE, maintainer)
        set_declared_modes(staging)
        write_export_metadata(staging, maintainer)
        validate_export(staging)
        staging.rename(output)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export ready-to-copy openwrt/packages and openwrt/luci PR trees, "
            "then validate their dependency and packaging contracts."
        )
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check-templates",
        action="store_true",
        help="validate repository templates without creating an export",
    )
    mode.add_argument(
        "--validate-only",
        metavar="DIRECTORY",
        help="validate a previously generated export without changing it",
    )
    parser.add_argument(
        "--maintainer",
        help=(
            "required real upstream identity in 'Name <email>' form for a "
            "normal export; validation-only modes do not accept it"
        ),
    )
    parser.add_argument(
        "--output",
        help=f"new export directory (default: {DEFAULT_OUTPUT})",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.check_templates:
            require(args.maintainer is None and args.output is None, "--check-templates takes no export options")
            validate_templates()
            print("OpenWrt upstream templates: OK")
            return 0
        if args.validate_only is not None:
            require(args.maintainer is None and args.output is None, "--validate-only takes no export options")
            output = safe_output_path(args.validate_only)
            maintainer = validate_export(output)
            identity = maintainer if maintainer is not None else "maintainer omitted"
            print(f"OpenWrt upstream export: OK ({identity})")
            return 0

        require(
            args.maintainer is not None,
            "--maintainer is required for an upstream submission export",
        )
        maintainer = validate_maintainer(args.maintainer)
        validate_templates()
        output = safe_output_path(args.output)
        export_trees(output, maintainer)
        print("OpenWrt upstream export and validation: OK")
        print(f"  packages: {output / NATIVE_RELATIVE}")
        print(f"  LuCI:     {output / LUCI_RELATIVE}")
        print(f"  modes:    {output / MODES_FILE}")
        return 0
    except ExportError as exc:
        print(f"export-openwrt-upstream: ERROR: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"export-openwrt-upstream: ERROR: filesystem operation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
