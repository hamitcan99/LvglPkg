# LVGL v9.5.0 -> v9.6.0 Upgrade Task List

Tracks the work needed to move the vendored LVGL submodule
(`LvglPkg/Library/LvglLib/lvgl`) from the currently pinned **v9.5.0**
(commit `85aa60d1`) to **v9.6.0** (tag `v9.6.0`, released 2026-09-16).

Background and rationale for each item: see `CLAUDE_LVGL_V9.6.0_UPGRADE.md`
in this same directory.

Scope: full deprecation cleanup (migrate off deprecated LVGL APIs now,
not just lean on the legacy aliases that still compile).

---

## A. Submodule bump

- [x] `cd Library/LvglLib/lvgl && git fetch --tags && git checkout v9.6.0`
      (checked out commit `80ca777e37a2b176770726a02e07a6fb79ef0b39`, tag `v9.6.0`)
- [x] Stage the submodule pointer move in the `LvglPkg` superproject commit.
- [x] Confirm `Library/LvglLib/lvgl/lv_version.h` reads
      `MAJOR=9 MINOR=6 PATCH=0`.
      (confirmed via `include/lvgl/lv_version.h` -- the top-level `lv_version.h` is now
      a deprecated `#warning` shim forwarding to the new path, per the header reorg)

## B. Build-file reconciliation -- `Library/LvglLib/LvglLib.inf`

The `[Sources]` section lists upstream LVGL `.c` files **explicitly, one
by one** (not a glob). v9.6.0 did a large source-tree reorg; every listed
path must exist or the build fails with "source file not found."

- [x] Enumerate `lvgl/src/**/*.{c,S}` at v9.6.0 and diff against the
      current `[Sources]` list. Result: 461 listed vs 484 actual; 52
      stale paths removed, 72 new/moved real files added (+ the 3
      UEFI indev files, correctly left out) = 481 final entries.
- [x] Fixed all 52 confirmed stale paths via full regeneration of the
      `[Sources]` block (`comm -23`/`comm -13` diff against the on-disk
      v9.6.0 tree), covering every group below plus a few not
      originally anticipated (`src/misc/lv_log.c` -> `src/logging/`,
      `src/misc/lv_fs.c`/`lv_bidi_common.c` moves, `src/libs/barcode`
      /`gstreamer`/`ffmpeg` -> `src/widgets/*`):
  - [x] `src/draw/lv_image_decoder.c` -> `src/image/lv_image_decoder.c`
  - [x] `src/libs/fsdrv/*` -> `src/fs/*`
  - [x] `src/libs/freetype/*` -> `src/font/freetype/*`
  - [x] `src/libs/{bmp,gif,qrcode,lodepng,libpng,libwebp,libjpeg_turbo,tjpgd,bin_decoder,svg,rle}/*`
        -> `src/image/*` (codec files moved under the new `image` module)
  - [x] `src/drivers/display/{st7735,st7789,st7796,ili9341,nv3007,lcd}/*`
        -> `src/drivers/display/mipi/*`
  - [x] `src/drivers/wayland/lv_wl_*.c` -> `src/drivers/wayland/lv_wayland_*.c`
  - [x] `src/draw/sw/lv_draw_sw_utils.c` -> `src/draw/lv_draw_utils.c`
        (moved out of `sw/`, confirmed)
- [x] Kept every currently-disabled subsystem's file list in sync with
      the real v9.6.0 tree (hardware draw units, other display drivers,
      wayland, etc.) -- `LV_USE_* 0` means they compile to (near-)empty
      objects, but the path must still exist.
- [x] New v9.6.0-only dirs (`src/draw/sifli/`, `src/draw/sw/blend/sve2/`,
      `src/debugging/profiler/`, `src/drivers/opengles/lv_opengles_pbuffer.c`,
      `src/libs/gltf/gltf_data/`) included in the regenerated list since
      the reconciliation rule is "list all of `src/`", matching the
      maintainer's existing philosophy -- no special-casing needed.
