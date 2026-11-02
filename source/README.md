# PS4 AirPlay 1.12-RC13

Standalone PS4 HEN AirPlay receiver based on 0.13. A Mac is needed only to build
or develop the project, not to receive media.

This candidate adds an English/Russian interface, shared language controls on
TV and the diagnostics page, and an Options settings menu. Exit now stops
request callbacks and the direct-video worker before freeing protocol/session
data, withdraws discovery services, and requests a return to the PS4 shell.
Discovery uses a renewable 15-second lease to limit stale entries after crashes.

Music, mirroring, and Safari video have worked on PS4 13.52 in earlier builds.
RC13's new Options/exit behavior still needs hardware validation. The unresolved
00:00 timer on some long videos is not claimed fixed. YouTube is outside the
current validation scope. DRM, HEVC, and AV1 are unsupported.

Install `PS4-AirPlay-1.12-RC13.pkg` over the previous version with HEN enabled.
English is the default. Press Options, select Language, and press X to switch to
Russian. Up/Down selects a menu item; Circle returns. Select Disconnect AirPlay
and exit to quit cleanly. Language is retained for the current app session.

Diagnostics: `http://PS4-IP:9080/`. Use Download report before restarting a failed
session. After a crash, reopen and save the report before resuming AirPlay.
The page has a 12-second H.264/AAC test video and a separate native player test.
Do not publish personal reports without checking their contents.

Direct video uses FFmpeg libavformat, software H.264/AAC decoding, native HTTP,
and verified OpenSSL HTTPS. The direct path does not use Sony AvPlayer/Audiodec.
It supports MP4, MPEG-TS HLS, and fragmented MP4 with Range, pause, and seek.
Actual quality and seek availability depend on source metadata and format.
Some fragmented MP4 streams and high-bitrate videos can stutter.

Build with OpenOrbis and the complete dependency tree included in this archive:

```sh
export OO_PS4_TOOLCHAIN=/absolute/path/OpenOrbis/PS4Toolchain
export PS4_CLANG=/absolute/path/clang
export PS4_LLD=/absolute/path/ld.lld
export PS4_FSELF=/absolute/path/create-fself
export PS4_PKGTOOL=/absolute/path/pkgtool-wrapper
export PS4_AIRPLAY_PORT="$PWD/work/airplay-port"
python3 source/scripts/build_core.py
python3 source/scripts/build.py
```

When running from inside `source/`, adjust script paths and set the port path to
`../work/airplay-port`. See `docs/BUILD-APPLE-SILICON.md` and `docs/DEPENDENCIES.md`.
The full source archive includes pinned dependency sources, CA bundle/license,
static libraries, and validation logs; proprietary Sony modules are excluded.

Host checks: `scripts/test_release.sh`, `scripts/test_airplay_protocol.py`,
`tests/test_diagnostics.py`. They exercise real core/FFmpeg behavior with
sanitizers, but do not replace a physical iPhone/PS4 check.

GPL-3.0-or-later; third-party components retain their respective licenses.
