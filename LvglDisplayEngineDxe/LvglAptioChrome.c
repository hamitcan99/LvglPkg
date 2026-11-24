/** @file
  AMI Aptio-style chrome implementation.

  Copyright (c) 2024-2026, Hamit Karaca. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "LvglAptioChrome.h"
#include <LvglTheme.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/LvglUiConfigLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Guid/MdeModuleHii.h>

STATIC lv_obj_t   *mClockLabel = NULL;
STATIC lv_timer_t *mClockTimer = NULL;
STATIC lv_obj_t   *mHelpLabel  = NULL;

/**
  UCS-2 -> UTF-8 (caller frees). Local copy to avoid cross-file linkage.
**/
STATIC
CHAR8 *
ChromeUcs2ToUtf8 (
  IN CONST CHAR16  *Str16
  )
{
  UINTN  Len;
  UINTN  Idx;
  UINTN  Out;
  CHAR8  *Utf8;

  if (Str16 == NULL) {
    return NULL;
  }

  for (Len = 0; Str16[Len] != 0; Len++) {
  }

  Utf8 = AllocatePool (Len * 3 + 1);
  if (Utf8 == NULL) {
    return NULL;
  }

  Out = 0;
  for (Idx = 0; Idx < Len; Idx++) {
    UINT16  Ch = Str16[Idx];

    if (Ch < 0x80) {
      Utf8[Out++] = (CHAR8)Ch;
    } else if (Ch < 0x800) {
      Utf8[Out++] = (CHAR8)(0xC0 | (Ch >> 6));
      Utf8[Out++] = (CHAR8)(0x80 | (Ch & 0x3F));
    } else {
      Utf8[Out++] = (CHAR8)(0xE0 | (Ch >> 12));
      Utf8[Out++] = (CHAR8)(0x80 | ((Ch >> 6) & 0x3F));
      Utf8[Out++] = (CHAR8)(0x80 | (Ch & 0x3F));
    }
  }
  Utf8[Out] = '\0';
  return Utf8;
}

/**
  Refresh clock label with current EFI runtime time.
**/
STATIC
VOID
ClockTimerCb (
  lv_timer_t  *Timer
  )
{
  EFI_TIME    Time;
  EFI_STATUS  Status;
  CHAR8       Buf[32];

  if ((mClockLabel == NULL) || (gRT == NULL)) {
    return;
  }

  Status = gRT->GetTime (&Time, NULL);
  if (EFI_ERROR (Status)) {
    lv_label_set_text (mClockLabel, "--:--:--");
    return;
  }

  AsciiSPrint (
    Buf,
    sizeof (Buf),
    "%02d:%02d:%02d   %04d-%02d-%02d",
    Time.Hour, Time.Minute, Time.Second,
    Time.Year, Time.Month, Time.Day
    );
  lv_label_set_text (mClockLabel, Buf);
}

