# Third-party notices

## FCEUmm

OpenWrt NES Emulator embeds
[libretro FCEUmm](https://github.com/libretro/libretro-fceumm), an emulator
core from the FCEU/FCEUX lineage.

- Pinned commit: `76f68314ce4213703174108f461c431001dcc204`
- License: GPL-2.0-only
- Upstream license file: `Copying`
- Archive and local patch hashes: recorded in
  [the technical reference](docs/TECHNICAL.md#pinned-fceumm) and in each
  release's `PROVENANCE.txt`

The repository's original host, OpenWrt integration, LuCI code,
documentation, and project-authored assets are covered by the MIT terms in
[LICENSE](LICENSE). Because the distributed `nesd` executable statically
links FCEUmm, that combined executable is distributed under GPL-2.0-only.

Each binary release must include the exact corresponding-source archive and
its SHA-256 checksum. GitHub's automatically generated source snapshot is not
a substitute for that archive.

Nintendo software, ROMs, BIOS files, trademarks, and artwork are not included
and are not licensed by this project.
