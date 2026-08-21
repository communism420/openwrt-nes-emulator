# Contributing

Thanks for helping improve the world's least necessary router workload.

## Before opening an issue

- Search existing issues first.
- Confirm the problem still exists with the latest package revision.
- Include the OpenWrt version, router model, `DISTRIB_ARCH` from
  `/etc/openwrt_release` (or `OPENWRT_ARCH` on 24.10), stream format, FPS
  limit, and relevant `logread` output.
- Remove bearer tokens, public IP addresses, Wi-Fi credentials, ROM names you
  do not want to disclose, and other private data.
- Do not attach ROMs, BIOS files, save data, router backups, or signing keys.

Security problems follow [SECURITY.md](SECURITY.md), not the public bug form.

## Development workflow

The project is primarily C, POSIX shell, JavaScript, and Python. Build and test
on Linux or WSL using a case-sensitive filesystem.

1. Fork and clone the repository.
2. Create a focused branch.
3. Make the smallest coherent change.
4. If `package/nes-emulator/src/play.html` changed, regenerate its embedded
   header:

   ```sh
   python3 scripts/embed-play-html.py
   python3 scripts/embed-play-html.py --check
   ```

5. Run the complete local contract suite:

   ```sh
   sh scripts/check.sh
   ```

6. Describe the user-visible behavior, failure cases, and tests in the pull
   request.

The GitHub Actions workflow additionally downloads the hash-pinned FCEUmm
tree, applies the two verified local patches, creates a static musl build, and
runs black-box integration tests.

Changes intended for the official OpenWrt feeds must also follow the separate
[upstream export, validation, and submission checklist](UPSTREAMING.md).

## Code expectations

- Keep router services responsive. Do not turn a media backlog into unbounded
  memory, latency, filesystem work, or reconnect churn.
- Treat every filename, HTTP value, WebSocket message, ROM, and save state as
  untrusted input.
- Preserve the thin-client boundary: NES emulation and game-frame generation
  remain on the router.
- Prefer explicit limits, atomic file replacement, and fail-closed validation.
- Update tests and documentation with behavior changes.
- Preserve unrelated work and avoid generated build products in commits.

## ROMs and test data

Commercial ROMs and proprietary BIOS files are never accepted. Tests must use
small project-authored fixtures or data generated during the test run. FDS
users must provide their own legally obtained `disksys.rom`; it is not part of
this project.

## Licensing contributions

By submitting a contribution, you agree that project-authored code and
documentation may be distributed under the repository's MIT terms. Changes
derived from FCEUmm must remain compatible with GPL-2.0-only and should be kept
as clearly identified patches. Do not submit code or assets whose provenance
or redistribution rights are unclear.
