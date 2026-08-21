# OpenWrt NES Emulator — technical reference

This document contains the complete build, runtime, transport, storage, and
release notes. For the project overview and quick start, return to the
[main README](../README.md).

A joke but fully functional NES emulator for OpenWrt, consisting of
the `nesd` daemon, an embedded FCEUmm libretro core, and a LuCI interface.

All NES emulation, PPU rendering, generation of the 256×240 game frame, and
optional JPEG encoding intentionally run on the router CPU. The browser
contains no NES emulator: it converts raw transport pixels or decodes JPEG,
scales the canvas, composites the client FPS OSD, plays PCM audio, and sends
input.

```text
browser / LuCI
  canvas + WebAudio + keyboard/touch/gamepad
                    │
                    │ authenticated LuCI RPC + WebSocket
                    ▼
OpenWrt router
  nesd + statically linked FCEUmm
  CPU emulation + software RGB565 render + JPEG/raw streaming
```

The current package version is **1.0.0-r19**. The project is not currently
submitted to the official OpenWrt feeds.

## Packages

| Package | Contents |
|---|---|
| `nes-emulator` | `nesd`, FCEUmm statically linked into `nesd`, procd init script, and UCI configuration |
| `luci-app-nes-emulator` | LuCI UI, ACL, and authenticated rpcd backend |
| `libretro-fceumm` | Hidden transitional metapackage for upgrading older installations |

New installations do not need a separate `libretro-fceumm` package or a
`fceumm_libretro.so` file. The transitional package exists only so that older
installations can migrate cleanly to `nes-emulator`.

The service runs as a dedicated `nesd` system user with dynamically assigned
unused UID and GID values.
The `/etc/nes-emulator/{roms,saves,system}` directories use mode `0750` and
are not made world-accessible. ROMs are imported through the standard
authenticated LuCI/rpcd workflow; no separate CGI uploader is installed.

On first startup, init/rpcd generates a random API token and stores it in the
separate `/etc/nes-emulator/auth.token` file (`root:nesd`, `0640`), rather
than in the UCI-accessible configuration. During an upgrade, the legacy UCI
option is migrated safely and removed. Read-only LuCI sessions do not receive
the token; opening the game client requires permission to control the
emulator. Normal `/api/*` requests and the `/ws` WebSocket upgrade require the
token; an allowed CORS `OPTIONS` preflight is the only tokenless API exception.
The unauthenticated `/play` endpoint contains only the static client shell.
The standalone game client uses same-origin access to the `nesd` API. The
`allowed_origin` option is needed only for a custom cross-origin client and
accepts a single exact Origin value.

`nesd` intentionally remains a small HTTP/WebSocket service for a trusted
local network and does not terminate TLS itself. LuCI opens the game client
in a separate window and passes the token in the URL fragment, which is not
included in the initial navigation request. The WebSocket API cannot set an
Authorization header, so its upgrade uses a token-bearing query string;
reverse proxies and access logs must redact query strings. The client ignores
`/play?token=…` and removes that parameter from its visible URL; only
`#token=…` or a token recovered from the same tab's session storage can
bootstrap it. A browser's HTTPS-only mode or HSTS policy may still upgrade
`http://router:29876` to HTTPS and block the connection. For such a browser,
allow HTTP specifically for the router's address or place `nesd` behind a
trusted HTTPS reverse proxy; do not expose port `29876` to the Internet. LuCI
keeps this transport note under the collapsed **Connection help** section
instead of presenting a healthy local connection as a service error.

## Pinned FCEUmm

The build never uses a moving `master` branch.

| Field | Value |
|---|---|
| Upstream | `https://github.com/libretro/libretro-fceumm` |
| Commit | `76f68314ce4213703174108f461c431001dcc204` |
| Commit date | `2026-07-24` |
| Codeload SHA-256 | `b067ebd0a973751e9b5af56f5b54d74d0a6e67349549b392a4615d3f0d44f031` |
| License | GPL-2.0-only |

The standard SDK build and the standalone APK script use the same FCEUmm
source set. `fceumm_bind.o` is mandatory, and the core is linked directly
into `/usr/bin/nesd`. Both build paths add `-static`: the resulting ELF has
no `PT_INTERP` or `DT_NEEDED` entries, so `nesd` requires no separate runtime
libraries.

