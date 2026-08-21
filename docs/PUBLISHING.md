# GitHub publication checklist

This repository is prepared for the name **`openwrt-nes-emulator`**. Publishing
is intentionally a separate manual step: the build and CI scripts never create
a GitHub repository, push commits, tags, packages, or keys.

## Repository profile

Suggested description:

> A joke that became a fully functional router-side NES emulator for OpenWrt, powered by FCEUmm and LuCI.

Suggested topics:

```text
openwrt nes emulator fceumm luci libretro embedded router c websocket
```

Upload `docs/assets/social-preview.png` under **Settings → General → Social
preview**. Its editable source is `docs/assets/social-preview.svg`.

After the final repository URL exists, add a dynamic GitHub Actions badge to
the README using that exact owner and repository name. Do not use a placeholder
badge before then.

## Settings worth enabling

- Issues and private vulnerability reporting.
- Secret scanning and push protection, when available for the account.
- A branch ruleset for `main` requiring the CI workflow and blocking force
  pushes.
- Automatically delete head branches after pull requests merge.
- Discussions only if there is enough traffic to justify another support
  channel.

The repository already contains issue forms, a pull-request template,
Dependabot configuration for GitHub Actions, a security policy, and a code of
conduct.

## First commit

Before committing, verify that Git reports this project directory—not a parent
profile directory—as its top level:

```sh
git rev-parse --show-toplevel
git status --short --ignored
```

Choose the repository-local Git name and email deliberately. Use a GitHub
`noreply` address if the personal address should not appear in public commit
metadata. Signed commits are optional, but they improve provenance when the
account is already configured for them.

Generated APKs, indexes, source archives, ROMs, save data, tokens, and private
keys must remain outside Git history. The `.gitignore` contains defense-in-depth
patterns for these files.

## Binary release

Recommended tag format: `v<version>-r<package-release>`, for example
`v1.0.0-r19`.

For a public release, attach:

1. all 70 APKs as separate assets, using the exact ABI as a filename suffix;
2. a top-level checksum file covering every APK and the source archive;
3. `openwrt-nes-emulator-<version>-r<release>-source.tar.gz` from the build;
4. concise release notes copied from `CHANGELOG.md`.

GitHub requires unique asset names. Flatten the per-ABI build directories with
this deterministic naming rule while keeping each APK byte-for-byte unchanged:

```text
nes-emulator-<version>-r<release>-<ABI>.apk
luci-app-nes-emulator-<version>-r<release>-<ABI>.apk
```

The exact corresponding-source archive is required for the statically linked
GPL-2.0-only FCEUmm binary. GitHub's automatic “Source code” archives do not
replace it.

Prefer packages signed with a dedicated offline key. Keep the private key
outside this repository and outside the build output. If the first release is
unsigned, state that clearly and retain the `--allow-untrusted` installation
instructions.

Release notes must tell users to run `apk --print-arch`, download the two APKs
whose names end with that exact ABI, verify their rows from the top-level
checksum file, and install both packages in one transaction.

The automatic installer treats these release details as a stable interface:

- the latest release exposes an asset named exactly `SHA256SUMS`;
- APK names follow the two ABI-suffixed patterns above;
- both APKs for one ABI use the same `version-rN` revision;
- tag-scoped asset URLs remain available after a newer release is published,
  and published assets are never replaced in place.

Changing any of these rules requires updating `install.sh` and
`tests/install_contract.sh` in the same commit.

## Final checks

```sh
sh scripts/check.sh
python3 scripts/embed-play-html.py --check
```

Also verify the release artifacts with `apk verify`, `apk adbdump`,
`sha256sum -c`, `file`, and `readelf`, and confirm that the source archive can
rebuild using the documented tool versions and inputs.
