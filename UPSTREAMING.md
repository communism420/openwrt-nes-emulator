# Upstreaming to the official OpenWrt feeds

This repository keeps its standalone GitHub release intact while also
maintaining two clean submission trees:

| Submit to | Exported subtree | Purpose |
|---|---|---|
| [`openwrt/packages`](https://github.com/openwrt/packages) | `multimedia/nes-emulator` | Native build recipe, reviewed service files, and independently pinned FCEUmm core |
| [`openwrt/luci`](https://github.com/openwrt/luci) | `applications/luci-app-nes-emulator` | LuCI pages, ACL/menu data, and the RPCD bridge |

The trees must be reviewed and submitted as two separate pull requests. Merge
the native package first; the LuCI application deliberately has a runtime and
build-selection dependency on it.

## One-command export and validation

From this repository's root, run:

```sh
python3 scripts/export-openwrt-upstream.py \
  --maintainer 'Your Real Name <your-reachable-email@domain.example>'
```

This creates `build/openwrt-upstream/`, validates both trees, records every
file's SHA-256 and intended mode, and refuses to replace an existing output
directory. It never edits the standalone package recipes. The exporter
requires a real maintainer identity because the official new-package
formality checks require `PKG_MAINTAINER`; it rejects placeholders, GitHub
noreply addresses, and example identities. You can also run either validation
mode independently:

```sh
python3 scripts/export-openwrt-upstream.py --check-templates
python3 scripts/export-openwrt-upstream.py \
  --validate-only build/openwrt-upstream
```

The standalone defaults retain `safety_migration=4` because the standalone
post-install script consumes that marker when upgrading older GitHub APKs. The
official package is a new feed entry and has no such migration script, so the
exporter removes exactly the marker's comment and option from its reviewed UCI
defaults while leaving every functional setting unchanged.

Copy only these generated directories into clean forks of the two official
repositories:

```text
build/openwrt-upstream/openwrt-packages/multimedia/nes-emulator
build/openwrt-upstream/openwrt-luci/applications/luci-app-nes-emulator
```

`SHA256SUMS`, `FILE_MODES`, and the JSON export marker are local review aids;
do not copy them into either upstream repository. The marker appends `-dirty`
to its source revision whenever the project worktree has uncommitted changes;
make the final export from a clean, reviewed commit.

The native recipe downloads the canonical `v1.0.0` project codeload and the
full FCEUmm commit `76f68314ce4213703174108f461c431001dcc204` as two separately
hash-pinned archives. The exporter copies the two local core changes into
`patches-fceumm/`; `Build/Prepare` unpacks the secondary archive and applies
them with OpenWrt's `PatchDir` helper. This keeps the project source, upstream
core provenance, and every downstream patch independently reviewable. The
patches retain their author and DCO sign-off. FCEUmm has since accepted the
savestate change, so the exporter deterministically changes only patch 001's
status line from the original Submitted PR to
`Upstream-Status: Backport` with the exact merged
[FCEUmm commit](https://github.com/libretro/libretro-fceumm/commit/3db086eabeb6608706df330e7991b1bce8d25fba).
The standalone `r19` source patch remains byte-identical.
The recipe binds OpenWrt's quilt refresh target to that separately unpacked
core, so the standard command refreshes the real nested patch stack:

```sh
make package/feeds/packages/nes-emulator/refresh V=s
```

Run the command twice and require the second run to leave
`patches-fceumm/` byte-identical, including the exported Backport metadata.
The FCEUmm pin intentionally remains unchanged for the immutable `v1.0.0`
project source: newer core revisions raise `FCEU_VERSION_NUMERIC` from the
frontend's fixed `9813` to `9900`, so updating only the core would make the
state-format contract inconsistent. A future core update must accompany a new
project release. The ROM-buffer patch remains downstream-specific: generic
frontends may leave
`data` and `size` invalid when FCEUmm advertises `need_fullpath=true`, whereas
nesd deliberately supplies both the path and the exact buffer it hashed.

The former `v1.0.0-r19` tag remains the standalone feed's package revision.
The official recipe deliberately uses the canonical software tag `v1.0.0`
and starts its independent OpenWrt `PKG_RELEASE` at `1`; both tags peel to the
same audited source commit. Future source changes must use a new semantic
software version such as `v1.0.1`, never move either published tag.

After copying the LuCI tree into its fork, stage it and explicitly preserve the
RPCD bridge's executable bit (especially when working from Windows):

```sh
git add applications/luci-app-nes-emulator
git update-index --chmod=+x \
  applications/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator
git ls-files -s -- \
  applications/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator
```

The last command must begin with `100755`; compare the remaining paths with
the generated `FILE_MODES` list before committing.

The export includes `po/templates/nes-emulator.pot`, generated with LuCI's
own scanner. From the root of the LuCI fork, refresh it before committing and
require the command to leave no unexpected diff:

```sh
./build/i18n-sync.sh applications/luci-app-nes-emulator
git diff -- applications/luci-app-nes-emulator/po/templates/nes-emulator.pot
```

Commit the English POT template, but do not add hand-maintained translation
`.po` files; LuCI translations are managed through Weblate.

## Why installing LuCI also installs the emulator

The upstream LuCI recipe declares:

```make
LUCI_DEPENDS:=+luci-base +rpcd +jshn +jsonfilter +cgi-io +nes-emulator
```

The official [`luci.mk`](https://github.com/openwrt/luci/blob/master/luci.mk)
maps `LUCI_DEPENDS` to the package's runtime `DEPENDS`. Consequently the APK
solver on current OpenWrt, and opkg on older IPK-based releases, install
`nes-emulator` when `luci-app-nes-emulator` is installed. The leading `+` also
selects each dependency in the OpenWrt configuration/build graph; it is not a
substitute for the runtime dependency.

`jshn`, `jsonfilter`, and `cgi-io` are direct dependencies because the shipped
RPCD bridge directly sources `jshn.sh`, invokes `jsonfilter`, and exposes the
LuCI upload endpoint through `cgi-io`. `luci-base` is not a promise that these
implementation details will remain installed. The native package has no
reverse LuCI dependency, so the graph is acyclic and the daemon can still be
installed without a web UI.

The standalone GitHub APK pair intentionally retains its exact
`nes-emulator=1.0.0-r19` lock. The official LuCI submission uses the
unversioned `+nes-emulator` dependency so normal feed upgrades and OpenWrt's
package-release lifecycle remain possible. Build-only variables such as
`PKG_BUILD_DEPENDS` or `LUCI_BUILD_DEPENDS` would not cause a router's package
manager to install the daemon.

## Version transition from the standalone feed

OpenWrt compares the package release suffix numerically. The existing
standalone `1.0.0-r19` package therefore sorts after the first official-feed
recipe, `1.0.0-r1`; an ordinary upgrade will not replace it. This does not
affect fresh official-feed installations. Existing standalone users must
remove the paired LuCI/native packages before installing the official pair, or
the project should publish a newer software version and update the native
recipe before opening the PR. The latter is the preferred path when a seamless
migration is required. Do not fake a higher `PKG_RELEASE`: new packages in the
official feed start at `1`.

## Submission order

1. Fork `openwrt/packages` and `openwrt/luci`, and update both forks from each
   repository's current `master` branch. Create a separate, descriptively
   named feature branch in each fork; do not work directly on `master`.
2. Export the trees and copy `multimedia/nes-emulator` into the packages fork.
3. Build and run the native package on representative targets, then submit the
   packages PR first.
4. After the native package is available to the official build, copy
   `applications/luci-app-nes-emulator` into the LuCI fork, build it with the
   packages feed enabled, and submit the LuCI PR.
5. Address review in the relevant fork; do not turn the standalone release
   recipes into the upstream recipes.

The standard OpenWrt feed list places both
[`packages` and `luci`](https://github.com/openwrt/openwrt/blob/main/feeds.conf.default)
in the buildroot. Merge order still matters: a LuCI PR cannot be fully built
until `nes-emulator` is present in the packages feed.

## Local upstream checks

In a clean current OpenWrt buildroot with the two forks configured as feeds:

```sh
./scripts/feeds update packages luci
./scripts/feeds install -p packages nes-emulator
./scripts/feeds install -p luci luci-app-nes-emulator
printf '%s\n' 'CONFIG_PACKAGE_luci-app-nes-emulator=m' >> .config
make defconfig
grep -E '^CONFIG_PACKAGE_(luci-app-nes-emulator|nes-emulator)=' .config
make package/feeds/packages/nes-emulator/compile V=s
make package/feeds/luci/luci-app-nes-emulator/compile V=s
```

Inspect the packaged daemon as well: it must be a dynamically linked PIE
(`readelf -h` reports `Type: DYN`) with a non-executable stack and GNU RELRO.

Also install the resulting LuCI package into a disposable matching test image
without naming the native package manually, then confirm that the package
manager resolves `nes-emulator`, `jshn`, `jsonfilter`, and `cgi-io`. Test the
menu, ACLs, RPCD calls, service lifecycle, upload, playback, uninstall, and
upgrade. An SDK compile alone does not replace router testing.

## Pull-request checklist

- [ ] Re-run `sh scripts/check.sh` and the one-command export.
- [ ] Confirm the exported source URLs are tag-scoped and every declared hash
      matches the downloaded archive. Do not replace assets behind published
      tags.
- [ ] Confirm the FCEUmm secondary download is pinned by its full commit and
      SHA-256, both `patches-fceumm/` files apply with unchanged hunks, and a
      second `make package/feeds/packages/nes-emulator/refresh V=s` is clean.
- [ ] Confirm exported patch 001 names merged FCEUmm commit `3db086eabeb6` as
      a Backport and quilt refresh preserves that status header.
- [ ] Confirm the native recipe starts at `PKG_RELEASE:=1`; leave both
      `PKG_VERSION` and `PKG_RELEASE` unset in the LuCI recipe so `luci.mk`
      uses its Git-derived package version.
- [ ] Build with a current OpenWrt buildroot/SDK for more than one CPU family.
- [ ] Confirm the packaged daemon is PIE and retains OpenWrt's standard ELF
      hardening.
- [ ] Confirm selecting the LuCI package selects the native package.
- [ ] Confirm installing only the LuCI package pulls every direct runtime
      dependency.
- [ ] Confirm the RPCD bridge is staged as mode `100755` and all other file
      modes match `FILE_MODES`.
- [ ] Refresh `po/templates/nes-emulator.pot` with LuCI's i18n scanner and
      confirm that it contains only upstream-layout source paths.
- [ ] Confirm no ROM, BIOS, save, build product, private key, or generated APK
      is present in either PR.
- [ ] Use focused commits with package-prefixed subjects.
- [ ] Create commits with `git commit -s`; author and `Signed-off-by` must use
      the submitter's matching real name and real, non-noreply email.
- [ ] Target the current `master` branches and follow each repository's PR
      template; new packages do not start in a stable release branch.
- [ ] Submit the packages PR first and reference it from the later LuCI PR.

The human identity and Developer Certificate of Origin sign-off are the only
steps this repository cannot automate.

## Primary upstream references

- [`openwrt/packages` contribution guidelines](https://github.com/openwrt/packages/blob/master/CONTRIBUTING.md)
- [`openwrt/luci` contribution guidelines](https://github.com/openwrt/luci/blob/master/CONTRIBUTING.md)
- [Current LuCI package machinery (`luci.mk`)](https://github.com/openwrt/luci/blob/master/luci.mk)
- [Current OpenWrt package machinery (`package.mk`)](https://github.com/openwrt/openwrt/blob/main/include/package.mk)
- [Current package metadata generator](https://github.com/openwrt/openwrt/blob/main/scripts/package-metadata.pl)
- [Current default feed ordering](https://github.com/openwrt/openwrt/blob/main/feeds.conf.default)