Both build paths also apply the same two small local hardening patches to the
pinned source. One makes FCEUmm propagate malformed savestate parser failures
instead of reporting a false success. The other lets the host preserve the
original filename for region/resource detection while FCEUmm loads the exact
immutable ROM buffer already validated and hashed by `nesd`. The pristine
upstream archive hash, both patch hashes, and the resulting patched-tree hash
are recorded in release provenance.

## Building in an OpenWrt SDK/buildroot

The package definitions and build instructions target both release series:

- OpenWrt 24.10 produces an `.ipk`;
- OpenWrt 25.12 produces an `.apk`.

The repository CI uses a static host-musl build rather than compiling both
complete OpenWrt SDK targets, so verify these source-build paths on the
intended SDK before distribution.

Build on Linux using a case-sensitive filesystem.

### Adding the local feed

From the root of the OpenWrt SDK/buildroot:

```sh
printf '%s\n' \
  'src-link nesemu /absolute/path/to/openwrt-nes-emulator/package' \
  >> feeds.conf
./scripts/feeds update nesemu
./scripts/feeds install -a -p nesemu
```

The same template is available in `feeds.conf.example`.

### Selecting and building

```sh
make menuconfig
# Games -> nes-emulator
# LuCI -> 3. Applications -> luci-app-nes-emulator

make package/nes-emulator/download V=s
make package/nes-emulator/compile V=s
make package/luci-app-nes-emulator/compile V=s
```

The `download` step verifies the pinned SHA-256 of the source archive. A
separate `package/libretro-fceumm/compile` build is not needed for new
installations.

## Self-contained APK v3 packages

`scripts/build-apks.sh` creates local packages for all 35 package ABIs in the
official OpenWrt 25.12 branch, using Zig, pinned OpenWrt 25.12.5 toolchains,
and apk-tools v3. The script does not publish anything.

Requirements:

- Linux, GNU `make`, GNU `tar`, `gzip`, `zstd`, `sha256sum`, `find`, `sort`,
  `xargs`, `file`, `readelf`, and `patch`;
- Zig (set its path with `ZIG=...`);
- `zstd` (if necessary, set its path with `ZSTD=...`);
- apk-tools v3 with the `mkpkg`, `mkndx`, `adbdump`, and `verify` applets
  (set its path with `APK=...`);
- `curl` or `wget`;
- root privileges or enabled unprivileged user namespaces, which are required
  to guarantee `root:root` ownership of the APK payload.

```sh
ZIG=/opt/zig/zig \
APK=/opt/apk-tools/apk \
bash scripts/build-apks.sh
```

For a quick build of selected targets, set, for example,
`ARCHES=x86_64,mipsel_24kc`. Without this variable, the script builds exactly
the 35 architectures in the official OpenWrt 25.12 package matrix.

Stock Zig is insufficient for older ARM, soft-float MIPS64, and older PowerPC
(464FP and 8548). The script automatically downloads the appropriate official
OpenWrt 25.12.5 toolchains. The URL and SHA-256 of every archive are pinned in
the script. Extracted toolchains are cached, and their version, hash, and
selected ABI are included in the binary fingerprint.

The `WORK` and `CACHE` paths can be overridden. The script creates a separate
directory with `mktemp`, removes only that directory after checking a marker
file, and never modifies `FCEUMM_SRC`. If `FCEUMM_SRC` is set, a Git worktree
must contain exactly the pinned commit; an extracted source tree is accepted
only when its complete content hash matches.

The binary cache accounts for the OpenWrt architecture, target, endianness,
floating-point ABI, compiler versions and flags, all `nesd` host code, and
the pristine and patched FCEUmm tree SHA-256 values. Every new and cached ELF
is checked with `readelf` for its
class, byte order, ISA/ABI, and the absence of `PT_INTERP` and `DT_NEEDED`.
The file itself is also verified by SHA-256.

The output is first assembled completely in an adjacent staging directory and
then published to `OUT` as a single snapshot. After a successful build, the
entire previous contents of `OUT`, including old APKs, indexes, and public
keys, are replaced. Keep any required historical artifacts outside `OUT`.

The script places a managed-output ownership marker in the generated `OUT`.
A non-empty user-provided `OUT` without this marker is not replaced; the only
migration exception is the project's standard `dist/apk/` directory. System
root directories are forbidden as `OUT`.

### Output layout

