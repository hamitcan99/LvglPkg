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
`include/lvgl/lvgl.h` with no warning, and we never include any old
`src/*.h` path directly in custom code (only the still-valid
`lvgl/src/indev/lv_indev_private.h`), so these shims never trigger for us.

**Correction after actually building**: the original version of this doc
claimed "no `-Werror` anywhere" based on grepping `LvglLib.inf` /
`OvmfPkgX64.dsc` / `LvglPkg.dsc` -- that was **wrong**. `-Werror` *is*
active on every compile (`-Wall -Werror -Wno-array-bounds ...`), it just
comes from EDK2's GCC toolchain defaults in `tools_def.txt`, not from any
package-local file, so the grep missed it. This meant every deprecated-API
*usage* (not just the harmless header-redirect `#warning`s) was a hard
compile error -- see the new section 6 below for what this actually broke.

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
- `lv_btn_create` -> `lv_button_create` (`LvglFormRenderer.c`, 7 call sites)
- `lv_img_dsc_t` / `LV_IMG_DECLARE` -> `lv_image_dsc_t` /
  `LV_IMAGE_DECLARE` (`MouseCursorIcon.c`, `lv_port_indev.c`;
  `AptioWallpaper.c` already uses the new name)
- **Correction after actually building** (see section 6): the generic
  object-flag API (`lv_obj_add_flag`/`lv_obj_remove_flag`/
  `lv_obj_clear_flag`/`lv_obj_has_flag`/`lv_obj_has_flag_any`) is
  deprecated **wholesale** in v9.6.0, not just for
  checked/disabled/pressed semantics as originally scoped here. We do
  use `LV_OBJ_FLAG_HIDDEN`, `LV_OBJ_FLAG_SCROLLABLE`,
  `LV_OBJ_FLAG_SCROLL_ON_FOCUS`, `LV_OBJ_FLAG_SCROLL_ELASTIC`, and
  `LV_OBJ_FLAG_SCROLL_MOMENTUM` throughout the port, and every one of
  those call sites is now a hard `-Werror` build failure under v9.6.0 --
  all 22 were migrated to the dedicated per-flag API.

### 6. Two build-breaking surprises only `-Werror` + an actual build surfaced
Static grepping against the vendored source predicted most of the above,
but these two were only found by running the real build (see
`LVGL_V9.6.0_UPGRADE_TASKS.md` Section E for the exact fixes):

