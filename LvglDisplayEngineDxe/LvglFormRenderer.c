/** @file
  LVGL Form Renderer — creates LVGL widgets from FORM_DISPLAY_ENGINE_FORM.

  Walks the StatementListHead provided by SetupBrowserDxe and maps each
  IFR opcode to a corresponding LVGL widget. Runs the LVGL event loop
  until the user selects a question or presses ESC, then fills USER_INPUT
  for the browser.

  Copyright (c) 2024-2026, Hamit Karaca. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "LvglFormRenderer.h"

STATIC LVGL_FORM_SESSION  mSession;
STATIC BOOLEAN            mLvglReady = FALSE;

//
// Popup state — shared by the F10 in-loop overlay and LvglRunConfirmPopup.
//
#define LVGL_POPUP_PENDING  0xFFFFFFFFU

STATIC UINT32     mPopupResult       = LVGL_POPUP_PENDING;
STATIC lv_obj_t  *mPopupOverlay      = NULL;
STATIC lv_obj_t  *mPopupFirstObj     = NULL;
STATIC lv_obj_t  *mPopupLastObj      = NULL;
STATIC UINT32     mPopupConfirmAction = 0;
STATIC UINT16     mPendingDefaultId   = 0;
STATIC UINT32     mDiscardAction      = BROWSER_ACTION_DISCARD;
STATIC UINT32     mNoneAction         = BROWSER_ACTION_NONE;

//
// Forward declarations for widget builders.
//
STATIC VOID ShowPopup (lv_group_t *Group, CONST CHAR8 *Title, CONST CHAR8 *ConfirmLabel, UINT32 ConfirmAction, BOOLEAN ShowDiscard);
STATIC VOID CreateSubtitleWidget      (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle);
STATIC VOID CreateTextWidget          (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle);
STATIC VOID CreateCheckboxWidget      (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateNumericWidget       (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateOneOfWidget         (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateOrderedListWidget   (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateStringWidget        (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateRefWidget           (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateActionWidget        (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);

//
// ---- Helper: UCS-2 string to UTF-8 for LVGL ----
//

/**
  Convert a UCS-2 (CHAR16) string to a UTF-8 (CHAR8) buffer.
  Caller must FreePool() the result.

  @param[in] Str16  UCS-2 string.

  @return Allocated UTF-8 string, or NULL on failure.
**/
STATIC
CHAR8 *
Ucs2ToUtf8 (
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

  //
  // Worst case: each UCS-2 char becomes 3 UTF-8 bytes.
  //
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
  Get prompt text for a statement as UTF-8. Caller must FreePool().
**/
STATIC
CHAR8 *
GetPromptUtf8 (
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement,
  IN EFI_HII_HANDLE                HiiHandle
  )
{
  EFI_STRING_ID  PromptId;
  CHAR16         *Str16;
  CHAR8          *Utf8;

  //
  // The Prompt field is at the same offset in both EFI_IFR_STATEMENT_HEADER
  // and EFI_IFR_QUESTION_HEADER (which embeds STATEMENT_HEADER first).
  // Cast through the smallest common structure.
  //
  PromptId = ((EFI_IFR_SUBTITLE *)Statement->OpCode)->Statement.Prompt;
  if (PromptId == 0) {
    return NULL;
  }

  Str16 = HiiGetString (HiiHandle, PromptId, NULL);
  if (Str16 == NULL) {
    return NULL;
  }

  Utf8 = Ucs2ToUtf8 (Str16);
  FreePool (Str16);
  return Utf8;
}

//
// ---- Ordered list array helpers (mirror of DisplayEngineDxe ProcessOptions.c) ----
//

STATIC
UINT64
GetArrayData (
  IN VOID   *Array,
  IN UINT8  Type,
  IN UINTN  Index
  )
{
  switch (Type) {
    case EFI_IFR_TYPE_NUM_SIZE_8:
      return (UINT64)*(((UINT8 *)Array) + Index);
    case EFI_IFR_TYPE_NUM_SIZE_16:
      return (UINT64)*(((UINT16 *)Array) + Index);
    case EFI_IFR_TYPE_NUM_SIZE_32:
      return (UINT64)*(((UINT32 *)Array) + Index);
    case EFI_IFR_TYPE_NUM_SIZE_64:
      return (UINT64)*(((UINT64 *)Array) + Index);
    default:
      return 0;
  }
}

STATIC
VOID
SetArrayData (
  IN VOID    *Array,
  IN UINT8   Type,
  IN UINTN   Index,
  IN UINT64  Value
  )
{
  switch (Type) {
    case EFI_IFR_TYPE_NUM_SIZE_8:
      *(((UINT8 *)Array) + Index) = (UINT8)Value;
      break;
    case EFI_IFR_TYPE_NUM_SIZE_16:
      *(((UINT16 *)Array) + Index) = (UINT16)Value;
      break;
    case EFI_IFR_TYPE_NUM_SIZE_32:
      *(((UINT32 *)Array) + Index) = (UINT32)Value;
      break;
    case EFI_IFR_TYPE_NUM_SIZE_64:
      *(((UINT64 *)Array) + Index) = (UINT64)Value;
      break;
    default:
      break;
  }
}

//
// Per-button context for ordered-list Up/Down buttons.
//
typedef struct {
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  UINT8                          ValueType;
  UINTN                          ActiveCount;
  UINTN                          Index;
  INT32                          Direction;
} LVGL_ORDERED_MOVE_CTX;

//
// ---- Event callbacks ----
//

/**
  Generic click handler — records the statement selection and requests exit.
**/
STATIC
VOID
OnStatementClicked (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  if (Ctx == NULL || mSession.UserInput == NULL) {
    return;
  }

  mSession.UserInput->SelectedStatement = Ctx->Statement;
  CopyMem (
    &mSession.UserInput->InputValue,
    &Ctx->Statement->CurrentValue,
    sizeof (EFI_HII_VALUE)
    );
  mSession.UserInput->Action = 0;
  mSession.ExitRequested     = TRUE;
}

/**
  String textarea commit — fires on LV_EVENT_READY (Enter key).
  Reads the typed UTF-8 text, converts to UTF-16, fills InputValue.Buffer,
  and registers a new HII string so SetupBrowserDxe can persist the value.
**/
STATIC
VOID
OnStringReady (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;
  lv_obj_t                *Ta;
  CONST CHAR8             *Utf8Text;
  CHAR16                  *Str16;
  CHAR16                  *PoolBuf;
  UINTN                   SrcLen;
  UINTN                   BufBytes;
  UINTN                   MaxChars;
  UINTN                   CopyChars;
  UINTN                   Idx;
  EFI_IFR_STRING          *StringOp;
  EFI_STRING_ID           NewStringId;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  if ((Ctx == NULL) || (mSession.UserInput == NULL)) {
    return;
  }

  Ta       = lv_event_get_target_obj (Event);
  Utf8Text = lv_textarea_get_text (Ta);
  if (Utf8Text == NULL) {
    Utf8Text = "";
  }

  SrcLen = AsciiStrLen (Utf8Text);
  Str16  = AllocateZeroPool ((SrcLen + 1) * sizeof (CHAR16));
  if (Str16 == NULL) {
    return;
  }

  for (Idx = 0; Idx < SrcLen; Idx++) {
    Str16[Idx] = (CHAR16)(UINT8)Utf8Text[Idx];
  }

  Str16[SrcLen] = L'\0';

  //
  // SetupBrowserDxe Presentation.c does:
  //   CopyMem (Statement->BufferValue, InputValue.Buffer, InputValue.BufferLen);
  //   FreePool (InputValue.Buffer);
  // so Buffer must be a real pool allocation sized to the field's StorageWidth,
  // which SetupBrowser copies into CurrentValue.BufferLen when building the
  // display statement. Fall back to EFI_IFR_STRING.MaxSize if BufferLen is 0.
  //
  BufBytes = Ctx->Statement->CurrentValue.BufferLen;
  if (BufBytes == 0) {
    StringOp = (EFI_IFR_STRING *)Ctx->Statement->OpCode;
    BufBytes  = (UINTN)StringOp->MaxSize * sizeof (CHAR16);
  }

  if (BufBytes < sizeof (CHAR16)) {
    FreePool (Str16);
    return;
  }

  PoolBuf = AllocateZeroPool (BufBytes);
  if (PoolBuf == NULL) {
    FreePool (Str16);
    return;
  }

  MaxChars  = (BufBytes / sizeof (CHAR16)) - 1;
  CopyChars = (SrcLen < MaxChars) ? SrcLen : MaxChars;
  CopyMem (PoolBuf, Str16, CopyChars * sizeof (CHAR16));

  NewStringId = HiiSetString (mSession.FormData->HiiHandle, 0, Str16, NULL);
  FreePool (Str16);
  if (NewStringId == 0) {
    FreePool (PoolBuf);
    return;
  }

  mSession.UserInput->SelectedStatement       = Ctx->Statement;
  mSession.UserInput->InputValue.Type         = EFI_IFR_TYPE_STRING;
  mSession.UserInput->InputValue.Buffer       = (UINT8 *)PoolBuf;
  mSession.UserInput->InputValue.BufferLen    = (UINT16)BufBytes;
  mSession.UserInput->InputValue.Value.string = NewStringId;
  mSession.UserInput->Action                  = 0;
  mSession.ExitRequested                      = TRUE;
}

/**
  Checkbox value-changed handler — toggles the boolean and records it.
**/
STATIC
VOID
OnCheckboxChanged (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;
  lv_obj_t                *Cb;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  if (Ctx == NULL || mSession.UserInput == NULL) {
    return;
  }

  Cb = lv_event_get_target_obj (Event);

  mSession.UserInput->SelectedStatement          = Ctx->Statement;
  mSession.UserInput->InputValue.Type            = EFI_IFR_TYPE_BOOLEAN;
  mSession.UserInput->InputValue.Value.b         = lv_obj_has_state (Cb, LV_STATE_CHECKED) ? TRUE : FALSE;
  mSession.UserInput->Action                     = 0;
  mSession.ExitRequested                         = TRUE;
}

/**
  Dropdown value-changed handler — records the selected option index.
**/
STATIC
VOID
OnDropdownChanged (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT       *Ctx;
  lv_obj_t                     *Dd;
  UINT32                       SelIdx;
  LIST_ENTRY                   *Link;
  DISPLAY_QUESTION_OPTION      *Option;
  UINT32                       CurIdx;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  if (Ctx == NULL || mSession.UserInput == NULL) {
    return;
  }

  Dd     = lv_event_get_target_obj (Event);
  SelIdx = lv_dropdown_get_selected (Dd);

  //
  // Walk the option list to find the selected option's value.
  //
  CurIdx = 0;
  for (Link = Ctx->Statement->OptionListHead.ForwardLink;
       Link != &Ctx->Statement->OptionListHead;
       Link = Link->ForwardLink)
  {
    Option = DISPLAY_QUESTION_OPTION_FROM_LINK (Link);
    if (CurIdx == SelIdx) {
      mSession.UserInput->SelectedStatement = Ctx->Statement;
      mSession.UserInput->InputValue.Type   = Option->OptionOpCode->Type;
      //
      // Zero the union first, then copy only the type-appropriate bytes.
      // The IFR binary stores Value at its native width; reading the full
      // sizeof(EFI_IFR_TYPE_VALUE) would read into the next IFR opcode.
      //
      ZeroMem (&mSession.UserInput->InputValue.Value, sizeof (EFI_IFR_TYPE_VALUE));
      switch (Option->OptionOpCode->Type) {
        case EFI_IFR_TYPE_NUM_SIZE_8:
          mSession.UserInput->InputValue.Value.u8  = Option->OptionOpCode->Value.u8;
          break;
        case EFI_IFR_TYPE_NUM_SIZE_16:
          mSession.UserInput->InputValue.Value.u16 = Option->OptionOpCode->Value.u16;
          break;
        case EFI_IFR_TYPE_NUM_SIZE_32:
          mSession.UserInput->InputValue.Value.u32 = Option->OptionOpCode->Value.u32;
          break;
        case EFI_IFR_TYPE_NUM_SIZE_64:
          mSession.UserInput->InputValue.Value.u64 = Option->OptionOpCode->Value.u64;
          break;
        default:
          break;
      }
      mSession.UserInput->Action = 0;
      mSession.ExitRequested     = TRUE;
      return;
    }

    CurIdx++;
  }
}

/**
  Ordered-list Up/Down button handler — swaps two entries in the buffer
  and emits the reordered buffer via USER_INPUT so SetupBrowserDxe
  re-invokes FormDisplay() with the updated state.
**/
STATIC
VOID
OnOrderedListMove (
  lv_event_t  *Event
  )
{
  LVGL_ORDERED_MOVE_CTX           *MoveCtx;
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement;
  VOID                            *NewBuf;
  UINTN                           i;
  UINTN                           j;
  UINT64                          Tmp;

  MoveCtx = (LVGL_ORDERED_MOVE_CTX *)lv_event_get_user_data (Event);
  if (MoveCtx == NULL || mSession.UserInput == NULL) {
    return;
  }

  Statement = MoveCtx->Statement;
  i = MoveCtx->Index;
  j = (UINTN)((INT32)i + MoveCtx->Direction);

  if (j >= MoveCtx->ActiveCount) {
    return;
  }

  NewBuf = AllocateCopyPool (Statement->CurrentValue.BufferLen, Statement->CurrentValue.Buffer);
  if (NewBuf == NULL) {
    return;
  }

  Tmp = GetArrayData (NewBuf, MoveCtx->ValueType, i);
  SetArrayData (NewBuf, MoveCtx->ValueType, i, GetArrayData (NewBuf, MoveCtx->ValueType, j));
  SetArrayData (NewBuf, MoveCtx->ValueType, j, Tmp);

  mSession.UserInput->SelectedStatement  = Statement;
  mSession.UserInput->InputValue.Type    = EFI_IFR_TYPE_BUFFER;
  mSession.UserInput->InputValue.Buffer  = NewBuf;
  mSession.UserInput->InputValue.BufferLen = Statement->CurrentValue.BufferLen;
  mSession.UserInput->Action             = 0;
  mSession.ExitRequested                 = TRUE;
}

/**
  Popup button click handler — records the chosen action and dismisses overlay.
**/
STATIC
VOID
OnPopupBtn (
  lv_event_t  *Event
  )
{
  UINT32  *ActionPtr;

  ActionPtr    = (UINT32 *)lv_event_get_user_data (Event);
  mPopupResult = (ActionPtr != NULL) ? *ActionPtr : BROWSER_ACTION_NONE;

  if (mPopupOverlay != NULL) {
    lv_obj_delete (mPopupOverlay);
    mPopupOverlay = NULL;
    mPopupFirstObj = NULL;
    mPopupLastObj  = NULL;
  }
}

/**
  Popup keyboard navigation handler.
  - ESC: dismiss popup (cancel).
  - UP/LEFT / DOWN/RIGHT: cycle through popup buttons without escaping to form.
**/
STATIC
VOID
OnPopupKey (
  lv_event_t  *Event
  )
{
  lv_key_t  Key;
  lv_obj_t  *Focused;

  Key = lv_indev_get_key (lv_indev_active ());

  if (Key == LV_KEY_ESC) {
    mPopupResult = BROWSER_ACTION_NONE;
    if (mPopupOverlay != NULL) {
      lv_obj_delete (mPopupOverlay);
      mPopupOverlay  = NULL;
      mPopupFirstObj = NULL;
      mPopupLastObj  = NULL;
    }

    lv_event_stop_processing (Event);
    return;
  }

  if ((Key == LV_KEY_LEFT) || (Key == LV_KEY_UP)) {
    Focused = lv_group_get_focused (mSession.Group);
    if (Focused != mPopupFirstObj) {
      lv_group_focus_prev (mSession.Group);
    }

    lv_event_stop_processing (Event);
  } else if ((Key == LV_KEY_RIGHT) || (Key == LV_KEY_DOWN)) {
    Focused = lv_group_get_focused (mSession.Group);
    if (Focused != mPopupLastObj) {
      lv_group_focus_next (mSession.Group);
    }

    lv_event_stop_processing (Event);
  }
}

/**
  Create a modal confirmation popup on top of the current screen.

  Buttons are added to @a Group so keyboard navigation stays within the popup.
  The confirm button uses @a ConfirmAction as its result value.
  If @a ShowDiscard is TRUE a third "Discard" button is shown (for the
  "unsaved changes" scenario where the user can choose to throw away edits).
**/
STATIC
VOID
ShowPopup (
  IN lv_group_t  *Group,
  IN CONST CHAR8  *Title,
  IN CONST CHAR8  *ConfirmLabel,
  IN UINT32        ConfirmAction,
  IN BOOLEAN       ShowDiscard
  )
{
  lv_obj_t  *Overlay;
  lv_obj_t  *Card;
  lv_obj_t  *TitleLbl;
  lv_obj_t  *MsgLbl;
  lv_obj_t  *Sep;
  lv_obj_t  *BtnRow;
  lv_obj_t  *ConfirmBtn;
  lv_obj_t  *DiscardBtn;
  lv_obj_t  *CancelBtn;
  lv_obj_t  *Lbl;

  mPopupConfirmAction = ConfirmAction;
  mPopupResult        = LVGL_POPUP_PENDING;

  //
  // Semi-transparent full-screen overlay that blocks clicks on form widgets.
  //
  Overlay = lv_obj_create (lv_screen_active ());
  lv_obj_set_size (Overlay, LV_PCT (100), LV_PCT (100));
  lv_obj_set_style_bg_color (Overlay, lv_color_black (), 0);
  lv_obj_set_style_bg_opa (Overlay, LV_OPA_50, 0);
  lv_obj_set_style_border_width (Overlay, 0, 0);
  lv_obj_set_style_pad_all (Overlay, 0, 0);
  lv_obj_set_flex_flow (Overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align (Overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  mPopupOverlay = Overlay;

  //
  // Dialog card.
  //
  Card = lv_obj_create (Overlay);
  lv_obj_set_width (Card, LV_PCT (50));
  lv_obj_set_height (Card, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all (Card, 16, 0);
  lv_obj_set_style_pad_row (Card, 10, 0);
  lv_obj_set_style_bg_color (Card, lv_color_hex (0x2A2A4A), 0);
  lv_obj_set_style_radius (Card, 8, 0);
  lv_obj_set_style_border_width (Card, 0, 0);

  TitleLbl = lv_label_create (Card);
  lv_label_set_text (TitleLbl, Title);
  lv_obj_set_style_text_font (TitleLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color (TitleLbl, lv_color_white (), 0);

  Sep = lv_obj_create (Card);
  lv_obj_set_size (Sep, LV_PCT (100), 1);
  lv_obj_set_style_bg_color (Sep, lv_color_hex (0x555580), 0);
  lv_obj_set_style_border_width (Sep, 0, 0);
  lv_obj_set_style_pad_all (Sep, 0, 0);

  MsgLbl = lv_label_create (Card);
  lv_label_set_text (MsgLbl, ShowDiscard ? "You have unsaved changes." : "Save the current settings?");
  lv_obj_set_style_text_color (MsgLbl, lv_color_hex (0xCCCCCC), 0);

  BtnRow = lv_obj_create (Card);
  lv_obj_set_size (BtnRow, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (BtnRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (BtnRow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all (BtnRow, 0, 0);
  lv_obj_set_style_border_width (BtnRow, 0, 0);
  lv_obj_set_style_bg_opa (BtnRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_column (BtnRow, 8, 0);

  //
  // Confirm (Save / Load) button.
  //
  ConfirmBtn = lv_btn_create (BtnRow);
  Lbl        = lv_label_create (ConfirmBtn);
  lv_label_set_text (Lbl, ConfirmLabel);
  lv_obj_add_event_cb (ConfirmBtn, OnPopupBtn, LV_EVENT_CLICKED, &mPopupConfirmAction);
  lv_obj_add_event_cb (ConfirmBtn, OnPopupKey, LV_EVENT_KEY, NULL);
  lv_group_add_obj (Group, ConfirmBtn);
  lv_group_focus_obj (ConfirmBtn);
  mPopupFirstObj = ConfirmBtn;

  //
  // Optional Discard button (shown for "unsaved changes" popup only).
  //
  if (ShowDiscard) {
    DiscardBtn = lv_btn_create (BtnRow);
    Lbl        = lv_label_create (DiscardBtn);
    lv_label_set_text (Lbl, "Discard");
    lv_obj_add_event_cb (DiscardBtn, OnPopupBtn, LV_EVENT_CLICKED, &mDiscardAction);
    lv_obj_add_event_cb (DiscardBtn, OnPopupKey, LV_EVENT_KEY, NULL);
    lv_group_add_obj (Group, DiscardBtn);
    mPopupLastObj = DiscardBtn;
  }

  //
  // Cancel button.
  //
  CancelBtn = lv_btn_create (BtnRow);
  Lbl       = lv_label_create (CancelBtn);
  lv_label_set_text (Lbl, "Cancel");
  lv_obj_add_event_cb (CancelBtn, OnPopupBtn, LV_EVENT_CLICKED, &mNoneAction);
  lv_obj_add_event_cb (CancelBtn, OnPopupKey, LV_EVENT_KEY, NULL);
  lv_group_add_obj (Group, CancelBtn);
  mPopupLastObj = CancelBtn;
}

/**
  Walk HotKeyListHead for an F-key match and set UserInput->Action if found.
  Returns TRUE if a hotkey was matched and the event loop should exit.
**/
STATIC
BOOLEAN
HandleFunctionKey (
  IN UINT32  LvKey
  )
{
  UINT16           ScanCode;
  LIST_ENTRY       *Link;
  BROWSER_HOT_KEY  *HotKey;

  if ((LvKey < LV_KEY_F1) || (LvKey > LV_KEY_F12)) {
    return FALSE;
  }

  if (IsListEmpty (&mSession.FormData->HotKeyListHead)) {
    return FALSE;
  }

  ScanCode = (UINT16)(SCAN_F1 + (LvKey - LV_KEY_F1));

  for (Link = mSession.FormData->HotKeyListHead.ForwardLink;
       Link != &mSession.FormData->HotKeyListHead;
       Link = Link->ForwardLink)
  {
    HotKey = BROWSER_HOT_KEY_FROM_LINK (Link);
    if ((HotKey->KeyData != NULL) &&
        (HotKey->KeyData->ScanCode == ScanCode) &&
        (HotKey->KeyData->UnicodeChar == CHAR_NULL))
    {
      //
      // For actions that modify settings (Save / Load Defaults), show a
      // confirmation popup instead of immediately exiting. The main event
      // loop processes mPopupResult once the user dismisses the dialog.
      // For other actions (Reset, Exit) execute immediately.
      //
      if ((HotKey->Action & (BROWSER_ACTION_SUBMIT | BROWSER_ACTION_DEFAULT)) != 0) {
        if (mPopupOverlay == NULL) {
          CONST CHAR8  *Title = ((HotKey->Action & BROWSER_ACTION_DEFAULT) != 0)
                                  ? "Load Defaults?" : "Save Changes?";
          CONST CHAR8  *Label = ((HotKey->Action & BROWSER_ACTION_DEFAULT) != 0)
                                  ? "Load" : "Save";
          mPendingDefaultId = HotKey->DefaultId;
          ShowPopup (mSession.Group, Title, Label, HotKey->Action, FALSE);
        }
      } else {
        mSession.UserInput->Action            = HotKey->Action;
        mSession.UserInput->DefaultId         = HotKey->DefaultId;
        mSession.UserInput->SelectedStatement = NULL;
        mSession.ExitRequested                = TRUE;
      }

      return TRUE;
    }
  }

  return FALSE;
}

/**
  Indev-level ESC fallback — LVGL only routes LV_EVENT_KEY to a focused widget,
  so forms with no focusable items (e.g. an empty Driver Health Manager form)
  would eat ESC. This handler runs on every keypress the indev produces, and
  handles ESC whenever OnNavKey wouldn't fire because nothing is focused.
**/
STATIC
VOID
OnIndevFallbackKey (
  lv_event_t  *Event
  )
{
  lv_indev_t  *Indev;
  lv_key_t    Key;

  Indev = lv_indev_active ();
  Key   = lv_indev_get_key (Indev);

  //
  // Popup is open — OnPopupKey handles all keys for popup buttons.
  //
  if (mPopupOverlay != NULL) {
    return;
  }

  if ((mSession.Group != NULL) && (lv_group_get_focused (mSession.Group) != NULL)) {
    //
    // A widget has focus — OnNavKey will handle the key.
    //
    return;
  }

  if (Key == LV_KEY_ESC) {
    mSession.UserInput->Action            = BROWSER_ACTION_FORM_EXIT;
    mSession.UserInput->SelectedStatement = NULL;
    mSession.ExitRequested                = TRUE;
    return;
  }

  HandleFunctionKey (Key);
}

STATIC
VOID
GetNumericRange (
  IN  EFI_IFR_NUMERIC  *NumOp,
  OUT UINT64           *MinVal,
  OUT UINT64           *MaxVal
  )
{
  switch (NumOp->Flags & EFI_IFR_NUMERIC_SIZE) {
    case EFI_IFR_NUMERIC_SIZE_1:
      *MinVal = NumOp->data.u8.MinValue;
      *MaxVal = NumOp->data.u8.MaxValue;
      break;
    case EFI_IFR_NUMERIC_SIZE_2:
      *MinVal = NumOp->data.u16.MinValue;
      *MaxVal = NumOp->data.u16.MaxValue;
      break;
    case EFI_IFR_NUMERIC_SIZE_4:
      *MinVal = NumOp->data.u32.MinValue;
      *MaxVal = NumOp->data.u32.MaxValue;
      break;
    default:
      *MinVal = NumOp->data.u64.MinValue;
      *MaxVal = NumOp->data.u64.MaxValue;
      break;
  }
}

STATIC
UINT64
AsciiDecimalToUint64 (
  IN CONST CHAR8  *Str
  )
{
  UINT64  Value = 0;

  while ((*Str >= '0') && (*Str <= '9')) {
    Value = Value * 10 + (UINT64)(*Str - '0');
    Str++;
  }

  return Value;
}

STATIC
VOID
Uint64ToAsciiDecimal (
  IN  UINT64  Value,
  OUT CHAR8   *Buf,
  IN  UINTN   BufLen
  )
{
  CHAR8  Tmp[24];
  UINTN  i;
  UINTN  o;

  i = 0;
  if (Value == 0) {
    Tmp[i++] = '0';
  } else {
    while ((Value > 0) && (i < sizeof (Tmp))) {
      Tmp[i++] = (CHAR8)('0' + (Value % 10));
      Value   /= 10;
    }
  }

  o = 0;
  while ((i > 0) && (o + 1 < BufLen)) {
    Buf[o++] = Tmp[--i];
  }

  Buf[o] = '\0';
}

/**
  Numeric textarea commit — fires when the user presses ENTER on a one_line
  textarea (LV_EVENT_READY). Parses the typed text, clamps to the IFR
  Min/Max range, and delivers the new value to the browser.
**/
STATIC
VOID
OnNumericReady (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;
  lv_obj_t                *Ta;
  CONST CHAR8             *Text;
  UINT64                  Value;
  UINT64                  MinVal;
  UINT64                  MaxVal;
  EFI_IFR_NUMERIC         *NumOp;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  if ((Ctx == NULL) || (mSession.UserInput == NULL)) {
    return;
  }

  Ta    = lv_event_get_target_obj (Event);
  Text  = lv_textarea_get_text (Ta);
  Value = AsciiDecimalToUint64 (Text);

  NumOp = (EFI_IFR_NUMERIC *)Ctx->Statement->OpCode;
  GetNumericRange (NumOp, &MinVal, &MaxVal);

  if (Value < MinVal) {
    Value = MinVal;
  }

  if (Value > MaxVal) {
    Value = MaxVal;
  }

  mSession.UserInput->SelectedStatement = Ctx->Statement;
  mSession.UserInput->InputValue.Type   = Ctx->Statement->CurrentValue.Type;

  switch (NumOp->Flags & EFI_IFR_NUMERIC_SIZE) {
    case EFI_IFR_NUMERIC_SIZE_1:
      mSession.UserInput->InputValue.Value.u8 = (UINT8)Value;
      break;
    case EFI_IFR_NUMERIC_SIZE_2:
      mSession.UserInput->InputValue.Value.u16 = (UINT16)Value;
      break;
    case EFI_IFR_NUMERIC_SIZE_4:
      mSession.UserInput->InputValue.Value.u32 = (UINT32)Value;
      break;
    default:
      mSession.UserInput->InputValue.Value.u64 = Value;
      break;
  }

  mSession.UserInput->Action = 0;
  mSession.ExitRequested     = TRUE;
}

STATIC
VOID
OnNavKey (
  lv_event_t  *Event
  )
{
  lv_key_t                Key;
  bool                    Editing;
  lv_obj_t                *Focused;
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Key     = lv_indev_get_key (lv_indev_active ());
  Editing = lv_group_get_editing (mSession.Group);
  Ctx     = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);

  //
  // Defer all key handling to OnPopupKey when a popup is visible.
  //
  if (mPopupOverlay != NULL) {
    return;
  }

  if (Key == LV_KEY_ESC) {
    if (Editing) {
      lv_group_set_editing (mSession.Group, false);
    } else {
      mSession.UserInput->Action            = BROWSER_ACTION_FORM_EXIT;
      mSession.UserInput->SelectedStatement = NULL;
      mSession.ExitRequested                = TRUE;
    }

    lv_event_stop_processing (Event);
    return;
  }

  if (HandleFunctionKey (Key)) {
    lv_event_stop_processing (Event);
    return;
  }

  if (Editing) {
    return;
  }

  (void)Ctx;

  if (Key == LV_KEY_UP) {
    lv_group_focus_prev (mSession.Group);
    lv_event_stop_processing (Event);
  } else if (Key == LV_KEY_DOWN) {
    lv_group_focus_next (mSession.Group);
    lv_event_stop_processing (Event);
  } else if (Key == LV_KEY_ENTER) {
    Focused = lv_group_get_focused (mSession.Group);
    if ((Focused != NULL) &&
        (lv_obj_check_type (Focused, &lv_dropdown_class) ||
         lv_obj_check_type (Focused, &lv_spinbox_class)))
    {
      lv_group_set_editing (mSession.Group, true);
      lv_event_stop_processing (Event);
    }
    //
    // Textarea: do NOT intercept — Enter passes to the class handler,
    // which fires LV_EVENT_READY → OnStringReady commits the value.
    //
  }
}

STATIC
VOID
AddToNavGroup (
  lv_group_t              *Group,
  lv_obj_t                *Widget,
  LVGL_STATEMENT_CONTEXT  *Ctx
  )
{
  lv_group_add_obj (Group, Widget);
  lv_obj_add_event_cb (Widget, OnNavKey, LV_EVENT_KEY | LV_EVENT_PREPROCESS, Ctx);
}

//
// ---- Widget builders ----
//

STATIC
VOID
CreateSubtitleWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle
  )
{
  CHAR8     *Text;
  lv_obj_t  *Label;

  Text = GetPromptUtf8 (Statement, HiiHandle);
  if (Text == NULL) {
    return;
  }

  Label = lv_label_create (Parent);
  lv_label_set_text (Label, Text);
  lv_obj_set_style_text_font (Label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color (Label, lv_palette_main (LV_PALETTE_BLUE), 0);
  lv_obj_set_style_pad_top (Label, 8, 0);

  FreePool (Text);
}

STATIC
VOID
CreateTextWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle
  )
{
  CHAR8     *Text;
  lv_obj_t  *Label;

  Text = GetPromptUtf8 (Statement, HiiHandle);
  if (Text == NULL) {
    return;
  }

  Label = lv_label_create (Parent);
  lv_label_set_text (Label, Text);

  FreePool (Text);
}

STATIC
VOID
CreateCheckboxWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle,
  lv_group_t                      *Group
  )
{
  CHAR8                   *Text;
  lv_obj_t                *Cb;
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Cb = lv_checkbox_create (Parent);
  lv_checkbox_set_text (Cb, Text != NULL ? Text : "Checkbox");

  if (Statement->CurrentValue.Value.b) {
    lv_obj_add_state (Cb, LV_STATE_CHECKED);
  }

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Cb, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Cb;
    lv_obj_add_event_cb (Cb, OnCheckboxChanged, LV_EVENT_VALUE_CHANGED, Ctx);
  }

  AddToNavGroup (Group, Cb, Ctx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

STATIC
VOID
CreateNumericWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle,
  lv_group_t                      *Group
  )
{
  CHAR8                   *Text;
  lv_obj_t                *Row;
  lv_obj_t                *Label;
  lv_obj_t                *Ta;
  EFI_IFR_NUMERIC         *NumOp;
  LVGL_STATEMENT_CONTEXT  *Ctx;
  UINT64                  CurVal;
  UINT64                  MinVal;
  UINT64                  MaxVal;
  CHAR8                   Initial[24];
  CHAR8                   MaxStr[24];

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Row = lv_obj_create (Parent);
  lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all (Row, 4, 0);
  lv_obj_set_style_border_width (Row, 0, 0);
  lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "Numeric");
  lv_obj_set_flex_grow (Label, 1);

  NumOp = (EFI_IFR_NUMERIC *)Statement->OpCode;
  GetNumericRange (NumOp, &MinVal, &MaxVal);

  switch (NumOp->Flags & EFI_IFR_NUMERIC_SIZE) {
    case EFI_IFR_NUMERIC_SIZE_1:
      CurVal = Statement->CurrentValue.Value.u8;
      break;
    case EFI_IFR_NUMERIC_SIZE_2:
      CurVal = Statement->CurrentValue.Value.u16;
      break;
    case EFI_IFR_NUMERIC_SIZE_4:
      CurVal = Statement->CurrentValue.Value.u32;
      break;
    default:
      CurVal = Statement->CurrentValue.Value.u64;
      break;
  }

  //
  // A one-line textarea accepting only digits. Pressing ENTER fires
  // LV_EVENT_READY, which OnNumericReady uses to commit.
  //
  Ta = lv_textarea_create (Row);
  lv_textarea_set_one_line (Ta, true);
  lv_textarea_set_accepted_chars (Ta, "0123456789");

  Uint64ToAsciiDecimal (MaxVal, MaxStr, sizeof (MaxStr));
  lv_textarea_set_max_length (Ta, (uint32_t)AsciiStrLen (MaxStr));

  Uint64ToAsciiDecimal (CurVal, Initial, sizeof (Initial));
  lv_textarea_set_text (Ta, Initial);

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Ta, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Ta;
    lv_obj_add_event_cb (Ta, OnNumericReady, LV_EVENT_READY, Ctx);
  }

  AddToNavGroup (Group, Ta, Ctx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

STATIC
VOID
CreateOneOfWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle,
  lv_group_t                      *Group
  )
{
  CHAR8                     *Text;
  lv_obj_t                  *Row;
  lv_obj_t                  *Label;
  lv_obj_t                  *Dd;
  LIST_ENTRY                *Link;
  DISPLAY_QUESTION_OPTION   *Option;
  CHAR8                     OptBuf[512];
  UINTN                     OptLen;
  CHAR16                    *OptStr16;
  CHAR8                     *OptStr8;
  UINT32                    SelectedIdx;
  UINT32                    CurIdx;
  LVGL_STATEMENT_CONTEXT    *Ctx;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Row = lv_obj_create (Parent);
  lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all (Row, 4, 0);
  lv_obj_set_style_border_width (Row, 0, 0);
  lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "OneOf");
  lv_obj_set_flex_grow (Label, 1);

  //
  // Build newline-separated option string for LVGL dropdown.
  //
  OptLen      = 0;
  OptBuf[0]   = '\0';
  SelectedIdx = 0;
  CurIdx      = 0;

  for (Link = Statement->OptionListHead.ForwardLink;
       Link != &Statement->OptionListHead;
       Link = Link->ForwardLink)
  {
    Option   = DISPLAY_QUESTION_OPTION_FROM_LINK (Link);
    OptStr16 = HiiGetString (HiiHandle, Option->OptionOpCode->Option, NULL);
    if (OptStr16 != NULL) {
      OptStr8 = Ucs2ToUtf8 (OptStr16);
      if (OptStr8 != NULL) {
        UINTN  ItemLen = AsciiStrLen (OptStr8);
        if (OptLen + ItemLen + 2 < sizeof (OptBuf)) {
          if (OptLen > 0) {
            OptBuf[OptLen++] = '\n';
          }

          AsciiStrCpyS (&OptBuf[OptLen], sizeof (OptBuf) - OptLen, OptStr8);
          OptLen += ItemLen;
        }

        FreePool (OptStr8);
      }

      FreePool (OptStr16);
    }

    //
    // Match based on the type-appropriate field only.  The IFR binary stores
    // EFI_IFR_ONE_OF_OPTION.Value at its native width, so reading the full
    // sizeof(EFI_IFR_TYPE_VALUE) union would read into adjacent opcodes.
    //
    {
      BOOLEAN  Match;

      switch (Option->OptionOpCode->Type) {
        case EFI_IFR_TYPE_NUM_SIZE_8:
          Match = (Option->OptionOpCode->Value.u8 == Statement->CurrentValue.Value.u8);
          break;
        case EFI_IFR_TYPE_NUM_SIZE_16:
          Match = (Option->OptionOpCode->Value.u16 == Statement->CurrentValue.Value.u16);
          break;
        case EFI_IFR_TYPE_NUM_SIZE_32:
          Match = (Option->OptionOpCode->Value.u32 == Statement->CurrentValue.Value.u32);
          break;
        case EFI_IFR_TYPE_NUM_SIZE_64:
          Match = (Option->OptionOpCode->Value.u64 == Statement->CurrentValue.Value.u64);
          break;
        default:
          Match = FALSE;
          break;
      }

      if (Match) {
        SelectedIdx = CurIdx;
      }
    }

    CurIdx++;
  }

  Dd = lv_dropdown_create (Row);
  lv_dropdown_set_options (Dd, OptBuf);
  lv_dropdown_set_selected (Dd, SelectedIdx);

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Dd, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Dd;
    lv_obj_add_event_cb (Dd, OnDropdownChanged, LV_EVENT_VALUE_CHANGED, Ctx);
  }

  AddToNavGroup (Group, Dd, Ctx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

STATIC
VOID
CreateOrderedListWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                  HiiHandle,
  lv_group_t                      *Group
  )
{
  CHAR8                    *PromptText;
  lv_obj_t                 *Panel;
  lv_obj_t                 *Header;
  lv_obj_t                 *Row;
  lv_obj_t                 *Label;
  lv_obj_t                 *UpBtn;
  lv_obj_t                 *DownBtn;
  lv_obj_t                 *BtnLabel;
  LIST_ENTRY               *Link;
  DISPLAY_QUESTION_OPTION  *FirstOption;
  DISPLAY_QUESTION_OPTION  *Option;
  EFI_IFR_ORDERED_LIST     *OrderOp;
  UINT8                    ValueType;
  UINTN                    MaxContainers;
  UINTN                    ActiveCount;
  UINTN                    i;
  UINT64                   Value;
  CHAR16                   *OptStr16;
  CHAR8                    *OptStr8;
  LVGL_ORDERED_MOVE_CTX    *UpCtx;
  LVGL_ORDERED_MOVE_CTX    *DownCtx;
  BOOLEAN                  Grayout;

  if (IsListEmpty (&Statement->OptionListHead)) {
    return;
  }

  OrderOp       = (EFI_IFR_ORDERED_LIST *)Statement->OpCode;
  MaxContainers = OrderOp->MaxContainers;
  Grayout       = (BOOLEAN)((Statement->Attribute & HII_DISPLAY_GRAYOUT) != 0);

  Link        = GetFirstNode (&Statement->OptionListHead);
  FirstOption = DISPLAY_QUESTION_OPTION_FROM_LINK (Link);
  ValueType   = FirstOption->OptionOpCode->Type;

  //
  // Determine number of active (non-zero) entries, bounded by MaxContainers.
  //
  ActiveCount = 0;
  for (i = 0; i < MaxContainers; i++) {
    if (GetArrayData (Statement->CurrentValue.Buffer, ValueType, i) == 0) {
      break;
    }
    ActiveCount++;
  }

  PromptText = GetPromptUtf8 (Statement, HiiHandle);

  //
  // Outer panel — vertical flex column holding header + one row per entry.
  //
  Panel = lv_obj_create (Parent);
  lv_obj_set_size (Panel, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all (Panel, 4, 0);
  lv_obj_set_style_border_width (Panel, 0, 0);
  lv_obj_set_style_bg_opa (Panel, LV_OPA_TRANSP, 0);

  Header = lv_label_create (Panel);
  lv_label_set_text (Header, PromptText != NULL ? PromptText : "Ordered List");
  lv_obj_set_style_text_font (Header, &lv_font_montserrat_16, 0);

  for (i = 0; i < ActiveCount; i++) {
    Value = GetArrayData (Statement->CurrentValue.Buffer, ValueType, i);

    //
    // Find the option whose Value matches this slot. Read the option's
    // Value at its native width — IFR stores it at ValueType size, so
    // reading .u64 would over-read into neighboring bytes.
    //
    OptStr8 = NULL;
    for (Link = Statement->OptionListHead.ForwardLink;
         Link != &Statement->OptionListHead;
         Link = Link->ForwardLink)
    {
      UINT64  OptValue;

      Option = DISPLAY_QUESTION_OPTION_FROM_LINK (Link);
      switch (ValueType) {
        case EFI_IFR_TYPE_NUM_SIZE_8:
          OptValue = Option->OptionOpCode->Value.u8;
          break;
        case EFI_IFR_TYPE_NUM_SIZE_16:
          OptValue = Option->OptionOpCode->Value.u16;
          break;
        case EFI_IFR_TYPE_NUM_SIZE_32:
          OptValue = Option->OptionOpCode->Value.u32;
          break;
        case EFI_IFR_TYPE_NUM_SIZE_64:
          OptValue = Option->OptionOpCode->Value.u64;
          break;
        default:
          OptValue = 0;
          break;
      }

      if (OptValue == Value) {
        OptStr16 = HiiGetString (HiiHandle, Option->OptionOpCode->Option, NULL);
        if (OptStr16 != NULL) {
          OptStr8 = Ucs2ToUtf8 (OptStr16);
          FreePool (OptStr16);
        }

        break;
      }
    }

    Row = lv_obj_create (Panel);
    lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all (Row, 2, 0);
    lv_obj_set_style_border_width (Row, 0, 0);
    lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);

    Label = lv_label_create (Row);
    lv_label_set_text (Label, OptStr8 != NULL ? OptStr8 : "?");
    lv_obj_set_flex_grow (Label, 1);

    if (OptStr8 != NULL) {
      FreePool (OptStr8);
    }

    //
    // Up button.
    //
    UpBtn    = lv_btn_create (Row);
    BtnLabel = lv_label_create (UpBtn);
    lv_label_set_text (BtnLabel, LV_SYMBOL_UP);

    if (Grayout || (i == 0)) {
      lv_obj_add_state (UpBtn, LV_STATE_DISABLED);
    } else {
      UpCtx = AllocateZeroPool (sizeof (LVGL_ORDERED_MOVE_CTX));
      if (UpCtx != NULL) {
        UpCtx->Statement   = Statement;
        UpCtx->ValueType   = ValueType;
        UpCtx->ActiveCount = ActiveCount;
        UpCtx->Index       = i;
        UpCtx->Direction   = -1;
        lv_obj_add_event_cb (UpBtn, OnOrderedListMove, LV_EVENT_CLICKED, UpCtx);
      }

      AddToNavGroup (Group, UpBtn, NULL);
    }

    //
    // Down button.
    //
    DownBtn  = lv_btn_create (Row);
    BtnLabel = lv_label_create (DownBtn);
    lv_label_set_text (BtnLabel, LV_SYMBOL_DOWN);

    if (Grayout || (i + 1 >= ActiveCount)) {
      lv_obj_add_state (DownBtn, LV_STATE_DISABLED);
    } else {
      DownCtx = AllocateZeroPool (sizeof (LVGL_ORDERED_MOVE_CTX));
      if (DownCtx != NULL) {
        DownCtx->Statement   = Statement;
        DownCtx->ValueType   = ValueType;
        DownCtx->ActiveCount = ActiveCount;
        DownCtx->Index       = i;
        DownCtx->Direction   = 1;
        lv_obj_add_event_cb (DownBtn, OnOrderedListMove, LV_EVENT_CLICKED, DownCtx);
      }

      AddToNavGroup (Group, DownBtn, NULL);
    }
  }

  if (PromptText != NULL) {
    FreePool (PromptText);
  }
}

STATIC
VOID
CreateStringWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle,
  lv_group_t                      *Group
  )
{
  CHAR8                   *Text;
  CHAR16                  *CurStr16;
  CHAR8                   *CurUtf8;
  lv_obj_t                *Row;
  lv_obj_t                *Label;
  lv_obj_t                *Ta;
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Row = lv_obj_create (Parent);
  lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all (Row, 4, 0);
  lv_obj_set_style_border_width (Row, 0, 0);
  lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "String");
  lv_obj_set_flex_grow (Label, 1);

  Ta = lv_textarea_create (Row);
  lv_textarea_set_one_line (Ta, true);

  if (Statement->OpCode->OpCode == EFI_IFR_PASSWORD_OP) {
    lv_textarea_set_password_mode (Ta, true);
  } else {
    //
    // For string fields the current value is stored as an HII string token
    // in CurrentValue.Value.string — NOT in CurrentValue.Buffer (which is
    // used only for buffer-type questions such as ordered lists).
    //
    if (Statement->CurrentValue.Value.string != 0) {
      CurStr16 = HiiGetString (HiiHandle, Statement->CurrentValue.Value.string, NULL);
      if (CurStr16 != NULL) {
        CurUtf8 = Ucs2ToUtf8 (CurStr16);
        FreePool (CurStr16);
        if (CurUtf8 != NULL) {
          lv_textarea_set_text (Ta, CurUtf8);
          FreePool (CurUtf8);
        }
      }
    }
  }

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Ta, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Ta;
    Ctx->HiiHandle = HiiHandle;
    lv_obj_add_event_cb (Ta, OnStringReady, LV_EVENT_READY, Ctx);
  }

  AddToNavGroup (Group, Ta, Ctx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

STATIC
VOID
CreateRefWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle,
  lv_group_t                      *Group
  )
{
  CHAR8                   *Text;
  lv_obj_t                *Btn;
  lv_obj_t                *Label;
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Btn   = lv_btn_create (Parent);
  Label = lv_label_create (Btn);
  lv_label_set_text (Label, Text != NULL ? Text : "Goto");
  lv_obj_set_width (Btn, LV_PCT (100));

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Btn, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Btn;
    lv_obj_add_event_cb (Btn, OnStatementClicked, LV_EVENT_CLICKED, Ctx);
  }

  AddToNavGroup (Group, Btn, Ctx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

STATIC
VOID
CreateActionWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle,
  lv_group_t                      *Group
  )
{
  //
  // Action buttons look identical to Ref (goto) buttons from the UI side.
  //
  CreateRefWidget (Parent, Statement, HiiHandle, Group);
}

//
// ---- Core renderer ----
//

/**
  Walk StatementListHead and create one LVGL widget per visible statement.
**/
STATIC
VOID
BuildFormWidgets (
  IN LVGL_FORM_SESSION  *Session
  )
{
  LIST_ENTRY                       *Link;
  FORM_DISPLAY_ENGINE_STATEMENT    *Statement;
  EFI_HII_HANDLE                  HiiHandle;

  HiiHandle = Session->FormData->HiiHandle;

  for (Link = Session->FormData->StatementListHead.ForwardLink;
       Link != &Session->FormData->StatementListHead;
       Link = Link->ForwardLink)
  {
    Statement = FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK (Link);

    //
    // Skip suppressed statements.
    //
    if (Statement->Attribute & HII_DISPLAY_SUPPRESS) {
      continue;
    }

    switch (Statement->OpCode->OpCode) {
      case EFI_IFR_SUBTITLE_OP:
        CreateSubtitleWidget (Session->Screen, Statement, HiiHandle);
        break;

      case EFI_IFR_TEXT_OP:
        CreateTextWidget (Session->Screen, Statement, HiiHandle);
        break;

      case EFI_IFR_CHECKBOX_OP:
        CreateCheckboxWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      case EFI_IFR_NUMERIC_OP:
        CreateNumericWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      case EFI_IFR_ONE_OF_OP:
        CreateOneOfWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      case EFI_IFR_ORDERED_LIST_OP:
        CreateOrderedListWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      case EFI_IFR_STRING_OP:
      case EFI_IFR_PASSWORD_OP:
        CreateStringWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      case EFI_IFR_REF_OP:
        CreateRefWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      case EFI_IFR_ACTION_OP:
        CreateActionWidget (Session->Screen, Statement, HiiHandle, Session->Group);
        break;

      default:
        DEBUG ((DEBUG_VERBOSE, "LvglRenderer: skipping opcode 0x%02x\n", Statement->OpCode->OpCode));
        break;
    }
  }
}

EFI_STATUS
EFIAPI
LvglRenderForm (
  IN  FORM_DISPLAY_ENGINE_FORM  *FormData,
  OUT USER_INPUT                *UserInputData
  )
{
  EFI_STATUS  Status;
  CHAR16      *TitleStr16;
  CHAR8       *TitleStr8;
  lv_obj_t    *TitleLabel;
  lv_obj_t    *ContentPanel;
  extern BOOLEAN mTickSupport;

  ASSERT (FormData != NULL);
  ASSERT (UserInputData != NULL);

  //
  // Initialize LVGL if not yet done (constructor in LvglLib handles this,
  // but call explicitly to be safe).
  //
  if (!mLvglReady) {
    Status = UefiLvglInit ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "LvglRenderer: UefiLvglInit failed — %r\n", Status));
      return Status;
    }

    mLvglReady = TRUE;
  }

  //
  // Clear the console and hide cursor for graphical mode.
  //
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  //
  // Set up session state.
  //
  ZeroMem (&mSession, sizeof (mSession));
  mSession.FormData       = FormData;
  mSession.UserInput      = UserInputData;
  mSession.ExitRequested  = FALSE;

  ZeroMem (UserInputData, sizeof (USER_INPUT));

  //
  // Create a new screen.
  //
  mSession.Screen = lv_obj_create (NULL);
  lv_obj_set_style_bg_color (mSession.Screen, lv_color_hex (0x1A1A2E), 0);
  lv_obj_set_flex_flow (mSession.Screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all (mSession.Screen, 16, 0);
  lv_obj_set_style_pad_row (mSession.Screen, 6, 0);

  //
  // Title bar.
  //
  TitleStr16 = HiiGetString (FormData->HiiHandle, FormData->FormTitle, NULL);
  TitleStr8  = Ucs2ToUtf8 (TitleStr16 != NULL ? TitleStr16 : L"Setup");
  if (TitleStr16 != NULL) {
    FreePool (TitleStr16);
  }

  TitleLabel = lv_label_create (mSession.Screen);
  lv_label_set_text (TitleLabel, TitleStr8 != NULL ? TitleStr8 : "Setup");
  lv_obj_set_style_text_font (TitleLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color (TitleLabel, lv_color_white (), 0);
  if (TitleStr8 != NULL) {
    FreePool (TitleStr8);
  }

  //
  // Scrollable content area.
  //
  ContentPanel = lv_obj_create (mSession.Screen);
  lv_obj_set_size (ContentPanel, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (ContentPanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow (ContentPanel, 1);
  lv_obj_set_style_pad_all (ContentPanel, 8, 0);
  lv_obj_set_style_pad_row (ContentPanel, 4, 0);
  lv_obj_set_style_bg_color (ContentPanel, lv_color_hex (0x16213E), 0);
  lv_obj_set_style_radius (ContentPanel, 8, 0);

  //
  // Create a navigation group and bind keyboard input devices.
  //
  mSession.Group = lv_group_create ();
  {
    lv_indev_t  *Indev = NULL;

    for (;;) {
      Indev = lv_indev_get_next (Indev);
      if (Indev == NULL) {
        break;
      }

      if (lv_indev_get_type (Indev) == LV_INDEV_TYPE_KEYPAD) {
        STATIC BOOLEAN  mFallbackInstalled = FALSE;
        lv_indev_set_group (Indev, mSession.Group);
        //
        // LvglRenderForm is called once per form transition. Only install the
        // indev fallback key handler the first time, otherwise stale callbacks
        // accumulate for the lifetime of LVGL.
        //
        if (!mFallbackInstalled) {
          lv_indev_add_event_cb (Indev, OnIndevFallbackKey, LV_EVENT_KEY, NULL);
          mFallbackInstalled = TRUE;
        }
      }
    }
  }

  //
  // Temporarily swap mSession.Screen to use ContentPanel for widget creation,
  // so widgets go inside the scrollable panel.
  //
  {
    lv_obj_t  *OrigScreen = mSession.Screen;

    mSession.Screen = ContentPanel;
    BuildFormWidgets (&mSession);
    mSession.Screen = OrigScreen;
  }

  //
  // Load the screen.
  //
  lv_screen_load (mSession.Screen);

  //
  // LVGL event loop — run until user makes a selection or presses ESC.
  //
  DEBUG ((DEBUG_INFO, "LvglRenderer: entering event loop for FormId=0x%x\n", FormData->FormId));

  while (!mSession.ExitRequested) {
    lv_timer_handler ();
    gBS->Stall (10 * 1000);  // 10 ms
    if (!mTickSupport) {
      lv_tick_inc (10);
    }

    //
    // Process popup result once the overlay has been dismissed.
    //
    if ((mPopupResult != LVGL_POPUP_PENDING) && (mPopupOverlay == NULL)) {
      if (mPopupResult != BROWSER_ACTION_NONE) {
        mSession.UserInput->Action            = mPopupResult;
        mSession.UserInput->DefaultId         = mPendingDefaultId;
        mSession.UserInput->SelectedStatement = NULL;
        mSession.ExitRequested                = TRUE;
      }

      mPopupResult = LVGL_POPUP_PENDING;
    }
  }

  //
  // We exit on a key PRESS event (ENTER to commit, ESC to leave). LVGL
  // hasn't yet seen the matching RELEASE — without draining, LVGL would
  // deliver the pending LV_EVENT_CLICKED to whatever widget is focused when
  // FormDisplay() is re-invoked, triggering a spurious submenu navigation.
  // lv_indev_wait_release() is NOT suitable here because it eats the entire
  // next keystroke (it waits for a RELEASED poll, which swallows any new
  // key the user presses before an idle poll).
  //
  lv_uefi_keypad_drain ();

  DEBUG ((DEBUG_INFO, "LvglRenderer: exiting event loop — Action=0x%x\n", UserInputData->Action));

  return EFI_SUCCESS;
}

UINTN
EFIAPI
LvglRunConfirmPopup (
  VOID
  )
{
  lv_group_t  *PopupGroup;
  lv_indev_t  *Indev;
  extern BOOLEAN  mTickSupport;

  if (!mLvglReady) {
    return BROWSER_ACTION_DISCARD;
  }

  //
  // Create a dedicated navigation group for the popup so that the keyboard
  // indev is isolated from the (now hidden) form group.
  //
  PopupGroup = lv_group_create ();

  Indev = NULL;
  while ((Indev = lv_indev_get_next (Indev)) != NULL) {
    if (lv_indev_get_type (Indev) == LV_INDEV_TYPE_KEYPAD) {
      lv_indev_set_group (Indev, PopupGroup);
    }
  }

  ShowPopup (PopupGroup, "Unsaved Changes", "Save", BROWSER_ACTION_SUBMIT, TRUE);

  //
  // Drain pending keystrokes so the popup isn't dismissed accidentally.
  //
  lv_uefi_keypad_drain ();

  while (mPopupResult == LVGL_POPUP_PENDING) {
    lv_timer_handler ();
    gBS->Stall (10 * 1000);
    if (!mTickSupport) {
      lv_tick_inc (10);
    }
  }

  //
  // Restore keyboard indev to the form group (still valid — ExitDisplay
  // has not been called yet at this point in the browser flow).
  //
  Indev = NULL;
  while ((Indev = lv_indev_get_next (Indev)) != NULL) {
    if (lv_indev_get_type (Indev) == LV_INDEV_TYPE_KEYPAD) {
      lv_indev_set_group (Indev, mSession.Group);
    }
  }

  lv_group_delete (PopupGroup);

  {
    UINTN  Result  = (UINTN)mPopupResult;
    mPopupResult   = LVGL_POPUP_PENDING;
    return Result;
  }
}

VOID
EFIAPI
LvglRendererCleanup (
  VOID
  )
{
  if (mSession.Group != NULL) {
    lv_group_delete (mSession.Group);
    mSession.Group = NULL;
  }

  if (mSession.Screen != NULL) {
    lv_obj_delete (mSession.Screen);
    mSession.Screen = NULL;
  }

  //
  // Note: LVGL_STATEMENT_CONTEXT objects were allocated with AllocateZeroPool
  // and are cleaned up when the screen is deleted (LVGL deletes children
  // recursively). However, user_data pointers are NOT freed by LVGL.
  // For a production implementation, we'd track and free them.
  // For now this is acceptable as the browser re-calls FormDisplay() in a loop
  // and the DXE pool is reclaimed on reboot.
  //
}