```text
dist/apk/
├── INSTALL.txt
├── keys/                         # optional public key only
├── sources/
│   ├── openwrt-nes-emulator-1.0.0-r19-source.tar.gz
│   └── SHA256SUMS
├── aarch64_cortex-a53/
│   ├── nes-emulator-1.0.0-r19.apk
│   ├── luci-app-nes-emulator-1.0.0-r19.apk
│   ├── packages.adb
│   └── SHA256SUMS
└── <34 more architectures>/...
```

The output contains 70 APKs: two packages for each of the 35 ABIs.
Supported `apk --print-arch` values:

```text
aarch64_cortex-a53            aarch64_cortex-a72
aarch64_cortex-a76            aarch64_generic
arm_arm1176jzf-s_vfp          arm_arm926ej-s
arm_cortex-a15_neon-vfpv4     arm_cortex-a5_vfpv4
arm_cortex-a7                 arm_cortex-a7_neon-vfpv4
arm_cortex-a7_vfpv4           arm_cortex-a8_vfpv3
arm_cortex-a9                 arm_cortex-a9_neon
arm_cortex-a9_vfpv3-d16       arm_fa526
arm_xscale                    armeb_xscale
i386_pentium-mmx              i386_pentium4
loongarch64_generic           mips64_mips64r2
mips64_octeonplus             mips64el_mips64r2
mips_24kc                     mips_mips32
mipsel_24kc                   mipsel_24kc_24kf
mipsel_74kc                   mipsel_mips32
powerpc64_e5500               powerpc_464fp
powerpc_8548                  riscv64_generic
x86_64
```

This is complete coverage of the CPU package ABIs in the current OpenWrt
25.12 release, not a list of individual router models. A compatible ABI does
not guarantee that a particular older router has enough flash storage, RAM,
or performance for emulation and software video encoding.

To determine the router architecture:

```sh
apk --print-arch
# or, on an older system:
. /etc/os-release
printf '%s\n' "$OPENWRT_ARCH"
```

### Unsigned local installation

Verify the checksums first:

```sh
cd dist/apk/aarch64_cortex-a53
sha256sum -c SHA256SUMS
scp nes-emulator-1.0.0-r19.apk \
  luci-app-nes-emulator-1.0.0-r19.apk root@192.168.1.1:/tmp/
```

On the router:

```sh
apk --allow-untrusted add \
  /tmp/nes-emulator-1.0.0-r19.apk \
  /tmp/luci-app-nes-emulator-1.0.0-r19.apk
```

The LuCI package requires exactly the same revision of the native package, so
`r18` and `r19` intentionally cannot be mixed. When upgrading, always pass both
local APKs to a single `apk add` command: one transaction lets the dependency
solver replace the old native and LuCI packages together.

`--force-overwrite` is neither required nor recommended.

### Local feed

Serve the contents of the relevant architecture directory over HTTP(S) and
add the direct URL of its index:

```sh
cat >/etc/apk/repositories.d/nes-emulator.list <<'EOF'
https://example.invalid/nes-emulator/aarch64_cortex-a53/packages.adb
EOF
# For an unsigned feed:
apk --allow-untrusted update
apk --allow-untrusted add nes-emulator luci-app-nes-emulator
```

Replace the URL and architecture with your own values. For a signed feed,
install the public key first, then run the same commands without
`--allow-untrusted`.

### Feed signing

Keys are not required for a normal local build. A signed build requires an
RSA PEM key pair, which can be generated with OpenSSL, and both paths must be
provided:

```sh
# Run once in a protected directory:
umask 077
openssl genrsa -out nes-emulator-private.pem 4096
openssl rsa -in nes-emulator-private.pem -pubout \
  -out nes-emulator-public.pem
chmod 0644 nes-emulator-public.pem

SIGNING_KEY=/secure/path/nes-emulator-private.pem \
SIGNING_PUBKEY=/secure/path/nes-emulator-public.pem \
ZIG=/opt/zig/zig \
APK=/opt/apk-tools/apk \
bash scripts/build-apks.sh
```

The private key is never copied into `dist/`. The public key is placed in
`dist/apk/keys/`; copy it to `/etc/apk/keys` before installation. Both the
packages and `packages.adb` are signed with the same key. Without keys, the
script creates a valid unsigned feed and explicitly documents the need for
`--allow-untrusted`.

## Corresponding Source and licenses

Every standalone build includes a deterministically normalized and
checksummed corresponding-source archive:

