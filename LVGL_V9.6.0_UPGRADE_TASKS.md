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

- [ ] Enumerate `lvgl/src/**/*.{c,S}` at v9.6.0 and diff against the
      current `[Sources]` list.
- [ ] Fix the confirmed hard breakers:
  - [ ] `src/draw/lv_image_decoder.c` -> `src/image/lv_image_decoder.c`
  - [ ] `src/libs/fsdrv/*` -> `src/fs/*`
  - [ ] `src/libs/freetype/*` -> `src/font/freetype/*`
  - [ ] `src/libs/{bmp,gif,qrcode,lodepng,libpng,libwebp,libjpeg_turbo,tjpgd,bin_decoder,svg,rle}/*`
        -> `src/image/*` (codec files moved under the new `image` module)
  - [ ] `src/drivers/display/{st7735,st7789,st7796,ili9341,nv3007,lcd}/*`
        -> `src/drivers/display/mipi/*`
  - [ ] `src/drivers/wayland/lv_wl_*.c` -> `src/drivers/wayland/lv_wayland_*.c`
  - [ ] `src/draw/sw/lv_draw_sw_utils.c` -> confirm new location
        (moved out of `sw/`, verify exact path in v9.6.0 tree)
- [ ] For every currently-disabled subsystem (hardware draw units, other
      display drivers, wayland, etc.) keep the *file list* in sync with
      the real tree even though `LV_USE_* 0` means they compile to
      (near-)empty objects -- a missing path is still a build error.
- [ ] Re-check new v9.6.0 directories that may need entries if we ever
      enable them (no action needed now, just note for awareness):
      `src/draw/sifli/`, `src/draw/sw/blend/sve2/`, `src/debugging/profiler/`.
- [ ] Keep the 3 compiled UEFI driver files unchanged:
      `lv_uefi_context.c`, `lv_uefi_display.c`, `lv_uefi_private.c`.
- [ ] Keep the 3 UEFI indev files excluded (we use custom
      `lv_port_indev.c` instead): `lv_uefi_indev_keyboard.c`,
      `lv_uefi_indev_pointer.c`, `lv_uefi_indev_touch.c`.

## C. Config -- `Library/LvglLib/lv_conf.h`

This is a project-local copy (not inside the submodule), so it does not
update automatically and must be hand-diffed against the new template.

- [ ] Diff our `lv_conf.h` against v9.6.0's `lv_conf_template.h`; port in
      new macros while preserving our settings:
      `LV_USE_UEFI 1`, `LV_USE_UEFI_INCLUDE "lv_uefi_edk2.h"`,
      `LV_USE_STDLIB_MALLOC/STRING LV_STDLIB_CLIB`,
      `LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN`, `LV_USE_OS LV_OS_NONE`,
      software-renderer-only draw units, curated Montserrat font subset,
      32bpp pixel format support (RGB888/XRGB8888/ARGB8888).
- [ ] Set `LV_USE_PRIVATE_API 1` -- **required**. `lv_port_indev.c`
      includes `lvgl/src/indev/lv_indev_private.h` and writes
      `Indev->keypad.last_state` / `last_key` directly
      (`lv_uefi_keypad_drain()`). Without this the private struct layout
      is not guaranteed exposed in v9.6.0's stricter private/public split.
- [ ] Decide `LV_CHECK_ARG` -- recommend leaving it `0`-equivalent
      (disabled, or `LV_CHECK_ARG_LOG_MODE_NONE` +
      `LV_CHECK_ARG_ASSERT_ON_FAIL 0`) for a firmware target. Default is
      ~2,900 runtime argument-validation checks added across the public
      API -- code-size and cycle cost we likely don't want in DXE phase.
      Document whichever choice is made.
- [ ] Migrate `LV_COLOR_DEPTH 32` -> `LV_COLOR_FORMAT_DEFAULT
      LV_COLOR_FORMAT_ARGB8888` (keep 32bpp; UEFI GOP `Blt()` expects
      BGRA/32-bit, unchanged).
- [ ] Review newly-default-on object style cache and display-tiling
      options; confirm they don't change rendering behavior we depend on
      in `LvglFormRenderer.c` / `LvglAptioChrome.c`.

## D. API deprecation cleanup (full scope)

- [ ] `LvglDisplayEngineDxe/LvglFormRenderer.c`: `lv_btn_create` ->
      `lv_button_create`.
- [ ] `Library/LvglLib/MouseCursorIcon.c`: `lv_img_dsc_t` ->
      `lv_image_dsc_t`, `LV_IMG_DECLARE` -> `LV_IMAGE_DECLARE`.
- [ ] `Library/LvglLib/lv_port_indev.c`: same `lv_img_dsc_t` /
      `LV_IMG_DECLARE` usages (cursor icon extern/declare at lines ~58,
      ~419) -> new names.
- [ ] `LvglDisplayEngineDxe/AptioWallpaper.c`: already uses
      `lv_image_dsc_t` -- just verify the hand-rolled
      `.header.{magic,cf,flags,w,h,stride,reserved_2}` initializer still
      matches `lv_image_header_t`'s field layout/order in v9.6.0.
- [ ] Audit any other flag-API deprecation warnings surfaced during the
      build (`lv_obj_add_flag`/`has_flag` used for semantic
      checked/disabled/pressed state) and migrate to typed helpers
      (`lv_obj_set_checked`, `lv_obj_set_disabled`, `lv_obj_is_pressed`)
      where the deprecation applies -- do not blanket-replace
      `lv_obj_add_flag`/`clear_flag` calls that are legitimate generic
      object flags (e.g. `LV_OBJ_FLAG_HIDDEN`, scrollable, clickable).
- [ ] Confirm the private keypad struct field names
      (`Indev->keypad.last_state`, `Indev->keypad.last_key`) are
      unchanged in v9.6.0's `lv_indev_private.h`; update
      `lv_uefi_keypad_drain()` if renamed.
- [ ] Grep the whole tree for `lv_uefi_std_wrapper.h` (removed upstream
      in v9.6.0) and confirm nothing includes it.

## E. Build + verify

- [ ] `source edksetup.sh && export PACKAGES_PATH=$HOME/workspace/edk2:$HOME/workspace/edk2/LvglPkg`
- [ ] `build -a X64 -t GCC -b DEBUG -p OvmfPkg/OvmfPkgX64.dsc` -- clean, no warnings
      treated specially (no `-Werror` in this build, but check for new
      *errors*, not just the expected deprecation `#warning` noise).
- [ ] `build -a X64 -t GCC -b DEBUG -p LvglPkg/LvglPkg.dsc` -- clean.
- [ ] `bash ~/workspace/edk2/qemu_lvgl.sh` and manually exercise:
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