- [x] Kept the 3 compiled UEFI driver files unchanged:
      `lv_uefi_context.c`, `lv_uefi_display.c`, `lv_uefi_private.c`.
- [x] Kept the 3 UEFI indev files excluded (we use custom
      `lv_port_indev.c` instead): `lv_uefi_indev_keyboard.c`,
      `lv_uefi_indev_pointer.c`, `lv_uefi_indev_touch.c`.

## C. Config -- `Library/LvglLib/lv_conf.h`

This is a project-local copy (not inside the submodule), so it does not
update automatically and must be hand-diffed against the new template.

- [x] Regenerated `lv_conf.h` from v9.6.0's `lv_conf_template.h` (guarantees
      every new mandatory macro exists) and re-applied our exact
      customizations. Method: diffed the *old* v9.5.0 template (pulled from
      the submodule's prior commit `85aa60d1`) against our current
      `lv_conf.h` to get the precise, minimal customization set (11 settings,
      confirmed nothing else was hand-edited), then applied the equivalent
      28 targeted replacements onto the v9.6.0 template. Preserved as-is:
      `LV_USE_UEFI 1`, `LV_USE_UEFI_INCLUDE "lv_uefi_edk2.h"`,
      `LV_USE_STDLIB_MALLOC/STRING LV_STDLIB_CLIB`, `LV_DEF_REFR_PERIOD 10`,
      software-renderer-only draw-unit support flags (RGB888/XRGB8888/ARGB8888
      on, rest off), curated Montserrat font subset (14/16/18/20/22/24/30/32/40),
      `LV_USE_DEMO_KEYPAD_AND_ENCODER 1`, and the pre-existing
      `LV_USE_PERF_MONITOR 1` / `LV_USE_MEM_MONITOR 1` (carried over
      unchanged -- not newly introduced by this upgrade).
- [x] Set `LV_USE_PRIVATE_API 1` -- **required**. `lv_port_indev.c`
      includes `lvgl/src/indev/lv_indev_private.h` and writes
      `Indev->keypad.last_state` / `last_key` directly
      (`lv_uefi_keypad_drain()`). Confirmed this was *already* `1` in our
      v9.5.0 config (not a new setting), and the private header + the
      `keypad.last_state`/`last_key` field names are unchanged in v9.6.0.
- [x] Disabled the new `LV_USE_CHECK_ARG` (actual macro name; not
      `LV_CHECK_ARG` as originally guessed) -- set to `0`. Default upstream
      is `1` with `LV_CHECK_ARG_LOG_MODE_NONE` + `LV_CHECK_ARG_ASSERT_ON_FAIL 0`
      (so even the default is low-overhead: no logging, no asserting), but
      disabling entirely removes the ~2,900 runtime argument-validation
      branches altogether -- the leaner choice for a firmware target.
- [x] Migrated `LV_COLOR_DEPTH 32` -> `LV_COLOR_FORMAT_DEFAULT
      LV_COLOR_FORMAT_XRGB8888` -- **correction from the original plan**,
      which had guessed `ARGB8888`. The v9.5.0 template's own comment
      documents `LV_COLOR_DEPTH 32` as meaning `XRGB8888` (no alpha), so
      `XRGB8888` is the behavior-preserving equivalent, not `ARGB8888`.
      Verified v9.6.0's `lv_conf_internal.h` auto-derives
      `LV_COLOR_DEPTH` from `LV_COLOR_FORMAT_DEFAULT` via
      `LV_COLOR_DEPTH_OF_LV_COLOR_FORMAT_XRGB8888 = 32`, satisfying
      `lv_uefi_display.c`'s `#error [lv_uefi] Unsupported LV_COLOR_DEPTH`
      guard (`#if LV_COLOR_DEPTH != 32`), which is still present verbatim
      in v9.6.0 -- UEFI GOP `Blt()` BGRA/32-bit expectation unchanged.
- [ ] Review newly-default-on object style cache and display-tiling
      options; confirm they don't change rendering behavior we depend on
      in `LvglFormRenderer.c` / `LvglAptioChrome.c`. (Deferred to the QEMU
      manual-test pass in Section E -- no config action taken, template
      defaults used.)

## D. API deprecation cleanup (full scope)

- [x] `LvglDisplayEngineDxe/LvglFormRenderer.c`: `lv_btn_create` ->
      `lv_button_create` (7 call sites: lines 930, 948, 961, 3131, 3155,
      3286, 3816). Confirmed `lv_btn_create` is a `#define` v8-compat
      alias for `lv_button_create` with an identical signature
      (`include/lvgl/api_map/lv_api_map_v8.h`) -- direct rename, no
      behavior change.
- [x] `Library/LvglLib/MouseCursorIcon.c`: `lv_img_dsc_t` ->
      `lv_image_dsc_t` (line 36).
- [x] `Library/LvglLib/lv_port_indev.c`: same alias at the `extern`
      declaration (line 58) -> `lv_image_dsc_t`; `LV_IMG_DECLARE` (line
      419) -> `LV_IMAGE_DECLARE`. Confirmed both are v8-compat aliases
      (`typedef lv_image_dsc_t lv_img_dsc_t;` /
      `#define LV_IMG_DECLARE(var_name) extern const lv_image_dsc_t var_name;`)
      with identical semantics.
- [x] `LvglDisplayEngineDxe/AptioWallpaper.c`: already uses
      `lv_image_dsc_t` -- verified the hand-rolled
      `.header.{magic,cf,flags,w,h,stride,reserved_2}` initializer still
      matches `lv_image_header_t`'s field layout/order in v9.6.0
      (`include/lvgl/draw/lv_image_dsc.h`) -- no change needed.
- [ ] Audit any other flag-API deprecation warnings surfaced during the
      build (`lv_obj_add_flag`/`has_flag` used for semantic
      checked/disabled/pressed state) and migrate to typed helpers
      (`lv_obj_set_checked`, `lv_obj_set_disabled`, `lv_obj_is_pressed`)
      where the deprecation applies -- do not blanket-replace
      `lv_obj_add_flag`/`clear_flag` calls that are legitimate generic
      object flags (e.g. `LV_OBJ_FLAG_HIDDEN`, scrollable, clickable).
      (Pending: resolve against real compiler output in Section E --
      no deprecation *errors* for this category were predictable by
      static grep alone.)
- [x] Confirm the private keypad struct field names
      (`Indev->keypad.last_state`, `Indev->keypad.last_key`) are
      unchanged in v9.6.0's `lv_indev_private.h`; update
      `lv_uefi_keypad_drain()` if renamed. (No rename found -- no code
      change needed.)
- [x] Grepped the whole tree for `lv_uefi_std_wrapper.h` (removed upstream
      in v9.6.0) -- no references found in custom code.

## E. Build + verify

- [x] `source edksetup.sh && export PACKAGES_PATH=$HOME/workspace/edk2:$HOME/workspace/edk2/LvglPkg`
- [x] `build -a X64 -t GCC -b DEBUG -p LvglPkg/LvglPkg.dsc` -- **clean** (exit 0,
      `- Done -`, `LvglDisplayEngine.efi` + `LvglSetupDxe.efi` produced).
- [x] `build -a X64 -t GCC -b DEBUG -p OvmfPkg/OvmfPkgX64.dsc` -- **clean** (exit 0,
      `- Done -`, all firmware volumes generated with `LvglSetupDxe`/
      `LvglDisplayEngine` linked in).
- [x] **Correction to the original plan**: `-Werror` *is* active for this build
      (`-Wall -Werror -Wno-array-bounds ...` on every `gcc` invocation, coming from
      EDK2's GCC toolchain defaults in `tools_def.txt`, not from any package-local
      `.inf`/`.dsc` -- that's why the earlier grep for `Werror` in `LvglLib.inf` /
      `OvmfPkgX64.dsc` / `LvglPkg.dsc` found nothing). This meant every deprecated-API
      usage was a **hard compile error**, not just noise, which surfaced additional
      Section D work beyond what static grep alone predicted:
  - [x] Discovered v9.6.0's `include/lvgl/lvgl.h` unconditionally includes
        `drivers/uefi/lv_uefi_gnu_efi.h` whenever `LV_USE_UEFI=1`, regardless of
        `LV_USE_UEFI_INCLUDE` -- it does `#include <efi.h>` (gnu-efi), which we don't
        have. Fixed with a minimal stub `LvglPkg/Include/efi.h` (empty include guard
        only; the `LV_UEFI_GNU_EFI_HEADERS` macro it would enable is confirmed unused
        anywhere else in the vendored tree, so the stub has zero functional effect).
  - [x] Discovered the `lv_obj_add_flag`/`lv_obj_remove_flag`/`lv_obj_clear_flag`/
        `lv_obj_has_flag` family is deprecated **wholesale** in v9.6.0 (not just for
        checked/disabled/pressed semantics as originally scoped) in favor of
        per-flag `lv_obj_set_<flag>()`/`lv_obj_is_<flag>()` functions. Migrated all 22
        call sites: `lv_port_indev.c` (4), `LvglFormRenderer.c` (6), and
        `LvglAptioChrome.c` (12). Flags involved:
        `HIDDEN` -> `lv_obj_set_hidden`, `SCROLLABLE` -> `lv_obj_set_scrollable`,
        `SCROLL_ON_FOCUS` -> `lv_obj_set_scroll_on_focus`, `SCROLL_ELASTIC` ->
        `lv_obj_set_scroll_elastic`, `SCROLL_MOMENTUM` -> `lv_obj_set_scroll_momentum`.
        Confirmed `lv_obj_add_state`/`remove_state`/`has_state` (a separate API) is
        **not** deprecated -- no change needed there.
- [ ] `bash ~/workspace/edk2/qemu_lvgl.sh` and manually exercise (user to run --
      build-only verification above confirms compile/link; this interactive pass is
      the actual functional confirmation):
  - [ ] Setup form renders (title bar, help pane, footer hotkey bar).
  - [ ] Keyboard UP/DOWN focus, ENTER edit-mode, ESC cancel/exit.
  - [ ] Dropdown: ENTER to edit, UP/DOWN cycles locally, ENTER commits,
        ESC reverts.
  - [ ] Spinbox / textarea edit and commit (string field save, no ASSERT).
  - [ ] Ordered list reorder (Up/Down buttons).
  - [ ] Mouse hover highlight on form rows.
  - [ ] Mouse wheel scrolling in a long form.
  - [ ] Bottom on-screen keyboard appears for text/password fields.
  - [ ] F9 (Load Defaults) / F10 (Save) hotkeys show confirm popup and work.
  - [ ] Save / discard / reset flow via SetupBrowserDxe.

## F. Docs

- [ ] Bump v9.5 -> v9.6.0 version references in the root
      `~/workspace/edk2/CLAUDE.md` (submodule commit, any version strings
      in the "Key Architecture Decisions" / status sections).
- [ ] Note the upgrade as complete in this package's upgrade CLAUDE.md
      (`CLAUDE_LVGL_V9.6.0_UPGRADE.md`) once E is fully green.

---

## Rollback

If the upgrade has to be reverted:

```bash
cd Library/LvglLib/lvgl
git checkout 85aa60d18b3d5e5588d7b247abf90198f07c8a63
cd ../../..
git checkout -- Library/LvglLib/LvglLib.inf Library/LvglLib/lv_conf.h \
  Library/LvglLib/lv_port_indev.c Library/LvglLib/MouseCursorIcon.c \
  LvglDisplayEngineDxe/LvglFormRenderer.c LvglDisplayEngineDxe/AptioWallpaper.c
```