```text
dist/apk/sources/openwrt-nes-emulator-1.0.0-r19-source.tar.gz
```

It contains the public project metadata and documentation, CI configuration,
host/LuCI code, package Makefiles, build and test scripts, the exact pristine
extracted FCEUmm tree, the two local hardening patches, `PROVENANCE.txt`, and
file-by-file `SOURCE-SHA256SUMS`. A SHA-256 checksum of the archive itself is
generated alongside it.

After extracting the archive, it can be rebuilt without another FCEUmm
download by passing its verified pristine tree back to the same script:

```sh
FCEUMM_SRC="$PWD/third_party/libretro-fceumm" \
ZIG=/opt/zig/zig APK=/opt/apk-tools/apk \
bash scripts/build-apks.sh
```

The script verifies that pristine tree, applies both bundled patches exactly once,
and checks the resulting patched-tree hash before compiling.

- The host and LuCI source code is licensed under the MIT License; see
  `LICENSE`.
- FCEUmm is licensed under GPL-2.0-only; see `Copying` inside the source
  bundle.
- The combined `nesd` binary with FCEUmm is distributed under the terms of
  GPL-2.0-only.

The MIT notice is also installed in both APKs. The main package places it
next to `FCEUmm-Copying` in `/usr/share/licenses/nes-emulator/`; the LuCI
package places it in `/usr/share/licenses/luci-app-nes-emulator/`.

## Local debug build

After modifying `package/nes-emulator/src/play.html`, update the embedded
header:

```sh
python3 scripts/embed-play-html.py
# Verify without modifying files (for example, in CI):
python3 scripts/embed-play-html.py --check
```

Python is needed only to generate the source file and is not a runtime
dependency of the package.

A normal host build also requires the pinned FCEUmm tree:

```sh
cd package/nes-emulator/src
make FCEUMM_DIR=/path/to/libretro-fceumm \
  FCEUMM_GIT_VERSION=76f68314ce42
./nesd --help
```

The Makefile creates `.d` dependency files, so header dependencies are tracked
automatically. Run `make clean` before rebuilding after changing compiler or
linker flags.

## Operation

After installation, open LuCI and go to **Services -> NES Emulator**. Do not
open the daemon port on the WAN. For remote access, use a VPN or another
authenticated tunnel.

A fresh installation does not start `nesd` automatically. The Overview page
lists ROMs without starting the emulator, while opening Play or uploading a
ROM starts the lightweight daemon on demand. The **Start at boot** option
explicitly enables persistent automatic startup. FCEUmm is initialized only
after a ROM is selected; without a ROM, the daemon generates no demo video or
PCM audio. Only one media client is allowed, and hiding the game tab
immediately closes the WebSocket.

### Custom controls

The game window includes an interactive **Controls** panel. Each NES button
has primary and alternate keyboard bindings: click a binding, then press the
physical key to assign. Backspace or Delete clears the selected slot, Escape
cancels capture, and **Restore defaults** restores arrows/WASD, Z=B, X=A,
Enter=Start, and either Shift=Select.

Bindings are validated before use, so one physical key cannot silently control
two different NES buttons. They are saved in browser storage for the current
router address, allowing different computers or users to keep independent
layouts. When enabled, touch controls remain available; the standard browser
gamepad mapping is always unaffected by keyboard customization.

### Fullscreen and picture shape

The game window can expand the complete game stage to fullscreen. The stage
keeps the picture controls and, when enabled, touch buttons available, so a
phone or tablet does not require an Escape key to leave fullscreen. Desktop
users can also leave with the browser's normal Escape shortcut. Entering,
leaving, or being
denied fullscreen releases held controller input to prevent stuck buttons.
The runtime fullscreen marker keeps this layout active without depending on
whether the browser supports or has already applied the CSS `:fullscreen`
pseudo-class. The complete 256x240 frame is fitted and centered inside the
fullscreen viewport's actual content box, including safe-area insets. The
picture toolbar and touch controls are absolute overlays excluded from normal
flow. The layout is designed and tested to keep the complete frame inside the
visible viewport.

The **Picture** selector defaults to **4:3 (original display)**, matching the
intended television presentation of the console. **16:9 (stretch)** fills a
widescreen shape without cropping. The selection is validated and saved in
browser storage for the current router origin; if storage is blocked, it still
applies for the current tab.

