# Third-party notices

White Sound is distributed under the MIT License (see `LICENSE`). It bundles the
following third-party components, each under its own license. The copyright and
permission notices of those licenses apply to the corresponding component.

| Component | Version | License | Bundled at |
|-----------|---------|---------|------------|
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | Public domain **or** MIT No Attribution (MIT-0) | `third_party/miniaudio/miniaudio.h` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9b | MIT | `third_party/imgui/` (`LICENSE.txt`) |
| [GLFW](https://www.glfw.org/) | 3.4.0 | zlib/libpng | `third_party/glfw/` (`LICENSE.md`) |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.12.0 | MIT | `third_party/json/json.hpp` |

## Notes

- **miniaudio** is dual-licensed; the license statement is at the end of
  `third_party/miniaudio/miniaudio.h`. This project relies on the MIT-0 option.
- **nlohmann/json** is a single header; its MIT license text is included in the
  header itself (`third_party/json/json.hpp`).
- **GLFW** is used only for the portable flyout window (GLFW + OpenGL 3
  backend). Its full license is in `third_party/glfw/LICENSE.md`.
- **Dear ImGui**'s full license is in `third_party/imgui/LICENSE.txt`.

When redistributing the application binary, keep this file together with the
full license texts listed above.
