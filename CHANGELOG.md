# Changelog

All notable changes to VLC-Whisper will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Experimental macOS (Apple Silicon) build: Metal-accelerated worker, VLC.app install rules, and a
  **Subtitles > Whisper AI Captions** menu toggle.
- Self-contained macOS release zip (`scripts/macos-package-release.sh`): bundled minimal LGPL FFmpeg, macOS 13+
  M1-baseline CPU flags, install/uninstall scripts that clear quarantine and restore VLC.app's signature on removal.
- `large-v3-turbo` model catalog entry (near large-v3 accuracy at about half the size and twice the speed),
  selectable in the settings extension.

### Fixed

- Captions no longer stall on Matroska/WebM (and other millisecond-timebase) media: the source decoder now derives
  output timestamps from an exact sample count, so container PTS rounding is no longer mistaken for a discontinuity
  that cleared the look-ahead buffer before the VAD window filled. Previously only the final seconds of such files
  were captioned. Regression test: `test_source_ms_timebase_audio_accumulates`.

## [0.2.0] - Unreleased

### Added

- Official Ubuntu/Debian x64 distribution for Ubuntu 24.04 and 26.04, including version-aware installation, `.deb` packages, checksums, VLC cache hooks, Debian `t64` plugin compatibility, and automated release packaging. (#57)
- Lower-latency transcription for live and non-seekable media: inference now starts after roughly 2 seconds, grows its acoustic context progressively to 8 seconds, and then uses a 1-second rolling hop while withholding unstable right-edge hypotheses. (#38)
- Enabled-by-default **Show subtitles while paused (local files only)** setting, including paused-seek preview behavior for seekable local media. (#62)
- Explicit translation failure attribution from the worker, including provider/transport/parse/deadline/local failure classes, attempted fallback tiers, privacy-safe diagnostics, and aggregate benchmark counters. (#60)
- Local English/Romanian ASR quality regression tooling with a deterministic FLEURS subset, live/look-ahead benchmark modes, and corpus-weighted WER/CER reporting. (#43)
- Isolated Qt 6 settings frontend prototype for evaluating a future native settings/control application without changing the production VLC/plugin runtime. (#39)
- Linux Whisper sample support for arbitrary FFmpeg-readable media inputs instead of requiring a pre-converted WAV file. (#40)

### Changed

- Translation and benchmark diagnostics now use a stable last-session report, explicit seconds/milliseconds fields, locale-independent numeric formatting, and safer atomic report replacement. (#44)
- The Windows installer now keeps the VLC-running warning open with **Retry** / **Cancel** until VLC has actually exited. (#56)
- Reliability work substantially expanded failure-path, lifecycle, protocol, decoder, translation, benchmark, and teardown regression coverage, with stricter invariant-driven test conventions. (#50, #52, #54, #59)
- Project documentation was consolidated and compressed, stale plans/reports were removed, and the roadmap was refocused around quality gates and the native subtitle/settings hub. (#41, #45, #52, #58)
- The strict clang-format CI gate remains blocking but now runs after the other strict checks. (#63)
- README navigation and developer/end-user guidance were reorganized. (#42)

### Fixed

- Windows workers now reserve sufficient stack space for Whisper model startup, fixing a startup crash that could prevent both GPU and CPU workers from reaching IPC initialization. (#44)
- Final speech and captions are preserved more reliably through media end, slow inference, translation fallback, and plugin teardown instead of being dropped during shutdown. (#50, #54)
- Caption presentation now preserves cue visibility correctly, avoids clearing VLC's system OSD channel, and behaves more reliably across video-output recreation, pause/resume, seek, and media transitions. (#50, #54, #62)
- Seek/discontinuity handling, decoder pre-roll and failed-seek behavior, negative timestamps, VAD trailing silence, playback-rate transitions, session resets, and source-mode timing were hardened. (#50, #54, #59)
- IPC and protocol handling now better covers large/continued payloads, interrupted accepts, HELLO version negotiation, message/control validation, malformed UTF-8, duplicate session starts, and transport-send failures. (#50, #54, #59)
- Translation subprocess and networking paths were hardened against SIGPIPE races, blocking/nonblocking descriptor mistakes, stale-session failures, timeout/fallback loss, and recoverable translation failures. (#50, #60)
- Model download/configuration handling now better protects cancellation, lock cleanup, atomic installation, path resolution, oversized identities, and stale worker executable paths. (#50, #59)
- Benchmark status/telemetry accuracy and report validation were improved, including queue duration/state reporting, manifest/result schema checks, atomic output handling, and correct empty-reference WER/CER semantics. (#44, #54, #59)

### Security

- Authentication/token generation now fails closed when secure randomness is unavailable, and identity-bearing paths, URIs, endpoints, model/language values, and worker arguments reject overflow instead of silently truncating. (#50, #54, #59)
- Local IPC/process handling was hardened with stricter peer/credential checks, safer process spawning and descriptor inheritance, bounded logical sends, and stronger malformed-frame validation. (#50, #54, #59)

## [0.1.0] - 2026-09-01

### Added

- Initial public MVP release of VLC-Whisper.
- Real-time Whisper speech transcription directly inside VLC Media Player.
- Support for local media, network video-on-demand, and live/non-seekable streams.
- Seek-aware subtitle synchronization for local and seekable media.
- Local speech recognition powered by whisper.cpp with Silero VAD.
- Vulkan GPU acceleration with multi-core CPU fallback.
- Optional real-time subtitle translation with single-line and dual-line display modes.
- In-VLC settings for transcription language, model, compute backend, CPU threads, and translation.
- On-demand Whisper model downloading and validation.
- Windows 10/11 64-bit installer and portable distribution.
- Experimental Linux source support.

[Unreleased]: https://github.com/rzv04/vlc-whisper/compare/v0.1.0...HEAD
[0.2.0]: https://github.com/rzv04/vlc-whisper/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/rzv04/vlc-whisper/releases/tag/v0.1.0
