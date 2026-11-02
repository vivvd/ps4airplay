English/Russian interface and safer session shutdown for PS4 AirPlay.

- English is now the default in the app and diagnostics page; Russian remains available. Change language from Options → Language or the diagnostics page. The selection is shared for the current app session.
- Options opens Settings. Choose Disconnect AirPlay and exit to close the receiver and return to the PS4 menu.
- Shutdown stops protocol callbacks before freeing video sessions, cancels/joins the direct player before destroying its core, and joins the HTTP thread even if it has already stopped itself.
- Discovery records use a 15-second lease, renewed every 5 seconds, plus zero-TTL goodbye messages on clean exit. iOS can still briefly retain a cached device or selected route after forced termination.
- Includes the accumulated Safari connection, encrypted control, verified HTTPS, FFmpeg video, and playback-info fixes since RC1. Direct video no longer uses Sony AvPlayer/Audiodec.
- GitHub documentation is now in English and describes the actual supported paths and limitations.

Validation: real core/FFmpeg integration under ASan/UBSan, active-playback shutdown with control socket EOF and closed listener, discovery TTL/renewal/goodbye, HTTP/HTTPS MP4/HLS/fMP4, pause/seek/stop, encrypted setup/pairing/FCUP, artwork/UI, 13 diagnostics tests, browser language switching, native PKG validation, and a clean source-archive rebuild.

This remains a pre-release: the RC13 Options/exit changes have not yet been tested on physical PS4 hardware. Earlier builds have played music, mirrored screens, and received Safari video on PS4 13.52. Some long videos still show 00:00 on iPhone; this is not claimed fixed. DRM, HEVC, AV1, and current YouTube compatibility are unsupported. Software decoding can stutter on demanding sources.

Install PS4-AirPlay-1.12-RC13.pkg over the previous version with HEN enabled. Title ID remains ARPL00001. Assets include the complete source/dependency archive and SHA256 checksums. Personal console reports are not included.
