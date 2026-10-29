# Third-Party Software Notices and Licenses

This project, VLC-Whisper, incorporates or links against third-party open-source components, models, and libraries under the terms described below.

---

## 1. whisper.cpp & ggml
- **Project**: whisper.cpp / ggml
- **Authors**: Georgi Gerganov and the ggml authors
- **License**: MIT License
- **URL**: https://github.com/ggerganov/whisper.cpp

```text
MIT License

Copyright (c) 2023-2026 The ggml authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. OpenAI Whisper Model Weights
- **Project**: OpenAI Whisper Models (tiny.en, base.en, etc.)
- **Authors**: OpenAI
- **License**: MIT License
- **URL**: https://github.com/openai/whisper

```text
MIT License

Copyright (c) 2022 OpenAI

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 3. Silero VAD Model Weights
- **Project**: Silero VAD
- **Authors**: Silero Team (snakers4)
- **License**: MIT License
- **URL**: https://github.com/snakers4/silero-vad

```text
MIT License

Copyright (c) 2020-2026 Silero Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 4. VLC Media Player Plugin API & libvlccore
- **Project**: VLC media player
- **Authors**: VideoLAN and VLC authors
- **License**: GNU Lesser General Public License v2.1 or later (LGPL v2.1+)
- **URL**: https://www.videolan.org/vlc/ | https://code.videolan.org/videolan/vlc

VLC-Whisper is an out-of-tree plugin module that interfaces with VLC Media Player via its public C plugin API and dynamically links against `libvlccore.dll` / `libvlccore.so` in accordance with LGPL v2.1 Section 6. VLC Media Player is not bundled with this plugin. Source code for VLC Media Player is available from VideoLAN at https://code.videolan.org/videolan/vlc.

---

## 5. FFmpeg (POSIX / Linux Builds)
- **Project**: FFmpeg (libavcodec, libavformat, libavutil, libswresample)
- **Authors**: FFmpeg Developers
- **License**: GNU Lesser General Public License v2.1 or later (LGPL v2.1+)
- **URL**: https://ffmpeg.org

On POSIX/Linux systems where FFmpeg support is enabled, `vlc-whisper-worker` dynamically links against system-provided shared libraries (`libavformat.so`, `libavcodec.so`, `libswresample.so`, `libavutil.so`) built under LGPL v2.1+ configuration. Source code for FFmpeg is available from https://ffmpeg.org. On Windows systems, native Windows Media Foundation is utilized instead of FFmpeg. The macOS release zip (`scripts/macos-package-release.sh`) bundles unmodified FFmpeg shared libraries built with the default LGPL v2.1+ configuration (no `--enable-gpl` or `--enable-nonfree`) under `VLC.app/Contents/MacOS/lib/vlc-whisper`, together with the LGPL license text and the exact source tarball URL in `share/doc/vlc-whisper`. The worker loads them dynamically, so they can be replaced with any ABI-compatible build.

---

## 6. Vulkan Headers & Khronos Loader
- **Project**: Vulkan-Headers, Vulkan-Loader, Google Shaderc / glslc
- **Authors**: Khronos Group Inc., Google LLC
- **License**: Apache License 2.0
- **URL**: https://github.com/KhronosGroup/Vulkan-Loader

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at:
http://www.apache.org/licenses/LICENSE-2.0

---

## 7. GCC / MinGW-w64 Runtime Libraries
- **Project**: MinGW-w64 / GCC Runtime (libgcc, libstdc++, libwinpthread)
- **Authors**: Free Software Foundation, MinGW-w64 Project
- **License**: GNU GPL v3 with GCC Runtime Library Exception v3.1 / MIT
- **URL**: https://www.gnu.org/licenses/gcc-exception-3.1.en.html

Binaries compiled with MinGW-w64 incorporate GCC runtime support code covered by the GCC Runtime Library Exception v3.1, which grants permission to link the runtime library with independent modules without subjecting the resulting binary to the GNU General Public License.
