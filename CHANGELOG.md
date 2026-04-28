# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased] — obelisk-complex fork

Base upstream: [1ay1/neowall](https://github.com/1ay1/neowall) @ 9c2edb8

### Added

- **Multi-monitor X11**: virtual-screen bounding box now spans all RandR monitors
  (XRandR 1.5 `XRRGetMonitors` preferred; CRTC fallback for older drivers).
- **EWMH WM detection**: managed wallpaper window on Mutter/Muffin/KWin/Xfwm4/
  Compiz/Marco/Openbox/Fluxbox/Awesome; override-redirect fallback for tiling WMs
  (i3, bspwm, etc.). Override via `NEOWALL_X11_OVERRIDE_REDIRECT=0/1`.
- **`interactive_mouse` config key** (default `true`): disables cursor reactivity in
  shader `iMouse` when set to `false`.
- **`xdg_output` logical_size stored** for fractional-scaling correctness on Wayland.
- **`wayland_init_registry` made idempotent**: safe to call multiple times.
- **Occlusion correctness**: tracked outputs stored as array per toplevel.
- **`first_paint_done` gate**: layer-shell surface is never unmapped before first paint.
- **PID-recycling check**: daemon liveness verified via `/proc/<pid>/stat` starttime;
  stale PID files from a recycled PID are cleaned up automatically.
- **Cycle skip-on-compile-fail**: `neowall next` advances past shaders that fail to
  compile instead of silently staying on the previous shader.

### Changed

- **`pause_on_fullscreen` default changed to `false`**: live wallpaper keeps rendering
  by default; set to `true` to restore the old save-GPU behaviour.
- **KDE Plasma layer-shell**: surface layer changed from `BOTTOM` to `BACKGROUND` to
  fix wallpaper placement on KWin 6 / NVIDIA.
- **`iMouse` scaled by `shader_speed`** around screen centre: slow-speed shaders get
  proportionally gentler camera movement.
- **`atomic needs_redraw`**, surface-dead skip, and `EGL_BAD_SURFACE` handling for
  robustness during output hot-plug.

### Fixed

- **Heap overflow in `wrap_pass_source`**: buffer was sized from the *input* length
  before Shadertoy compatibility expansion; now sized from the expanded output.
- **ASan-clean `snprintf`**: all `strncpy(dst, src, sizeof-1)` patterns replaced with
  `snprintf(dst, sizeof(dst), "%s", src)` to silence `-Wstringop-truncation`.

### Performance

- **Constant uniforms cached once** (`iChannelResolution`, `iTimeDelta`, `iFrameRate`,
  `iSampleRate`): uploaded at shader compile time, not every frame.
- **`iDate` cached at 1 Hz**: `localtime` syscall skipped when the wall-clock second
  has not changed.
- **FBO binding cached**: `glGetIntegerv(GL_FRAMEBUFFER_BINDING)` only re-queried on
  resize (dirty flag), not every frame.
- **FPS-watermark VBO allocated once** at program creation; `glGenBuffers` /
  `glDeleteBuffers` no longer called per frame.
- **Alpha uniform location cached** alongside `tex_sampler`; per-frame
  `glGetUniformLocation` for `alpha` removed.
- **`glGetError` moved behind `#ifndef NDEBUG`**: GPU sync removed from release builds.