- **`include/lvgl/lvgl.h` unconditionally includes both UEFI backend
  headers.** As of v9.6.0, the top-level `lvgl.h` directly
  `#include`s `drivers/uefi/lv_uefi_edk2.h` **and**
  `drivers/uefi/lv_uefi_gnu_efi.h` whenever `LV_USE_UEFI=1` -- it does not
  gate on which backend `LV_USE_UEFI_INCLUDE` actually selects. (v9.5.0's
  top-level `lvgl.h` referenced no driver headers at all; this blanket
  include is new.) `lv_uefi_gnu_efi.h` does `#include <efi.h>`
  (gnu-efi's own header), which we don't have since we're EDK2-only.
  Fix: `LvglPkg/Include/efi.h` -- a trivial empty-body stub with just an
  include guard. Safe because the only thing `lv_uefi_gnu_efi.h` does
  besides that include is `#define LV_UEFI_GNU_EFI_HEADERS 1`, and that
  macro is confirmed unused anywhere else in the vendored tree -- nothing
  downstream needs real gnu-efi types. `LvglPkg/Include` is declared
  under `[Includes]` in `LvglPkg.dec`, so the stub is visible to every
  consuming module automatically.
- **The object-flag API is deprecated wholesale, not just for
  checked/disabled/pressed state.** See section 5's correction above.
  `lv_obj_add_flag`/`remove_flag`/`clear_flag`/`has_flag` now carry
  `LV_DEPRECATED(...)` for *every* flag, with suggested replacements like
  `lv_obj_set_hidden(obj, true)`, `lv_obj_set_scrollable(obj, bool)`,
  `lv_obj_set_scroll_on_focus`, `lv_obj_set_scroll_elastic`,
  `lv_obj_set_scroll_momentum`. (`lv_obj_add_state`/`remove_state`/
  `has_state` -- the *semantic* state API -- is a separate, non-deprecated
  mechanism; no change needed there.)

### UEFI driver itself -- mostly no action needed on the public API we call
`lv_uefi_init`, `lv_uefi_display_create`, `lv_uefi_display_get_any` are
unchanged in v9.6.0. Three things checked:
- `lv_uefi_std_wrapper.h` was **removed** upstream -- confirmed nothing in
  our wrapper headers (`limits.h`, `stdint.h`, etc. in `Library/LvglLib/`)
  or port code includes it.
- Upstream added a `Kconfig` file to the UEFI driver dir (irrelevant to
  our EDK2-native build, no action).
- The master `lvgl.h`'s blanket UEFI-header include (section 6) *does*
  require action -- the `efi.h` stub above.

## Files this upgrade touches, and why

| File | Why it changes |
|---|---|
| `Library/LvglLib/lvgl` (submodule) | Pointer move v9.5.0 -> v9.6.0 |
| `Library/LvglLib/LvglLib.inf` | `[Sources]` path reconciliation (#3): 481 entries, regenerated from the real tree |
| `Library/LvglLib/lv_conf.h` | Regenerated from the v9.6.0 template; new config macros, private-API gate, color-format rename (#2, #4) |
| `Library/LvglLib/lv_port_indev.c` | Private keypad struct re-check (#2, unchanged); `lv_img_dsc_t` rename (#5); flag-API migration (#6, 4 sites) |
| `Library/LvglLib/MouseCursorIcon.c` | `lv_img_dsc_t` rename (#5) |
| `LvglDisplayEngineDxe/LvglFormRenderer.c` | `lv_btn_create` rename (#5, 7 sites); flag-API migration (#6, 6 sites) |
| `LvglDisplayEngineDxe/LvglAptioChrome.c` | Flag-API migration (#6, 12 sites) -- not originally scoped, found only by building |
| `LvglDisplayEngineDxe/AptioWallpaper.c` | Verified `lv_image_header_t` layout unchanged (#5, already on new type name) -- no edit needed |
| `LvglPkg/Include/efi.h` (new file) | gnu-efi header stub (#6) -- not originally scoped, found only by building |
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
- **`LV_USE_CHECK_ARG` disabled** (corrected macro name -- not `LV_CHECK_ARG`
  as first guessed): firmware-appropriate choice; revisit if v9.6.0-era
  bugs are suspected to be argument-validation-shaped (then temporarily
  enable for a debug build only).
- **32bpp retained** via `LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888`
  (corrected from an earlier `ARGB8888` guess -- the v9.5.0 template's own
  comment documents `LV_COLOR_DEPTH 32` as meaning `XRGB8888`, i.e. no
  alpha channel; `XRGB8888` is the behavior-preserving equivalent).
  Verified v9.6.0's `lv_conf_internal.h` auto-derives `LV_COLOR_DEPTH`
  from `LV_COLOR_FORMAT_DEFAULT` (`LV_COLOR_DEPTH_OF_LV_COLOR_FORMAT_XRGB8888
  = 32`), satisfying `lv_uefi_display.c`'s `#error` guard
  (`#if LV_COLOR_DEPTH != 32`), which is still present verbatim in v9.6.0.

## Verification

1. Both builds compile clean -- **done**:
   ```bash
   source edksetup.sh
   export PACKAGES_PATH=$HOME/workspace/edk2:$HOME/workspace/edk2/LvglPkg
   build -a X64 -t GCC -b DEBUG -p LvglPkg/LvglPkg.dsc
   build -a X64 -t GCC -b DEBUG -p OvmfPkg/OvmfPkgX64.dsc
   ```
   Both exited 0 with `- Done -`. No "source file not found" errors, no
   undefined `lv_*` symbols at link time. **Correction**: `-Werror` *is*
   active (from EDK2's GCC toolchain defaults, not package-local files --
   see section 1's correction above), so this was a real pass/fail signal,
   not just a warnings check.
2. `bash ~/workspace/edk2/qemu_lvgl.sh` -> enter Setup (F2/DEL) ->
   full manual pass per `LVGL_V9.6.0_UPGRADE_TASKS.md` section E.
   **Not yet run** -- this is the interactive functional pass, left for
   the user to do after merging (per the original request: "then i will
   merge it and test it").
3. `git diff --submodule` on the `LvglPkg` superproject shows only the
   intended `v9.5.0 -> v9.6.0` move -- no unrelated submodule drift.

## Rollback

```bash
cd Library/LvglLib/lvgl
git checkout 85aa60d18b3d5e5588d7b247abf90198f07c8a63
cd ../../..
git checkout -- Library/LvglLib/LvglLib.inf Library/LvglLib/lv_conf.h \
  Library/LvglLib/lv_port_indev.c Library/LvglLib/MouseCursorIcon.c \
  LvglDisplayEngineDxe/LvglFormRenderer.c LvglDisplayEngineDxe/LvglAptioChrome.c
rm -f Include/efi.h
```

## Status

Sections A-E (submodule bump, `[Sources]` reconciliation, `lv_conf.h`
migration, deprecated-API cleanup, build verification) are **done** --
both `LvglPkg/LvglPkg.dsc` and `OvmfPkg/OvmfPkgX64.dsc` build clean under
GCC/DEBUG/X64. Section E's manual QEMU functional pass (navigation,
dropdown edit, save/discard, hotkeys, etc.) is **pending** -- left for
the user to run after merging. Section F (root CLAUDE.md version bump)
not yet done.