/**
  Build the gradient header bar with title (left), vendor (center), clock (right).
**/
STATIC
VOID
BuildHeader (
  IN lv_obj_t  *Screen
  )
{
  lv_obj_t  *Bar;
  lv_obj_t  *Title;
  lv_obj_t  *Vendor;

  Bar = lv_obj_create (Screen);
  lv_obj_remove_style_all (Bar);
  lv_obj_set_size (Bar, LV_PCT (100), THEME_HEADER_HEIGHT);
  lv_obj_set_style_bg_color (Bar, lv_color_hex (THEME_COLOR_HEADER_BG_TOP), 0);
  lv_obj_set_style_bg_grad_color (Bar, lv_color_hex (THEME_COLOR_HEADER_BG_BOTTOM), 0);
  lv_obj_set_style_bg_grad_dir (Bar, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa (Bar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left (Bar, THEME_PAD_HEADER_X, 0);
  lv_obj_set_style_pad_right (Bar, THEME_PAD_HEADER_X, 0);
  lv_obj_set_style_pad_top (Bar, 0, 0);
  lv_obj_set_style_pad_bottom (Bar, 0, 0);
  lv_obj_set_scrollable (Bar, false);

  // Left: brand title
  Title = lv_label_create (Bar);
  lv_label_set_text (Title, (CONST CHAR8 *)PcdGetPtr (PcdLvglAptioHeaderTitle));
  lv_obj_set_style_text_font (Title, THEME_FONT_TITLE, 0);
  lv_obj_set_style_text_color (Title, lv_color_hex (THEME_COLOR_HEADER_TEXT), 0);
  lv_obj_align (Title, LV_ALIGN_LEFT_MID, 0, 0);

  // Center: vendor / version (dimmed)
  Vendor = lv_label_create (Bar);
  lv_label_set_text (Vendor, (CONST CHAR8 *)PcdGetPtr (PcdLvglAptioHeaderVendor));
  THEME_APPLY_BODY_FONT (Vendor);
  lv_obj_set_style_text_color (Vendor, lv_color_hex (THEME_COLOR_HEADER_DIM), 0);
  lv_obj_align (Vendor, LV_ALIGN_CENTER, 0, 0);

  // Right: clock
  mClockLabel = lv_label_create (Bar);
  THEME_APPLY_BODY_FONT (mClockLabel);
  lv_obj_set_style_text_color (mClockLabel, lv_color_hex (THEME_COLOR_HEADER_TEXT), 0);
  lv_obj_align (mClockLabel, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_label_set_text (mClockLabel, "");
  ClockTimerCb (NULL);
  mClockTimer = lv_timer_create (ClockTimerCb, 1000, NULL);
}

/**
  Find the centered front-page banner (device model) on this form.

  The VFR `banner ... align center` line (device model, LineNumber 1) arrives
  as a Tiano GUIDed EFI_IFR_EXTEND_OP_BANNER opcode in the statement list.

  @param[in]  FormData  Current form.

  @return Allocated UTF-8 device-model string (caller frees), or NULL if the
          form has no centered banner (e.g. any non-front-page form).
**/
STATIC
CHAR8 *
FindCenterBannerUtf8 (
  IN FORM_DISPLAY_ENGINE_FORM  *FormData
  )
{
  LIST_ENTRY                     *Link;
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  EFI_IFR_GUID_BANNER            *Banner;
  CHAR16                         *Ucs2;
  CHAR8                          *Utf8;

  if (FormData == NULL) {
    return NULL;
  }

  for (Link = FormData->StatementListHead.ForwardLink;
       Link != &FormData->StatementListHead;
       Link = Link->ForwardLink)
  {
    Statement = FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK (Link);

    if (Statement->OpCode->OpCode != EFI_IFR_GUID_OP) {
      continue;
    }

    if (!CompareGuid (
           (EFI_GUID *)((UINT8 *)Statement->OpCode + sizeof (EFI_IFR_OP_HEADER)),
           &gEfiIfrTianoGuid
           ))
    {
      continue;
    }

    if (((EFI_IFR_GUID_LABEL *)Statement->OpCode)->ExtendOpCode != EFI_IFR_EXTEND_OP_BANNER) {
      continue;
    }

    Banner = (EFI_IFR_GUID_BANNER *)Statement->OpCode;
    if ((Banner->Alignment != EFI_IFR_BANNER_ALIGN_CENTER) || (Banner->Title == 0)) {
      continue;
    }

    Ucs2 = HiiGetString (FormData->HiiHandle, Banner->Title, NULL);
    if (Ucs2 == NULL) {
      continue;
    }

    Utf8 = ChromeUcs2ToUtf8 (Ucs2);
    FreePool (Ucs2);

    if ((Utf8 != NULL) && (Utf8[0] != '\0')) {
      return Utf8;
    }

    if (Utf8 != NULL) {
      FreePool (Utf8);
    }
  }

  return NULL;
}

/**
  Build the subtitle bar.

  When SubtitleShowsDeviceModel is enabled in Graphical UI Configuration, the
  centered front-page banner (device model) is shown here; otherwise the form
  title is shown.
**/
STATIC
VOID
BuildSubtitleBar (
  IN lv_obj_t                    *Screen,
  IN FORM_DISPLAY_ENGINE_FORM    *FormData,
  IN BOOLEAN                     SubtitleShowsDeviceModel
  )
{
  lv_obj_t  *Bar;
  lv_obj_t  *Label;
  CHAR16    *Str16;
  CHAR8     *Utf8;

  Bar = lv_obj_create (Screen);
  lv_obj_remove_style_all (Bar);
  lv_obj_set_size (Bar, LV_PCT (100), THEME_SUBTITLE_HEIGHT);
  lv_obj_set_style_bg_color (Bar, lv_color_hex (THEME_COLOR_SUBTITLE_BG), 0);
  lv_obj_set_style_bg_opa (Bar, LV_OPA_80, 0);
  lv_obj_set_style_border_color (Bar, lv_color_hex (THEME_COLOR_HIGHLIGHT_ROW), 0);
  lv_obj_set_style_border_width (Bar, 0, 0);
  lv_obj_set_style_border_side (Bar, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_pad_left (Bar, THEME_PAD_SUBTITLE_X, 0);
  lv_obj_set_style_pad_right (Bar, THEME_PAD_SUBTITLE_X, 0);
  lv_obj_set_style_pad_top (Bar, 0, 0);
  lv_obj_set_style_pad_bottom (Bar, 0, 0);
  lv_obj_set_scrollable (Bar, false);

  Str16 = NULL;
  Utf8  = NULL;

  if (SubtitleShowsDeviceModel) {
    Utf8 = FindCenterBannerUtf8 (FormData);
  }

  if (Utf8 == NULL) {
    if ((FormData != NULL) && (FormData->FormTitle != 0)) {
      Str16 = HiiGetString (FormData->HiiHandle, FormData->FormTitle, NULL);
    }

    if (Str16 != NULL) {
      Utf8 = ChromeUcs2ToUtf8 (Str16);
      FreePool (Str16);
    }
  }

  Label = lv_label_create (Bar);
  lv_label_set_text (Label, Utf8 != NULL ? Utf8 : "Setup");
  THEME_APPLY_BODY_FONT (Label);
  lv_obj_set_style_text_color (Label, lv_color_hex (THEME_COLOR_SUBTITLE_TEXT), 0);
  lv_obj_align (
    Label,
    SubtitleShowsDeviceModel ? LV_ALIGN_CENTER : LV_ALIGN_LEFT_MID,
    0,
    0
    );

  if (Utf8 != NULL) {
    FreePool (Utf8);
  }
}

/**
  Append a single chip (label-on-pill) to the hotkey bar.
**/
STATIC
VOID
AddHotKeyChip (
  IN lv_obj_t       *Parent,
  IN CONST CHAR8    *KeyText,
  IN CONST CHAR8    *LabelText
  )
{
  lv_obj_t  *Chip;
  lv_obj_t  *KeyLbl;
  lv_obj_t  *DescLbl;

  Chip = lv_obj_create (Parent);
  lv_obj_remove_style_all (Chip);
  lv_obj_set_size (Chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Chip, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_left (Chip, THEME_PAD_CHIP_X, 0);
  lv_obj_set_style_pad_right (Chip, THEME_PAD_CHIP_X, 0);
  lv_obj_set_style_pad_top (Chip, THEME_PAD_CHIP_Y, 0);
  lv_obj_set_style_pad_bottom (Chip, THEME_PAD_CHIP_Y, 0);
  lv_obj_set_style_pad_column (Chip, THEME_PAD_CHIP_COL_GAP, 0);
  lv_obj_set_style_radius (Chip, THEME_RADIUS_CHIP, 0);
  lv_obj_set_style_bg_color (Chip, lv_color_hex (THEME_COLOR_BG_PANEL), 0);
  lv_obj_set_style_bg_opa (Chip, LV_OPA_COVER, 0);
  lv_obj_set_scrollable (Chip, false);

  KeyLbl = lv_label_create (Chip);
  lv_label_set_text (KeyLbl, KeyText);
  THEME_APPLY_BODY_FONT (KeyLbl);
  lv_obj_set_style_text_color (KeyLbl, lv_color_hex (THEME_COLOR_ACCENT_HOVER), 0);

  DescLbl = lv_label_create (Chip);
  lv_label_set_text (DescLbl, LabelText);
  THEME_APPLY_BODY_FONT (DescLbl);
  lv_obj_set_style_text_color (DescLbl, lv_color_hex (THEME_COLOR_FOOTER_TEXT), 0);
}

/**
  Map an EFI scan code to a short chip label like "F9".
  Returns NULL if unsupported (chip is skipped).
**/
STATIC
CONST CHAR8 *
ScanCodeToChipKey (
  IN UINT16  ScanCode
  )
{
  STATIC CONST struct {
    UINT16          Scan;
    CONST CHAR8     *Label;
  } Map[] = {
    { SCAN_F1,  "F1"  }, { SCAN_F2,  "F2"  }, { SCAN_F3,  "F3"  },
    { SCAN_F4,  "F4"  }, { SCAN_F5,  "F5"  }, { SCAN_F6,  "F6"  },
    { SCAN_F7,  "F7"  }, { SCAN_F8,  "F8"  }, { SCAN_F9,  "F9"  },
    { SCAN_F10, "F10" }, { SCAN_F11, "F11" }, { SCAN_F12, "F12" }
  };
  UINTN  Idx;

  for (Idx = 0; Idx < ARRAY_SIZE (Map); Idx++) {
    if (Map[Idx].Scan == ScanCode) {
      return Map[Idx].Label;
    }
  }
  return NULL;
}

/**
  Build the footer: chip row of registered hotkeys plus standard nav chips.
**/
STATIC
VOID
BuildFooter (
  IN lv_obj_t                    *Screen,
  IN FORM_DISPLAY_ENGINE_FORM    *FormData
  )
{
  lv_obj_t         *Bar;
  LIST_ENTRY       *Link;
  BROWSER_HOT_KEY  *HotKey;
  CONST CHAR8      *KeyLabel;
  CHAR8            *Utf8;

  Bar = lv_obj_create (Screen);
  lv_obj_remove_style_all (Bar);
  lv_obj_set_size (Bar, LV_PCT (100), THEME_FOOTER_HEIGHT);
  lv_obj_set_style_bg_color (Bar, lv_color_hex (THEME_COLOR_FOOTER_BG), 0);
  lv_obj_set_style_bg_opa (Bar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left (Bar, THEME_PAD_FOOTER_X, 0);
  lv_obj_set_style_pad_right (Bar, THEME_PAD_FOOTER_X, 0);
  lv_obj_set_style_pad_top (Bar, THEME_PAD_FOOTER_Y, 0);
  lv_obj_set_style_pad_bottom (Bar, THEME_PAD_FOOTER_Y, 0);
  lv_obj_set_style_pad_column (Bar, THEME_PAD_FOOTER_COL_GAP, 0);
  lv_obj_set_flex_flow (Bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (Bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollable (Bar, false);

  //
  // "Unsaved changes" notice -- prepended when any question on the form
  // has been modified since the form opened. SetupBrowserDxe maintains
  // FormData->SettingChangedFlag.
  //
  if ((FormData != NULL) && FormData->SettingChangedFlag) {
    lv_obj_t  *Notice;

    Notice = lv_label_create (Bar);
    lv_label_set_text (Notice, LV_SYMBOL_WARNING "  Unsaved changes -- press F10 to save");
    THEME_APPLY_BODY_FONT (Notice);
    lv_obj_set_style_text_color (Notice, lv_color_hex (THEME_COLOR_WARNING), 0);
    lv_obj_set_style_pad_right (Notice, THEME_PAD_FOOTER_X, 0);
  }

  //
  // Standard navigation chips (always present).
  //
  AddHotKeyChip (Bar, LV_SYMBOL_UP LV_SYMBOL_DOWN, "Move");
  AddHotKeyChip (Bar, "Enter", "Select");
  AddHotKeyChip (Bar, "Esc",   "Exit");

  //
  // Driver-registered hotkeys from FormData->HotKeyListHead.
  //
  if ((FormData != NULL) && !IsListEmpty (&FormData->HotKeyListHead)) {
    for (Link = FormData->HotKeyListHead.ForwardLink;
         Link != &FormData->HotKeyListHead;
         Link = Link->ForwardLink)
    {
      HotKey = BROWSER_HOT_KEY_FROM_LINK (Link);
      if ((HotKey->KeyData == NULL) || (HotKey->HelpString == NULL)) {
        continue;
      }
      KeyLabel = ScanCodeToChipKey (HotKey->KeyData->ScanCode);
      if (KeyLabel == NULL) {
        continue;
      }
      Utf8 = ChromeUcs2ToUtf8 (HotKey->HelpString);
      AddHotKeyChip (Bar, KeyLabel, Utf8 != NULL ? Utf8 : "");
      if (Utf8 != NULL) {
        FreePool (Utf8);
      }
    }
  }
}

lv_obj_t *
EFIAPI
AptioBuildChrome (
  IN lv_obj_t                    *Screen,
  IN FORM_DISPLAY_ENGINE_FORM    *FormData
  )
{
  lv_obj_t                       *Content;
  lv_obj_t                       *ChromeRoot;
  lv_obj_t                       *Frame;
  LVGL_UI_CONFIG_VARSTORE_DATA   UiConfig;
  int32_t                        HorRes;
  int32_t                        VerRes;
  int32_t                        FrameW;
  int32_t                        FrameH;

  ChromeRoot = Screen;
  LvglUiConfigLoad (&UiConfig);

  if (UiConfig.CenteredFrameEnabled != 0) {
    //
    // Screen is the full-display backdrop. The UI itself lives in a centered
    // "Frame" sized from the Graphical UI Configuration settings (PCD defaults
    // on first boot).
    //
    lv_obj_remove_style_all (Screen);
    lv_obj_set_style_bg_color (Screen, lv_color_hex (THEME_COLOR_BACKDROP), 0);
    lv_obj_set_style_bg_opa (Screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all (Screen, 0, 0);
    lv_obj_set_scrollable (Screen, false);

    HorRes = LV_HOR_RES;
    VerRes = LV_VER_RES;
    FrameH = (VerRes * UiConfig.CenteredFrameHeightPct) / 100;
    if ((FrameH <= 0) || (FrameH > VerRes)) {
      FrameH = VerRes;
    }

    FrameW = (FrameH * UiConfig.CenteredFrameAspectNum) / UiConfig.CenteredFrameAspectDen;
    if ((FrameW <= 0) || (FrameW > HorRes)) {
      FrameW = HorRes;
    }

    //
    // Frame is a vertical flex column with an Aptio-style gradient background
    // (dark navy top -> mid navy bottom). The compiled-in RGB565 wallpaper
    // image is bypassed -- LVGL's decoder doesn't render it cleanly with
    // LV_COLOR_DEPTH=32 and the asset is also smaller than a typical GOP
    // framebuffer (800x600 vs 1280x800), so it wouldn't tile/scale correctly.
    //
    Frame = lv_obj_create (Screen);
    lv_obj_remove_style_all (Frame);
    lv_obj_set_size (Frame, FrameW, FrameH);
    lv_obj_center (Frame);
    lv_obj_set_style_bg_color (Frame, lv_color_hex (THEME_COLOR_BG_SCREEN), 0);
    lv_obj_set_style_bg_grad_color (Frame, lv_color_hex (THEME_COLOR_BG_SCREEN), 0);
    lv_obj_set_style_bg_grad_dir (Frame, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa (Frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color (Frame, lv_color_hex (THEME_COLOR_BG_SEPARATOR), 0);
    lv_obj_set_style_border_width (Frame, THEME_BORDER_PANE, 0);
    lv_obj_set_flex_flow (Frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all (Frame, 0, 0);
    lv_obj_set_style_pad_row (Frame, 0, 0);
    lv_obj_set_scrollable (Frame, false);

    ChromeRoot = Frame;
  } else {
    //
    // Screen is a vertical flex column with an Aptio-style gradient
    // background (dark navy top -> mid navy bottom). The compiled-in
    // RGB565 wallpaper image is bypassed -- LVGL's decoder doesn't render
    // it cleanly with LV_COLOR_DEPTH=32 and the asset is also smaller
    // than a typical GOP framebuffer (800x600 vs 1280x800), so it
    // wouldn't tile/scale correctly anyway.
    //
    lv_obj_remove_style_all (Screen);
    lv_obj_set_style_bg_color (Screen, lv_color_hex (THEME_COLOR_BG_SCREEN), 0);
    lv_obj_set_style_bg_grad_color (Screen, lv_color_hex (THEME_COLOR_BG_SCREEN), 0);
    lv_obj_set_style_bg_grad_dir (Screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa (Screen, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow (Screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all (Screen, 0, 0);
    lv_obj_set_style_pad_row (Screen, 0, 0);
    lv_obj_set_scrollable (Screen, false);
  }

  // Header / subtitle / [content row | help pane] / footer in flex order.
  BuildHeader (ChromeRoot);
  BuildSubtitleBar (ChromeRoot, FormData, (UiConfig.SubtitleShowsDeviceModel != 0));

  //
  // Middle band: horizontal flex with rows panel on the left and help pane
  // on the right. The middle band itself fills the vertical space between
  // the subtitle bar and the footer.
  //
  {
    lv_obj_t  *Middle;
    lv_obj_t  *HelpPane;
    lv_obj_t  *HelpHeader;

    Middle = lv_obj_create (ChromeRoot);
    lv_obj_remove_style_all (Middle);
    lv_obj_set_width (Middle, LV_PCT (100));
    lv_obj_set_flex_grow (Middle, 1);
    lv_obj_set_flex_flow (Middle, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all (Middle, 0, 0);
    lv_obj_set_style_pad_column (Middle, 0, 0);
    lv_obj_set_style_bg_opa (Middle, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable (Middle, false);

    // Left: scrollable form rows (transparent column).
    Content = lv_obj_create (Middle);
    lv_obj_remove_style_all (Content);
    lv_obj_set_height (Content, LV_PCT (100));
    lv_obj_set_flex_grow (Content, 1);
    lv_obj_set_flex_flow (Content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left (Content, THEME_PAD_CONTENT_LEFT, 0);
    lv_obj_set_style_pad_right (Content, THEME_PAD_CONTENT_RIGHT, 0);
    lv_obj_set_style_pad_top (Content, THEME_PAD_CONTENT_Y, 0);
    lv_obj_set_style_pad_bottom (Content, THEME_PAD_CONTENT_Y, 0);
    lv_obj_set_style_pad_row (Content, THEME_PAD_CONTENT_ROW_GAP, 0);
    lv_obj_set_style_bg_opa (Content, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable (Content, true);
    lv_obj_set_scroll_elastic (Content, false);
    lv_obj_set_scroll_momentum (Content, false);
    LvglThemeApplyBodyFont (Content);

    // Right: help pane (percentage width). Header label "Help" + body label.
    HelpPane = lv_obj_create (Middle);
    lv_obj_remove_style_all (HelpPane);
    lv_obj_set_size (HelpPane, LV_PCT (PcdGet8 (PcdLvglHelpPaneWidthPct)), LV_PCT (100));
    lv_obj_set_style_bg_color (HelpPane, lv_color_hex (THEME_COLOR_BG_PANEL), 0);
    lv_obj_set_style_bg_opa (HelpPane, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color (HelpPane, lv_color_hex (THEME_COLOR_BG_SEPARATOR), 0);
    lv_obj_set_style_border_side (HelpPane, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width (HelpPane, THEME_BORDER_PANE, 0);
    lv_obj_set_style_pad_left (HelpPane, THEME_PAD_HELPPANE_X, 0);
    lv_obj_set_style_pad_right (HelpPane, THEME_PAD_HELPPANE_X, 0);
    lv_obj_set_style_pad_top (HelpPane, THEME_PAD_HELPPANE_Y, 0);
    lv_obj_set_style_pad_bottom (HelpPane, THEME_PAD_HELPPANE_Y, 0);
    lv_obj_set_style_pad_row (HelpPane, THEME_PAD_HELPPANE_ROW_GAP, 0);
    lv_obj_set_flex_flow (HelpPane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable (HelpPane, false);

    HelpHeader = lv_label_create (HelpPane);
    lv_label_set_text (HelpHeader, "Help");
    THEME_APPLY_BODY_FONT (HelpHeader);
    lv_obj_set_style_text_color (HelpHeader, lv_color_hex (THEME_COLOR_TEXT_SECONDARY), 0);

    mHelpLabel = lv_label_create (HelpPane);
    lv_label_set_long_mode (mHelpLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width (mHelpLabel, LV_PCT (100));
    THEME_APPLY_BODY_FONT (mHelpLabel);
    lv_obj_set_style_text_color (mHelpLabel, lv_color_hex (THEME_COLOR_TEXT_PRIMARY), 0);
    lv_label_set_text (mHelpLabel, "");
  }

  BuildFooter (ChromeRoot, FormData);

  return Content;
}

VOID
EFIAPI
AptioChromeTeardown (
  VOID
  )
{
  if (mClockTimer != NULL) {
    lv_timer_delete (mClockTimer);
    mClockTimer = NULL;
  }
  mClockLabel = NULL;
  mHelpLabel  = NULL;
}

VOID
EFIAPI
AptioSetHelpText (
  IN CONST CHAR8  *Utf8
  )
{
  if (mHelpLabel == NULL) {
    return;
  }
  lv_label_set_text (mHelpLabel, (Utf8 != NULL) ? Utf8 : "");
}