Both choices are browser-side scaling of the completed frame. They do not
change the canvas backing frame, move emulation into JavaScript, or offload
rendering from the router: FCEUmm and the software renderer still run entirely
on the router CPU.

### Save states

The game window provides ten full-machine save slots for the selected ROM.
Choose a slot, optionally give it a short label, and use **Save state**,
**Load state**, or **Delete**. **Refresh** rescans the slots without changing
the emulation state. A snapshot includes the FCEUmm CPU, RAM, PPU, APU, mapper,
and other serialized console state; it is not merely a copy of battery-backed
SRAM.

Slots are associated with the SHA-256 of the exact ROM snapshot loaded by the
core and its effective PAL/NTSC region, rather than with the path alone.
Renaming or moving an unchanged ROM therefore keeps its states when the rename
does not change filename-based region detection. Replacing a file with
different contents, or renaming an ambiguous iNES v1 ROM so that its effective
region changes, selects a different set of slots. Files live in
`${save_dir}/states`, a private `0700` directory, and are atomically written
with mode `0600`.

The architecture-independent wrapper records the format version, ROM and core
identity, effective region, payload size, label, timestamp, a portable RGB565
screenshot, and SHA-256 checksums for both the core payload and screenshot.
The screenshot lets a paused load immediately show the saved frame without
advancing emulation by one frame. The pinned FCEUmm serializer performs the
required endian conversion, so the format is designed to be portable between
supported router ABIs running the same core revision. A cross-ABI round trip is
not executed on every one of the 35 release architectures.

Each payload is limited to 4 MiB, the state directory is limited to 16 MiB,
and saving preserves an 8 MiB filesystem reserve. Corrupt or incompatible
slots remain visible but cannot be loaded; they can still be deleted or safely
overwritten. Slot listing reads only bounded headers, so opening or refreshing
the panel does not hash up to 16 MiB in the emulation thread; the complete
payload, screenshot, checksums, and FCEUmm chunk structure are validated before
every load. Loading is serialized with emulation, creates an in-memory rollback
snapshot, and preserves whether the game was running or paused. It also durably
writes the restored SRAM and clears stale controller, audio, and video queues
before the client resumes. If both loading and rollback ever fail, emulation is
stopped and every connected client receives an explicit fatal status event.

Additional USB/SD directories are disabled by default with the **Enable extra
ROM directories** flag (`extra_rom_dirs_enabled=0`). While the flag is off,
saved `extra_rom_dir` values are not validated, passed to `nesd`, or scanned
by rpcd; LuCI hides the list but preserves its values. To restore the
directories, enable the flag and the list will reappear with its previous
paths. An empty entry in the optional list does not prevent the form from
being saved. The `:`, `;`, and `,` characters are forbidden in paths because
they are delimiters in the `-e` protocol.

The current one-time configuration migration is identified by
`safety_migration=4`. It replaces the previous default port `9090` with
`29876`: `9090` is frequently occupied by local proxy management services
such as OpenClash, which either prevented `nesd` from starting or disrupted
the proxy and Internet access. Any other valid custom port from 1 through
65535 is preserved, while an invalid value is replaced with `29876`.

For configurations at the previous marker `1`, the historical combination of
`stream_format=raw` and `stream_fps=5` is also changed to a conservative
2 FPS. Any other valid value from 1 through 60 is preserved, while an invalid
value is replaced with 2. For a configuration with no marker, or with an
older marker, the migration also disables inherited automatic startup
(`enabled=0`). When upgrading from the previous marker `3`, saved additional
paths remain in place, but the gate is safely set to
`extra_rom_dirs_enabled=0`; they can be enabled with one flag after review.
If the marker is already `4`, user settings are no longer modified. Paths and
the selected ROM are preserved in every case.

Emulation runs at the core's native cadence, approximately 60 FPS for NTSC or
50 FPS for PAL, while `stream_fps` independently limits the transmitted frame
rate to 1–60 FPS, with a conservative default of 2 Hz. One uncompressed
256x240 RGB565 frame occupies 120 KiB: approximately 2.0 Mbit/s at 2 FPS and
59.0 Mbit/s at 60 FPS. Uncompressed 48 kHz stereo int16 PCM adds approximately
1.54 Mbit/s, so the combined raw video and audio stream is about 3.5 Mbit/s
at 2 FPS and 60.5 Mbit/s at 60 FPS, before WebSocket and Wi-Fi overhead.
Consequently, a raw stream at the maximum setting can still consume a
significant share of airtime; use 1–2 FPS or JPEG on a slow or shared Wi-Fi
network. The 60 FPS setting is a ceiling, not a promise that undersized
hardware or a congested radio can carry a 60.5 Mbit/s stream.

