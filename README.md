# PS4 AirPlay

A standalone AirPlay receiver for a HEN-enabled PS4. Stream music, mirror your
screen, or send video from an iPhone player. No computer is required during use.

**Latest build: [1.12-RC13](https://github.com/vivvd/ps4airplay/releases/tag/v1.12-rc13).**
This is a release candidate based on the working 0.13 receiver. Music, mirroring,
and Safari video have worked on the user's PS4 running firmware 13.52 in earlier
builds. RC13's new settings and exit behavior still need a console check.

![Music screen with example artwork](docs/now-playing-preview.png)

## Install

1. Download **PS4-AirPlay-1.12-RC13.pkg** from
   [Releases](https://github.com/vivvd/ps4airplay/releases).
2. Enable your compatible HEN, install the package, and open PS4 AirPlay.
   Updates use the same title ID: `ARPL00001`.
3. Connect PS4 and iPhone to the same local network. Wait for **Ready to play**,
   then choose **PS4 AirPlay** on your iPhone.

Firmware 13.52 is the tested setup; other firmware/HEN combinations are not
claimed to work. This project does not install or provide HEN.

## Playback

- **Apple Music:** audio, artwork, title, artist, album, and progress when sent
  by iPhone. Control playback on your phone.
- **Screen Mirroring:** select the receiver from Control Center → Screen Mirroring.
- **Safari video:** use the AirPlay button inside the video player. Direct video
  uses FFmpeg H.264/AAC decoding, native HTTP, and OpenSSL HTTPS with certificate
  and hostname verification. HTTP(S) MP4 and HLS are supported within these limits.

**Known limits:** DRM video, HEVC, and AV1 are unsupported. Compatibility with
all websites is not guaranteed. Some long videos play while the iPhone timer
stays at 00:00 and seeking is unavailable; this is still under investigation.
High-bitrate sources and some fragmented MP4/HLS streams can stutter because
video decoding is done in software. YouTube compatibility is not currently
supported or being validated.

## Controls and language

| Button | Action |
| --- | --- |
| Cross / X | Pause or resume direct video |
| Circle / O | Stop direct video |
| Square | Show or hide diagnostics |
| Triangle, while diagnostics are visible | Display color bars |
| Options | Open Settings |

In Settings, use Up/Down and X to choose **Language**, **Back**, or
**Disconnect AirPlay and exit**. Circle or Options closes Settings.
English is the default; Russian is also available. The language selector on the
diagnostics page changes the app language too. Language selection lasts for the
current app session.

RC13 stops protocol callbacks before freeing video-session data and closes the
receiver before returning to the PS4 menu. Discovery records expire after
15 seconds and are renewed every 5 seconds while the app is running. A forced
close or crash cannot send a goodbye packet, and iOS may temporarily retain a
cached device or selected route. If necessary, select iPhone in the AirPlay menu.

## Diagnostics

Open `http://PS4-IP:9080/` in Safari. The address is shown on TV. The page includes
English/Russian language selection, a 12-second video with sound, a native player
test, mirroring profiles, and **Download report**.

Download a report before restarting after a playback failure. After a crash,
reopen the app and save the report before resuming AirPlay. Reports can contain
network and device information; review them before sharing publicly.

Mirroring profiles are upper limits selected by the sender. Change a profile,
then use Options → Disconnect AirPlay and exit and reopen the app. These
profiles do not control the resolution of direct video.

## Build

App source is in `source/`; the patched UxPlay core is in
`work/airplay-port/vendor/uxplay-lib/`. A complete `*-source.zip` accompanies the
release and includes pinned dependency sources and PS4 static libraries.

```sh
python3 scripts/bootstrap_dependencies.py
export OO_PS4_TOOLCHAIN=/absolute/path/OpenOrbis/PS4Toolchain
export PS4_CLANG=/absolute/path/clang
export PS4_LLD=/absolute/path/ld.lld
export PS4_FSELF=/absolute/path/create-fself
export PS4_PKGTOOL=/absolute/path/pkgtool-wrapper
export PS4_AIRPLAY_PORT="$PWD/work/airplay-port"
python3 source/scripts/build_core.py
python3 source/scripts/build.py
```

Bootstrap verifies the dependency archive's SHA256 and leaves tracked app/core
source in place. You can use a local archive with `--archive /path/to/archive.zip`.
See [Apple Silicon build notes](source/docs/BUILD-APPLE-SILICON.md) and
[dependencies and licenses](source/docs/DEPENDENCIES.md).

## Validation

Host checks exercise the real protocol core and FFmpeg engine under ASan/UBSan:
HTTP/HTTPS MP4, HLS MPEG-TS and fMP4, pause/seek/stop, encrypted setup, pairing,
reverse/FCUP playlists, TLS rejection paths, metadata/artwork, and diagnostics.
Native builds also validate package structure and extracted runtime files.
These checks do not prove physical iPhone/PS4 behavior for a new build.

```sh
python3 source/scripts/build_video_ffmpeg.py --host --fast
sh source/scripts/test_release.sh
python3 source/scripts/test_airplay_protocol.py
python3 source/tests/test_diagnostics.py
```

Tests require a C compiler, make, Go, and OpenSSL (`HOST_OPENSSL` can override
its location for protocol tests). See the source archive's `validation/` logs.

## License

GPL-3.0-or-later. Third-party components retain their licenses; see
[LICENSE](LICENSE), `source/third_party/`, `source/assets/fonts/OFL.txt`, and the
dependency notes. Proprietary Sony modules are not distributed.
This project is not affiliated with Apple or Sony.
