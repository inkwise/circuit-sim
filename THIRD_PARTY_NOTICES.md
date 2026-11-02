# Third-party notices

CircuitLab bundles or links the following third-party components.

## SDL3 3.4.18

- Upstream: https://github.com/libsdl-org/SDL
- License: zlib
- Source copy: `third_party/SDL3/`, license text: `third_party/SDL3/LICENSE.txt`.
- Built as `libSDL3.so` by the Android CMake build.

## Dear ImGui 1.92.8 (docking branch tag)

- Upstream: https://github.com/ocornut/imgui
- License: MIT
- Source copy: `third_party/imgui/`, license text: `third_party/imgui/LICENSE.txt`.
- Backends used: `imgui_impl_sdl3`, `imgui_impl_sdlrenderer3`.

## ngspice 47

- Upstream: https://ngspice.sourceforge.io/
- Release source: https://sourceforge.net/projects/ngspice/files/ng-spice-rework/47/
- License: Modified BSD for most files, with additional licenses for some
  subdirectories (see `third_party/ngspice-47/COPYING`). The sharedspice API
  headers are BSD-licensed.
- Built from source with the Android NDK as `libngspice.so`
  (`scripts/build-ngspice-android.sh`). The library is packaged in the APK.
- The build recipe is based on public Android/ngspice cross-compilation
  knowledge and the SpiceFlow project's ngspice-47 notes:
  https://github.com/hendr15k/spiceflow

## Noto Sans SC UI subset

- The bundled `CircuitLabSansSC.ttf` is a renamed subset of Noto Sans SC
  (SIL Open Font License 1.1), derived from the OFL-licensed
  `MaterializrSansSC-UI.ttf` used by the local Materializr project.
- License texts:
  - `android/app/src/main/assets/fonts/OFL-NotoSansSC.txt`
  - `android/app/src/main/assets/fonts/OFL-1.1.txt`

## Gradle wrapper

- Gradle 8.7 wrapper files are included to make the Android build
  reproducible; Gradle is licensed under the Apache License 2.0.