The **Show FPS counter** setting (`show_fps=1` or `0`) is enabled by default.
Its FCEUX-like pixel OSD draws only the measured number directly over the NES
canvas in the top-right corner; it is part of the game picture rather than a
separate DOM widget. The number counts completed client canvas updates
after raw-frame coalescing, JPEG decoding, and stale-frame dropping. It
therefore exposes delivery, decode, paint, or browser slowdowns; it is neither
the core's nominal 50/60 FPS metadata nor a restatement of the configured
stream ceiling. Disabling it removes the OSD and stops its one-second sampling
timer. Save & Apply restarts the service, and an open game window adopts the
new value when it reconnects. In fullscreen, the display toolbar stays outside
the top-right OSD zone: it sits at the bottom center when touch controls are
hidden and becomes a compact top-left column when they are visible.

The **Show on-screen controls** setting (`show_touch_controls=1` or `0`) is
also enabled by default, preserving the touch-friendly D-pad and NES buttons
for phones and tablets. Desktop users can disable it to remove the entire
clickable control area while keeping keyboard and gamepad input available.
The authenticated game window requests this preference as soon as it loads,
without starting the media stream, and follows later status messages whenever
the stream reconnects. Hiding the area also releases any pointer button that
was held at that instant, preventing a stuck direction or action. Save & Apply
makes either choice persistent, and the same flag restores the controls
whenever they are needed again.

With an active viewer, the media loop sleeps until the next absolute video
deadline instead of rounding 60 FPS to a fixed 10 ms polling interval. The
effective delivery ceiling never exceeds the ROM region's native cadence, so
a PAL game configured for 60 FPS is transmitted at an even 50 FPS instead of
being sampled in an irregular 50/60 Hz pattern. Audio is packetized every
40 ms rather than once per video wake-up, which substantially reduces
`AudioBuffer` and `AudioBufferSource` churn in the browser.
The transport pull buffer covers the host ring's complete 100 ms freshness
window, so normal 40 ms pacing jitter cannot truncate a three-frame FCEUmm
audio batch.

The streaming path is explicitly latency-bounded. The daemon limits the TCP
send buffer, uses `TCP_NOTSENT_LOWAT` and `TCP_USER_TIMEOUT` when the OpenWrt
kernel supports them, caps the work performed per writable-socket turn, and
keeps only bounded control, heartbeat, status, audio, and video slots. Fresh
controller input is read before output is flushed. Pending audio uses an
ordered three-slot FIFO capped at 120 ms: this preserves short PCM jitter behind
an in-flight video WebSocket frame, while overflow drops the oldest whole
packet instead of growing latency without bound. Pending video never grows
beyond one frame, and queued media is dropped once it is stale. The host audio
ring likewise discards an old network backlog and retains at most the newest
100 ms instead of replaying a long burst after Wi-Fi recovers.

JPEG compression still runs entirely on the router CPU, but it no longer runs
inside the network event loop. A lower-priority encoder thread owns one active
frame and one latest-only pending frame; additional submissions replace that
pending frame. A completion pipe wakes the network loop, and viewer/session or
state changes invalidate unfinished results. Slow software encoding therefore
cannot block controller input, heartbeat acknowledgements, audio, or HTTP, and
its memory use cannot grow with the backlog. If the optional worker cannot be
created because resources are exhausted, `nesd` stays online and falls back to
raw RGB565 streaming instead of terminating the emulator service.

HTTP response bodies are copied into bounded nonblocking output buffers; a
client that opens `/play` and then stops reading cannot pause WebSocket media
or controller processing. An active game also limits each ROM-directory scan
to a short time budget and reports `truncated=true` rather than walking
thousands of files in one network-loop turn. ROM upload timeouts use a
20-second sliding inactivity deadline plus a size-derived total allowance at
an 8 KiB/s floor, capped at 30 minutes. The browser adds a 10-second response
grace period. This lets a genuinely slow but progressing LAN upload finish
without allowing a stalled authenticated upload to occupy the slot forever.

