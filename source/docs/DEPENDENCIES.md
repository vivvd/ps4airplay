# Закреплённые источники

| Компонент | Версия / commit | Использование |
|---|---|---|
| [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/tag/v0.5.4) | v0.5.4, toolchain-llvm-18.tar.gz | PS4 headers, static libc, CRT, import stubs |
| [create-fself](https://github.com/OpenOrbis/create-fself) | f2da6229684ce320f673c06fceac9c218f621788 | ELF → OELF/fSELF |
| [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) | 643477263b2644e0803e0f58b8726ea4e3f3b7d4 | SFO, PKG, validation, extraction |
| LLVM / LLD | 23.1.4, Homebrew arm64 | PS4 cross compilation and linking |
| Go | 1.27.3, Homebrew arm64 | Build native create-fself |
| [.NET](https://builds.dotnet.microsoft.com/dotnet/release-metadata/8.0/releases.json) | SDK 8.0.427 osx-arm64 | Build/run PkgTool.Core |
| [UxPlay](https://github.com/FDH2/UxPlay) | 534e2cc0e558d6c94bcc66964f8fc13ad73a6cdc | Protocol implementation linked in 0.05, with Orbis patches |

OpenOrbis release archive SHA256:
`3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`

.NET SDK archive SHA512:
`79786b93f249d233b15f1d6338d4798bd71c8143f267c5e84de76c9eb8a1054920e56ea5478f7bddfb74b951cc58aaca11fbd780cf134350b7cb7cdcb42a8c8c`

OpenOrbis reference samples informed platform initialization. No Sony SDK,
game assets, original sample music or proprietary Sony runtime modules are
included in this PKG. The application imports system modules from the console.
The open-source static CRT/libc are linked into the executable.

Project source: GPL-3.0-or-later. OpenOrbis, its musl fork, and related toolchain
components retain their respective licenses. License notices for musl and LLVM
builtins are in `third_party/licenses`. The CRT is from the OpenOrbis musl fork:
https://github.com/OpenOrbis/musl (the release build does not pin its commit;
the exact prebuilt archive used here is identified by SHA256 above).
Packaging tools are build dependencies, not runtime binaries shipped in PKG.


## Audio receiver 0.05

- OpenSSL 3.5.4: c1eeb9406b6142148f267594197d853403d10208; static no-asm/no-dso/no-sock/no-engine/no-async. Automatic Unix entropy disabled. RAND_METHOD delegates each request to real PS4 sceRandomGetRandomNumber in chunks <=64 bytes; no emulator rand() implementation is used. Failure prevents receiver startup.
- libplist 2.8.0: fe3dc34dc3484006e12b403f7ceb06f0ad40b6f4; static C library.
- FFmpeg n7.1.2: f893221c8d89cb798b829bebe71d55e1a3f242fd; static AAC/ALAC/PCM decoders and swresample, no encoders/demux/network. Generic FreeBSD sysctl CPU probing disabled for Orbis. All API calls serialized in adapter.
- UxPlay local changes: bounded select-based NTP socket draining instead of unavailable FIONREAD; NetCtl IPv4 discovery, IPv6 discovery disabled for Orbis. Socket address lengths are adapted by linker wrappers in socket_compat.c.

Corresponding sources are in work/research/ and work/airplay-port/vendor/ within the source bundle. Generated dependency headers and linked static archives are also included. Build commands: scripts/build_audio_deps.py. For 1.00-RC1, UxPlay core and application were rebuilt successfully from a fresh extraction of the source bundle using its supplied static dependencies. A full clean bootstrap of all third-party dependency libraries has not been re-run.


## Release 1.00-RC1

- Base app/vendor: exact 0.13 source archive; audio, mirror decoding, HEN bridge,
  SIMD FFmpeg and socket wrappers retained. Native profile-file I/O is a focused
  backport from 0.14, independently tested with mock Orbis APIs.
- AvPlayer ABI declarations checked against shadPS4 Emulator Project, Copyright
  2024, GPL-2.0-or-later, `src/core/libraries/avplayer/avplayer.h`:
  https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/avplayer/avplayer.h
  System AvPlayer is loaded from the console; no Sony binary is redistributed.
  No code was copied from PS4-IPTV-Player.
- stb_image.h/stb_truetype.h from the OpenOrbis 0.5.4 SDK include directory;
  dual MIT/public-domain license is reproduced at the end of each header in
  third_party/stb/. JPEG and PNG cover decoding only.
- Noto Sans Regular from notofonts/noto-fonts, SIL Open Font License 1.1;
  assets/fonts/OFL.txt. Font bytes are embedded in generated/noto_font.h.
- Additional UxPlay changes: direct URL/legacy text requests and fractional
  Start-Position; loopback-only HLS GET classification; optional fraction callback;
  system-language fallback; bounded playlist payloads; server-info feature mask
  matches discovery; invalid playback commands return 400 instead of crashing.
  These patches are supplied in full in work/airplay-port/vendor/uxplay-lib/.


## AvPlayer HTTP contexts, 1.01-RC2

AvPlayerPostInitData / sceAvPlayerPostInit signatures are from the same
shadPS4 avplayer.h/cpp primary implementation cited above. Public structure size
132 bytes, HTTP/SSL context IDs at offsets 68/72, checked by static assertions.
The app creates and owns Net, SSL and HTTP contexts; TLS verification stays enabled.
Error 0x806A0002 was reproduced on the user's PS4 by a direct HTTP play request;
RC2's new native path has not yet been verified on hardware.


## Direct FFmpeg receiver, 1.06-RC7

FFmpeg remains pinned to n7.1.2. The direct build additionally enables libavformat,
MOV/HLS/MPEG-TS/AAC demuxers and required bitstream filters. HTTP and TLS transport
are provided through custom AVIO and console HTTP/SSL APIs; certificate checking
is not disabled. source/patches/ffmpeg-hls-custom-io.patch documents the HLS URL
recognition change used with custom AVIO. The full source bundle includes the
modified hls.c and the linked ffmpeg-video-direct static libraries/headers.
Native AvPlayer code is no longer part of the production direct receiver.

## Verified HTTPS transport (RC11)

OpenSSL libssl 3.5.4 is linked with the existing identical libcrypto 3.5.4 build,
using a custom socket BIO (no-sock build). New source/scripts/build_tls.py can
rebuild libssl from the pinned source when it is missing. No newer crypto build
or global certificate-store changes are required.

Public Mozilla roots: source/assets/cacert.pem, downloaded from
https://curl.se/ca/cacert.pem, extracted 2026-10-20. SHA256 and count are in
assets/cacert-source.json; MPL-2.0 text in assets/LICENSE.cacert.txt.
Only these public roots are embedded; no user/OS private certificate material.
