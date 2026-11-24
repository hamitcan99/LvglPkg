# CLAUDE.md -- LVGL v9.5.0 -> v9.6.0 Upgrade (LvglPkg)

This is a scoped CLAUDE.md for the LVGL version-bump effort only. It
supplements, and does not replace, the main project doc at
`~/workspace/edk2/CLAUDE.md` (the Display Engine Replacement
architecture doc). Read that one first for overall project context;
read this one when working on the LVGL upgrade specifically.

Companion file: `LVGL_V9.6.0_UPGRADE_TASKS.md` in this same directory --
the actionable checklist. This file is the "why" behind each item.

## Version delta

| | Version | Commit / Tag |
|---|---|---|
| Before | v9.5.0 | `85aa60d18b3d5e5588d7b247abf90198f07c8a63` |
| After | v9.6.0 | tag `v9.6.0` (released 2026-09-16) |

LVGL is vendored as a git submodule at `Library/LvglLib/lvgl`
(`.gitmodules`: `url = https://github.com/lvgl/lvgl.git`). The upgrade
is a submodule pointer move plus reconciling everything downstream of
it that assumes v9.5.0's file layout, config surface, and API names.

## Why this matters more than a typical dependency bump

Two things make this port unusually sensitive to an LVGL version bump:

1. `LvglLib.inf` lists every compiled upstream `.c` file **by explicit
   path**, not a glob (`[Sources]`, ~450 entries). Any upstream rename
   or move is a hard build failure ("source file not found"), not a
   silent behavior change.
2. `lv_port_indev.c` reaches past the public API into a private LVGL
   header (`lvgl/src/indev/lv_indev_private.h`) to directly manipulate
   `lv_indev_t`'s internal `keypad` sub-struct. This is the single most
   fragile point in the whole port and the reason `LV_USE_PRIVATE_API`
   must be turned on for v9.6.0.

## The five things v9.6.0 actually changes (verified against upstream)

### 1. Header reorganization -- low risk for us
Public API moved to `include/lvgl/`. Old `src/*.h` public headers are
now `#warning` redirect shims (e.g. `src/drivers/uefi/lv_uefi_indev.h`
just warns and forwards). `src/lvgl.h` itself forwards cleanly to
`include/lvgl/lvgl.h` with no warning.
**Why this is low risk**: our build has **no `-Werror`** anywhere
(`LvglLib.inf`, `OvmfPkgX64.dsc`, `LvglPkg.dsc` all checked -- none set
it), so these deprecation warnings are compiler noise, not failures.

### 2. Private API gating -- must act
Internal types now require `LV_USE_PRIVATE_API 1` or including
`lvgl_private.h`. `lv_indev_private.h` still exists at the same path in
v9.6.0, but the private/public split is stricter. **Action**: set
`LV_USE_PRIVATE_API 1` in `lv_conf.h`. Also re-verify that
`Indev->keypad.last_state` / `Indev->keypad.last_key` (written in
`lv_uefi_keypad_drain()`) are still the correct field names -- this is
the kind of internal-struct change that wouldn't show up as a compile
error if the struct still has *a* `last_state` field but with different
semantics.

### 3. Source-tree reorganization -- must act (mechanical, large)
Confirmed path changes that intersect files we actually list in
`[Sources]`:
- `src/draw/lv_image_decoder.c` -> `src/image/lv_image_decoder.c`
- `src/libs/fsdrv/*` -> `src/fs/*`
- `src/libs/freetype/*` -> `src/font/freetype/*`
- `src/libs/*` (bmp, gif, qrcode, png, webp, jpeg, svg, etc.) -> `src/image/*`
- `src/drivers/display/{st7735,st7789,st7796,ili9341,nv3007,lcd}/*` ->
  `src/drivers/display/mipi/*`
- `src/drivers/wayland/lv_wl_*.c` -> `lv_wayland_*.c`

Even though most of these subsystems are disabled for us
(`LV_USE_* 0`), the file must still exist at the listed path to compile
-- EDK2's build system resolves `[Sources]` paths at build time
regardless of whether the resulting object does anything.