The browser sends a complete controller-state heartbeat every second. Each
heartbeat has a strictly increasing safe-integer sequence number, and only a
matching, recently issued acknowledgement from the current socket proves that
the path is healthy; delayed acknowledgements from an old TCP backlog do not.
Video or audio arriving in only one direction is likewise not treated as proof
that controller input can reach the router. A connection attempt or fresh-ACK
silence lasting 9 seconds, or excessive browser WebSocket output buffering,
abandons that transport and starts a jittered exponential reconnect capped at
8 seconds. The backoff is reset only after 5 seconds of confirmed bidirectional
operation. On the server, matched WebSocket Ping/Pong deadlines and TCP progress
deadlines release the single viewer slot promptly. The bundled client's `hello`
message also enables a six-second application-heartbeat lease, so a dead upload
path cannot leave an otherwise flowing download stream occupying that slot.
A separate 3.5-second input lease guarantees that a lost client cannot leave a
NES button held indefinitely.

Hidden/offline pages suspend the transport while preserving the user's desire
to play. Visibility, `online`, and BFCache `pageshow` transitions resample the
network state, resume an existing audio context, and establish exactly one new
WebSocket. The browser's `navigator.onLine` value is only a hint: it never tears
down an already proven LAN socket, and a missing socket is probed immediately
once before failed offline retries settle into a jittered 6–8 second cadence.
Raw browser frames are coalesced to one conversion per animation paint. JPEG
decoding has one latest pending frame per stream epoch: a completed
current-epoch decode is allowed to paint even when a newer frame is waiting,
which prevents decode starvation. A two-second decoder deadline starts one
fresh transport epoch when a decoder hangs. A global limit of two unresolved
decode operations then stops automatic retries and preserves only the newest
pending frame, preventing a broken browser codec from causing a reconnect storm
or unbounded memory growth; reloading the page or selecting raw video remains a
deterministic recovery path if the browser's JPEG decoder itself is unusable.

These mechanisms prefer a fresh picture and prompt controls over replaying
obsolete media, but they cannot repeal the bandwidth calculation above. If a
radio cannot carry PCM plus even one selected video format at a useful rate,
choose JPEG and/or lower `stream_fps`; reconnecting or buffering cannot make a
60.5 Mbit/s raw stream fit through a slower link. The daemon uses `nice=5`, so
normal router services retain scheduler priority while all emulation and
software rendering still intentionally remain on the router CPU.

When there are no WebSocket viewers, the worker blocks in a low-CPU wait state
and stops emulating or rendering frames. Battery-backed SRAM is loaded
automatically. This periodic battery-save mechanism is separate from the
manual full-machine save states described above.
Every 30 seconds, changed SRAM is copied into a bounded latest-only slot by
the emulation thread. A dedicated writer performs the atomic
`write`/`fsync`/`rename`/directory-`fsync` sequence, so periodic flash I/O
never blocks frame emulation. Pause, stop, reset, ROM switching, manual flush,
and clean shutdown still wait for the relevant durable save.

The importer accepts NES/FDS/UNIF ROMs no larger than 16 MiB, validates their
extension and file-format signature (magic bytes), enforces the quota and
free-space reserve, and then atomically moves the file into the ROM directory.

During installation or upgrade, supported regular ROM files directly inside
`/etc/nes-emulator/roms` receive the safe ownership `root:nesd` and mode
`0640`; symlinks and hard links are not modified. LuCI uploads create ROMs
with the correct permissions immediately. If a ROM is later copied manually
with mode `0600`, run
`chown root:nesd FILE && chmod 0640 FILE`. Until the permissions are fixed,
the running daemon still lists such a ROM as inaccessible and blocks loading
it with an explicit hint; the background read-only scan intentionally does
not modify files as root. For nested and external directories, the
administrator must grant the `nesd` user read permission and directory
traversal access.

At startup, the daemon removes only regular files left by a crashed process
that match its own strict `.nes-upload-…tmp` naming scheme; live PIDs,
symlinks, and unrelated hidden files are untouched. rpcd similarly cleans up
safely recognized remnants of interrupted imports.

Use ROMs only when you have the legal right to do so.

## Historical artifacts

Do not mix APKs from different builds in one feed. Before running the script
again, move any required historical files from `OUT` to a separate archive
directory: a successful build atomically replaces `OUT` with only the current
packages and indexes.
