/** @file
  Stub for gnu-efi's <efi.h>, required only to satisfy LVGL v9.6.0's
  top-level lvgl.h.

  As of LVGL v9.6.0, include/lvgl/lvgl.h unconditionally includes BOTH
  drivers/uefi/lv_uefi_edk2.h and drivers/uefi/lv_uefi_gnu_efi.h whenever
  LV_USE_UEFI is set, regardless of which backend LV_USE_UEFI_INCLUDE
  actually selects (see Library/LvglLib/lv_conf.h: LV_USE_UEFI_INCLUDE is
  "lv_uefi_edk2.h" -- we never use the gnu-efi backend). Prior to v9.6.0,
  lvgl.h did not reference any UEFI driver header at all, so this never
  came up.

  lv_uefi_gnu_efi.h does nothing but `#include <efi.h>` behind an
  `#if LV_USE_UEFI` guard and define the otherwise-unused macro
  LV_UEFI_GNU_EFI_HEADERS (grep confirms no other LVGL header or source
  file in the vendored tree checks that macro). So this stub only needs
  to exist -- it must not define any gnu-efi types, since EDK2's own
  <Uefi.h> (pulled in via lv_uefi_edk2.h) is what every LVGL UEFI driver
  file we actually compile relies on.

  Placed in LvglPkg/Include (declared under [Includes] in LvglPkg.dec)
  so it is visible to every module that depends on LvglPkg.dec, matching
  how lvgl.h is reached from LvglLib, LvglThemeLib, LvglSetupDxe, and
  LvglDisplayEngineDxe alike.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EFI_H_LVGLPKG_STUB__
#define __EFI_H_LVGLPKG_STUB__

#endif