### 4. `lv_conf.h` new/changed options -- must act
- `LV_USE_PRIVATE_API` (see #2).
- `LV_CHECK_ARG` (+ `LV_CHECK_ARG_LOG_MODE`, `LV_CHECK_ARG_ASSERT_ON_FAIL`)
  -- new runtime argument validation, ~2,900 checks, **enabled by
  default** upstream. Decision for this project: turn it off (or
  minimal/no-assert) -- DXE-phase firmware code is not the place to pay
  for broad runtime arg validation by default.
- `LV_COLOR_DEPTH` deprecated in favor of `LV_COLOR_FORMAT_DEFAULT`.
  We keep 32bpp; migrate to
  `LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_ARGB8888` (GOP `Blt()` still
  expects BGRA/32-bit, this is a rename not a behavior change).
- Object style cache now default-on; display tiling now configurable.
  Lower risk, but worth a sanity pass against `LvglFormRenderer.c` /
  `LvglAptioChrome.c` styling.

### 5. API deprecations -- full cleanup scope (this project's choice)
We chose to migrate off deprecated names now rather than rely on the
still-compiling legacy aliases:
- `lv_btn_create` -> `lv_button_create` (`LvglFormRenderer.c`)
- `lv_img_dsc_t` / `LV_IMG_DECLARE` -> `lv_image_dsc_t` /
  `LV_IMAGE_DECLARE` (`MouseCursorIcon.c`, `lv_port_indev.c`;
  `AptioWallpaper.c` already uses the new name)
- generic flag helpers -> typed state APIs (`lv_obj_set_checked`,
  `lv_obj_set_disabled`, `lv_obj_is_pressed`) where a flag call was
  standing in for semantic state, not a structural `LV_OBJ_FLAG_*`
  (hidden/clickable/scrollable flags are untouched)

### UEFI driver itself -- no action needed on the public API we call
`lv_uefi_init`, `lv_uefi_display_create`, `lv_uefi_display_get_any` are
unchanged in v9.6.0. Two things to double check:
- `lv_uefi_std_wrapper.h` was **removed** upstream -- grep to confirm
  nothing in our wrapper headers (`limits.h`, `stdint.h`, etc. in
  `Library/LvglLib/`) or port code includes it.
- Upstream added a `Kconfig` file to the UEFI driver dir (irrelevant to
  our EDK2-native build, no action).
- Upstream behavior tweak: suspend scan code now treated as Enter --
  worth noting if any odd key-handling surfaces during manual testing,
  but not expected to require a code change on our side.

## Files this upgrade touches, and why

| File | Why it changes |
|---|---|
| `Library/LvglLib/lvgl` (submodule) | Pointer move v9.5.0 -> v9.6.0 |
| `Library/LvglLib/LvglLib.inf` | `[Sources]` path reconciliation (#3) |
| `Library/LvglLib/lv_conf.h` | New config macros, private-API gate, color-format rename (#2, #4) |
| `Library/LvglLib/lv_port_indev.c` | Private keypad struct re-check (#2); `lv_img_dsc_t` rename (#5) |
| `Library/LvglLib/MouseCursorIcon.c` | `lv_img_dsc_t` / `LV_IMG_DECLARE` rename (#5) |
| `LvglDisplayEngineDxe/LvglFormRenderer.c` | `lv_btn_create` rename (#5) |
| `LvglDisplayEngineDxe/AptioWallpaper.c` | Verify `lv_image_header_t` layout unchanged (#5, already on new type name) |
| `~/workspace/edk2/CLAUDE.md` | Version references bumped once upgrade lands (not a functional change) |

## Decisions made for this upgrade

- **Full deprecation cleanup**, not minimal-build-green. Rationale:
  avoid accumulating a second round of churn on the next LVGL bump;
  the deprecated aliases still compile today but are not guaranteed to
  survive indefinitely.
- **`LV_USE_PRIVATE_API 1`**: required by the existing keypad-drain
  workaround; no alternative without rewriting that workaround (which
  is itself a documented upstream contribution candidate -- see the
  "Upstream LVGL UEFI Driver -- Contribution Topics" section in the
  root CLAUDE.md, item #2).
- **`LV_CHECK_ARG` disabled**: firmware-appropriate choice; revisit if
  v9.6.0-era bugs are suspected to be argument-validation-shaped (then
  temporarily enable for a debug build only).
- **32bpp retained** via `LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_ARGB8888`:
  no behavior change, just following the new macro name since
  `LV_COLOR_DEPTH` is deprecated.

## Verification

1. Both builds compile clean:
   ```bash
   source edksetup.sh
   export PACKAGES_PATH=$HOME/workspace/edk2:$HOME/workspace/edk2/LvglPkg
   build -a X64 -t GCC -b DEBUG -p OvmfPkg/OvmfPkgX64.dsc
   build -a X64 -t GCC -b DEBUG -p LvglPkg/LvglPkg.dsc
   ```
   No "source file not found" errors, no undefined `lv_*` symbols at
   link time, no new *errors* (the expected deprecated-header
   `#warning` lines are fine since there's no `-Werror`).
2. `bash ~/workspace/edk2/qemu_lvgl.sh` -> enter Setup (F2/DEL) ->
   full manual pass per `LVGL_V9.6.0_UPGRADE_TASKS.md` section E.
3. `git diff --submodule` on the `LvglPkg` superproject shows only the
   intended `v9.5.0 -> v9.6.0` move -- no unrelated submodule drift.

## Rollback

```bash
cd Library/LvglLib/lvgl
git checkout 85aa60d18b3d5e5588d7b247abf90198f07c8a63
cd ../../..
git checkout -- Library/LvglLib/LvglLib.inf Library/LvglLib/lv_conf.h \
  Library/LvglLib/lv_port_indev.c Library/LvglLib/MouseCursorIcon.c \
  LvglDisplayEngineDxe/LvglFormRenderer.c LvglDisplayEngineDxe/AptioWallpaper.c
```

## Status

Not started. This document and the task list were created to scope the
work before beginning. Update the task list checkboxes as work
proceeds; mark this file's Status line `Done` once section E's manual
QEMU pass is fully green.
