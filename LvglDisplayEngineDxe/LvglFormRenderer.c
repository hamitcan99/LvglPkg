/** @file
  LVGL Form Renderer -- creates LVGL widgets from FORM_DISPLAY_ENGINE_FORM.

  Walks the StatementListHead provided by SetupBrowserDxe and maps each
  IFR opcode to a corresponding LVGL widget. Runs the LVGL event loop
  until the user selects a question or presses ESC, then fills USER_INPUT
  for the browser.

  Copyright (c) 2024-2026, Hamit Karaca. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "LvglFormRenderer.h"
#include "LvglAptioChrome.h"
#include <LvglTheme.h>
#include <Library/LvglUiConfigLib.h>
#include <Library/PrintLib.h>
#include <Guid/MdeModuleHii.h>

STATIC LVGL_FORM_SESSION  mSession;
STATIC BOOLEAN            mLvglReady = FALSE;

//
// Custom navigation list. lv_group_focus_next/prev skip widgets that have
// LV_STATE_DISABLED, which would make grayed-out questions un-reachable
// by keyboard. We keep a parallel ordered list of every nav-eligible widget
// so UP/DOWN can step onto disabled rows too (read-only focus).
//
#define LVGL_NAV_MAX  256
STATIC lv_obj_t                       *mNavList[LVGL_NAV_MAX];
STATIC FORM_DISPLAY_ENGINE_STATEMENT  *mNavStatement[LVGL_NAV_MAX];
STATIC UINTN                          mNavCount = 0;

//
// Scrollable form rows panel from AptioBuildChrome(). Used to restore the top
// of the view (banner / labels above the first controls) when focus returns
// to rows in the upper portion of the form.
//
STATIC lv_obj_t  *mContentPanel = NULL;

//
// Loaded from LvglUiScale each FormDisplay(); mirrors SubtitleShowsDeviceModel.
//
STATIC BOOLEAN  mSubtitleShowsDeviceModel = FALSE;

//
// Editable field registry -- commit dirty values when leaving a field via
// Tab/arrows or when exiting the form (ESC), not only on Enter.
//
typedef struct {
  FORM_DISPLAY_ENGINE_STATEMENT  *Statement;
  BOOLEAN                        IsDate;
  lv_obj_t                       *Field0;
  lv_obj_t                       *Field1;
  lv_obj_t                       *Field2;
} LVGL_DATETIME_CTX;

typedef enum {
  LVGL_EDIT_NUMERIC,
  LVGL_EDIT_STRING,
  LVGL_EDIT_DATETIME,
} LVGL_EDIT_KIND;

typedef struct {
  LVGL_EDIT_KIND  Kind;
  union {
    LVGL_STATEMENT_CONTEXT  *TextCtx;
    LVGL_DATETIME_CTX       *DateTime;
  } u;
} LVGL_EDIT_ENTRY;

#define LVGL_EDIT_MAX  64

STATIC LVGL_EDIT_ENTRY  mEditRegistry[LVGL_EDIT_MAX];
STATIC UINTN            mEditRegistryCount = 0;

//
// After a nav-time commit, SetupBrowser highlights the edited question.
// Prefer the intended destination nav index on the next FormDisplay.
//
STATIC BOOLEAN  mRestoreFocusPending = FALSE;
STATIC UINTN    mRestoreFocusNavIdx  = 0;

//
// Deferred exit/submenu click after committing the focused dirty field once.
//
typedef struct {
  BOOLEAN              Active;
  UINT32               Action;
  UINT16               DefaultId;
  EFI_IFR_OP_HEADER    *OpCode;
} LVGL_PENDING_LEAVE;

STATIC LVGL_PENDING_LEAVE  mPendingLeave;

//
// Live banner refresh. Front-page banner lines (e.g. the battery status) are
// updated in place by their producer via HiiSetString while the form stays
// open. The banner labels are built once, so we poll their HII strings on a
// timer and update the labels when the text changes -- otherwise the graphical
// banner would show a stale value.
//
#define LVGL_BANNER_MAX  16
typedef struct {
  lv_obj_t        *Label;
  EFI_HII_HANDLE  HiiHandle;
  EFI_STRING_ID   StringId;
} LVGL_BANNER_ENTRY;

STATIC LVGL_BANNER_ENTRY  mBannerRegistry[LVGL_BANNER_MAX];
STATIC UINTN              mBannerCount = 0;
STATIC lv_timer_t         *mBannerTimer = NULL;

//
// Keyboard-edit tracking for ONE_OF dropdowns.
// When ENTER enters editing, we snapshot the selection so ESC can revert it.
// Only one dropdown can be keyboard-edited at a time.
//
STATIC lv_obj_t  *mEditingDropdown        = NULL;
STATIC UINT32     mEditingDropdownOrigSel = 0;

//
// Popup state -- shared by the F10 in-loop overlay and LvglRunConfirmPopup.
//
#define LVGL_POPUP_PENDING  0xFFFFFFFFU

STATIC UINT32     mPopupResult       = LVGL_POPUP_PENDING;
STATIC lv_obj_t  *mPopupOverlay      = NULL;
STATIC lv_obj_t  *mPopupFirstObj     = NULL;
STATIC lv_obj_t  *mPopupLastObj      = NULL;
STATIC UINT32     mPopupConfirmAction = 0;
STATIC UINT16     mPendingDefaultId   = 0;
STATIC UINT32     mDiscardAction      = BROWSER_ACTION_DISCARD;
STATIC BOOLEAN    mPopupHasDiscard    = FALSE;
STATIC lv_obj_t   *mPopupPrevFocus    = NULL;
STATIC UINT32     mNoneAction         = BROWSER_ACTION_NONE;

//
// EFI_HII_POPUP_PROTOCOL popup state
// Allows a driver callback to raise a modal Yes/No while a form is displayed.
//
#define LVGL_HII_POPUP_PENDING  (-1)
STATIC INT32     mHiiPopupSel     = LVGL_HII_POPUP_PENDING;
STATIC lv_obj_t  *mHiiPopupOverlay = NULL;
STATIC lv_obj_t  *mHiiPopupFirst   = NULL;
STATIC lv_obj_t  *mHiiPopupLast    = NULL;
STATIC INT32       mHiiPopupYesSel = LVGL_HII_POPUP_PENDING;
STATIC INT32       mHiiPopupNoSel  = LVGL_HII_POPUP_PENDING;
STATIC INT32       mHiiPopupEscSel = LVGL_HII_POPUP_PENDING;
STATIC lv_group_t  *mHiiPopupGroup = NULL;

//
// UiApp front-page formset GUID
//
STATIC CONST EFI_GUID  mFrontPageFormSetGuid = {
  0x9e0c30bc, 0x3f06, 0x4ba6, { 0x82, 0x88, 0x09, 0x17, 0x9b, 0x85, 0x5d, 0xbe }
};

//
// Forward declarations for widget builders.
//
STATIC VOID ShowPopup (lv_group_t *Group, CONST CHAR8 *Title, CONST CHAR8 *ConfirmLabel, UINT32 ConfirmAction, BOOLEAN ShowDiscard);
STATIC VOID CreateSubtitleWidget      (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle);
STATIC VOID CreateTextWidget          (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle);
STATIC VOID CreateBannerWidget        (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle);
STATIC VOID CreateCheckboxWidget      (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateNumericWidget       (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateOneOfWidget         (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateOrderedListWidget   (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateStringWidget        (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateRefWidget           (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateActionWidget        (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group);
STATIC VOID CreateDateTimeWidget      (lv_obj_t *Parent, FORM_DISPLAY_ENGINE_STATEMENT *Statement, EFI_HII_HANDLE HiiHandle, lv_group_t *Group, BOOLEAN IsDate);
STATIC VOID RequestUserExit (IN UINT32 Action, IN UINT16 DefaultId, IN FORM_DISPLAY_ENGINE_STATEMENT *Statement OPTIONAL);
STATIC BOOLEAN CompletePendingLeaveIfReady (VOID);
STATIC BOOLEAN TryCommitFocusedThenNav (IN UINTN DestNavIdx);

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

/**
  Get help text for a statement as UTF-8. Caller must FreePool().
  STATEMENT_HEADER.Help is shared by every opcode that embeds it.
**/
STATIC
CHAR8 *
GetHelpUtf8 (
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement,
  IN EFI_HII_HANDLE                HiiHandle
  )
{
  EFI_STRING_ID  HelpId;
  CHAR16         *Str16;
  CHAR8          *Utf8;

  HelpId = ((EFI_IFR_SUBTITLE *)Statement->OpCode)->Statement.Help;
  if (HelpId == 0) {
    return NULL;
  }

  Str16 = HiiGetString (HiiHandle, HelpId, NULL);
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
  Verify that Statement is still present in the current FormData's
  StatementListHead. LVGL event callbacks can fire after the form has been
  rebuilt (e.g. while a previous form's widgets are still being torn down),
  in which case Ctx->Statement is dangling. Assigning a stale pointer to
  USER_INPUT.SelectedStatement causes SetupBrowserDxe's GetBrowserStatement
  to return NULL and assert at Presentation.c(1619).

  Match by OpCode -- the IFR opcode pointer is stable across browser
  FormDisplay rebuilds; the FORM_DISPLAY_ENGINE_STATEMENT wrapper is not.
**/
STATIC
FORM_DISPLAY_ENGINE_STATEMENT *
FindStatementByOpCode (
  IN EFI_IFR_OP_HEADER  *OpCode
  )
{
  LIST_ENTRY                     *Link;
  FORM_DISPLAY_ENGINE_STATEMENT  *Walk;

  if ((OpCode == NULL) || (mSession.FormData == NULL)) {
    return NULL;
  }

  for (Link = mSession.FormData->StatementListHead.ForwardLink;
       Link != &mSession.FormData->StatementListHead;
       Link = Link->ForwardLink)
  {
    Walk = FORM_DISPLAY_ENGINE_STATEMENT_FROM_LINK (Link);
    if (Walk->OpCode == OpCode) {
      return Walk;
    }
  }

  return NULL;
}

STATIC
BOOLEAN
IsStatementInCurrentForm (
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement
  )
{
  if (Statement == NULL) {
    return FALSE;
  }

  return FindStatementByOpCode (Statement->OpCode) == Statement;
}

/**
  Return TRUE when the form currently displayed is the UiApp front page.
  ESC is a no-op there (see mFrontPageFormSetGuid).
**/
STATIC
BOOLEAN
IsFrontPageForm (
  VOID
  )
{
  return (mSession.FormData != NULL) &&
         CompareGuid (&mSession.FormData->FormSetGuid, &mFrontPageFormSetGuid);
}

/**
  Generic click handler -- records the statement selection and requests exit.
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

  if (!IsStatementInCurrentForm (Ctx->Statement)) {
    return;
  }

  RequestUserExit (0, 0, Ctx->Statement);
}

/**
  Bottom-docked on-screen keyboard show/hide handlers. The keyboard is
  created once per form (in LvglRenderForm) and hidden by default. When a
  text-entry textarea gains focus we re-bind the keyboard to it and reveal
  it; on defocus we hide it again. lv_keyboard's default event callback
  forwards LV_EVENT_READY/CANCEL to the bound textarea, so the existing
  OnStringReady / OnNumericReady commit paths still fire -- we only need to
  manage visibility here.

  user_data on the textarea event: (uintptr_t)1 = numeric mode, else text.
**/
STATIC
VOID
OnTextareaFocused (
  lv_event_t  *Event
  )
{
  lv_obj_t  *Ta;
  UINTN     IsNumeric;

  if (mSession.OnScreenKb == NULL) {
    return;
  }

  //
  // The on-screen keyboard is a mouse-only affordance. On machines driven by a
  // physical keyboard the focused textarea already receives keystrokes through
  // the keypad indev, so suppress the pop-up keyboard when no mouse is present.
  //
  if (!lv_uefi_pointer_is_present ()) {
    return;
  }

  Ta        = lv_event_get_target_obj (Event);
  IsNumeric = (UINTN)lv_event_get_user_data (Event);

  lv_keyboard_set_textarea (mSession.OnScreenKb, Ta);
  lv_keyboard_set_mode (
    mSession.OnScreenKb,
    IsNumeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER
    );
  lv_obj_set_hidden (mSession.OnScreenKb, false);
  lv_obj_move_foreground (mSession.OnScreenKb);
  lv_obj_scroll_to_view (Ta, LV_ANIM_OFF);
}

STATIC
VOID
OnTextareaDefocused (
  lv_event_t  *Event
  )
{
  if (mSession.OnScreenKb == NULL) {
    return;
  }

  (VOID)Event;
  lv_obj_set_hidden (mSession.OnScreenKb, true);
  lv_keyboard_set_textarea (mSession.OnScreenKb, NULL);
}

STATIC
VOID
OnKeyboardCancel (
  lv_event_t  *Event
  )
{
  if (mSession.OnScreenKb == NULL) {
    return;
  }

  (VOID)Event;
  lv_obj_set_hidden (mSession.OnScreenKb, true);
  lv_keyboard_set_textarea (mSession.OnScreenKb, NULL);
}

/**
  Checkbox value-changed handler -- toggles the boolean and records it.
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

  if (!IsStatementInCurrentForm (Ctx->Statement)) {
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
  Dropdown opened handler -- sizes the popup list to its content so long
  option strings are not truncated by the (narrow) button width.
**/
STATIC
VOID
OnDropdownOpened (
  lv_event_t  *Event
  )
{
  lv_obj_t  *Dd;
  lv_obj_t  *List;

  Dd   = lv_event_get_target_obj (Event);
  List = lv_dropdown_get_list (Dd);
  if (List != NULL) {
    lv_obj_set_width (List, LV_SIZE_CONTENT);
    LvglThemeStyleDropdownList (List);
  }
}

/**
  Dropdown value-changed handler -- records the selected option index.
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

  if (!IsStatementInCurrentForm (Ctx->Statement)) {
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
  Ordered-list Up/Down button handler -- swaps two entries in the buffer
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
  if (!IsStatementInCurrentForm (Statement)) {
    return;
  }

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
  Popup button click handler -- records the chosen action and dismisses overlay.
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
    if ((mPopupPrevFocus != NULL) && (mSession.Group != NULL)) {
      lv_group_focus_obj (mPopupPrevFocus);
    }

    mPopupPrevFocus = NULL;
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

  //
  // Y/N/ESC shortcuts mirror the legacy text browser: Y confirms (Save),
  // N discards (when offered), ESC cancels.
  //
  if ((Key == LV_KEY_ESC) ||
      (Key == 'Y') || (Key == 'y') ||
      (Key == 'N') || (Key == 'n'))
  {
    if ((Key == 'Y') || (Key == 'y')) {
      mPopupResult = mPopupConfirmAction;
    } else if (((Key == 'N') || (Key == 'n')) && mPopupHasDiscard) {
      mPopupResult = mDiscardAction;
    } else {
      mPopupResult = BROWSER_ACTION_NONE;
    }

    if (mPopupOverlay != NULL) {
      lv_obj_delete (mPopupOverlay);
      mPopupOverlay  = NULL;
      mPopupFirstObj = NULL;
      mPopupLastObj  = NULL;
      if ((mPopupPrevFocus != NULL) && (mSession.Group != NULL)) {
        lv_group_focus_obj (mPopupPrevFocus);
      }

      mPopupPrevFocus = NULL;
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

typedef struct {
  lv_obj_t  *Overlay;
  lv_obj_t  *Card;
  lv_obj_t  *BtnRow;
} LVGL_DIALOG_SHELL;

/**
  Shared modal shell: dimmed overlay, dialog card, title, separator, button row.
**/
STATIC
VOID
CreateDialogShell (
  OUT LVGL_DIALOG_SHELL  *Shell,
  IN  CONST CHAR8        *Title
  )
{
  lv_obj_t  *TitleLbl;
  lv_obj_t  *Sep;

  Shell->Overlay = lv_obj_create (lv_layer_top ());
  lv_obj_set_size (Shell->Overlay, LV_PCT (100), LV_PCT (100));
  lv_obj_set_style_bg_color (Shell->Overlay, lv_color_black (), 0);
  lv_obj_set_style_bg_opa (Shell->Overlay, THEME_OVERLAY_OPA, 0);
  lv_obj_set_style_border_width (Shell->Overlay, 0, 0);
  lv_obj_set_style_pad_all (Shell->Overlay, 0, 0);
  lv_obj_set_flex_flow (Shell->Overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align (
    Shell->Overlay,
    LV_FLEX_ALIGN_CENTER,
    LV_FLEX_ALIGN_CENTER,
    LV_FLEX_ALIGN_CENTER
    );

  Shell->Card = lv_obj_create (Shell->Overlay);
  lv_obj_set_width (Shell->Card, LV_PCT (THEME_DIALOG_WIDTH_PCT));
  lv_obj_set_height (Shell->Card, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Shell->Card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all (Shell->Card, THEME_PAD_DIALOG, 0);
  lv_obj_set_style_pad_row (Shell->Card, THEME_PAD_DIALOG_ROW_GAP, 0);
  lv_obj_set_style_bg_color (Shell->Card, lv_color_hex (THEME_COLOR_BG_DIALOG), 0);
  lv_obj_set_style_radius (Shell->Card, THEME_RADIUS, 0);
  lv_obj_set_style_border_width (Shell->Card, 0, 0);

  TitleLbl = lv_label_create (Shell->Card);
  lv_label_set_text (TitleLbl, Title);
  THEME_APPLY_POPUP_FONT (TitleLbl);
  lv_obj_set_style_text_color (TitleLbl, lv_color_hex (THEME_COLOR_TEXT_TITLE), 0);

  Sep = lv_obj_create (Shell->Card);
  lv_obj_set_size (Sep, LV_PCT (100), THEME_BORDER_PANE);
  lv_obj_set_style_bg_color (Sep, lv_color_hex (THEME_COLOR_BG_SEPARATOR), 0);
  lv_obj_set_style_border_width (Sep, 0, 0);
  lv_obj_set_style_pad_all (Sep, 0, 0);

  Shell->BtnRow = lv_obj_create (Shell->Card);
  lv_obj_set_size (Shell->BtnRow, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Shell->BtnRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (
    Shell->BtnRow,
    LV_FLEX_ALIGN_END,
    LV_FLEX_ALIGN_CENTER,
    LV_FLEX_ALIGN_CENTER
    );
  lv_obj_set_style_pad_all (Shell->BtnRow, 0, 0);
  lv_obj_set_style_border_width (Shell->BtnRow, 0, 0);
  lv_obj_set_style_bg_opa (Shell->BtnRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_column (Shell->BtnRow, THEME_PAD_DIALOG_COL_GAP, 0);
}

STATIC
lv_obj_t *
CreateDialogMessageLabel (
  IN lv_obj_t      *Card,
  IN CONST CHAR8   *Text,
  IN BOOLEAN       Wrap
  )
{
  lv_obj_t  *MsgLbl;

  MsgLbl = lv_label_create (Card);
  if (Wrap) {
    lv_label_set_long_mode (MsgLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width (MsgLbl, LV_PCT (100));
  }

  lv_label_set_text (MsgLbl, Text);
  lv_obj_set_style_text_color (MsgLbl, lv_color_hex (THEME_COLOR_TEXT_POPUP), 0);
  THEME_APPLY_BODY_FONT (MsgLbl);
  return MsgLbl;
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
  LVGL_DIALOG_SHELL  Shell;
  lv_obj_t           *ConfirmBtn;
  lv_obj_t           *DiscardBtn;
  lv_obj_t           *CancelBtn;
  lv_obj_t           *Lbl;

  mPopupConfirmAction = ConfirmAction;
  mPopupResult        = LVGL_POPUP_PENDING;
  mPopupHasDiscard    = ShowDiscard;
  mPopupPrevFocus     = (Group != NULL) ? lv_group_get_focused (Group) : NULL;

  CreateDialogShell (&Shell, Title);
  mPopupOverlay = Shell.Overlay;

  CreateDialogMessageLabel (
    Shell.Card,
    ShowDiscard ? "You have unsaved changes." : "Save the current settings?",
    FALSE
    );

  //
  // Confirm (Save / Load) button.
  //
  ConfirmBtn = lv_button_create (Shell.BtnRow);
  Lbl        = lv_label_create (ConfirmBtn);
  {
    CHAR8  ConfirmText[64];
    AsciiSPrint (ConfirmText, sizeof (ConfirmText), "%a (Y)", ConfirmLabel);
    lv_label_set_text (Lbl, ConfirmText);
  }
  LvglThemeStyleButtonLabel (ConfirmBtn, Lbl);
  lv_obj_add_event_cb (ConfirmBtn, OnPopupBtn, LV_EVENT_CLICKED, &mPopupConfirmAction);
  lv_obj_add_event_cb (ConfirmBtn, OnPopupKey, LV_EVENT_KEY, NULL);
  lv_group_add_obj (Group, ConfirmBtn);
  lv_group_focus_obj (ConfirmBtn);
  mPopupFirstObj = ConfirmBtn;

  //
  // Optional Discard button (shown for "unsaved changes" popup only).
  //
  if (ShowDiscard) {
    DiscardBtn = lv_button_create (Shell.BtnRow);
    Lbl        = lv_label_create (DiscardBtn);
    lv_label_set_text (Lbl, "Discard (N)");
    LvglThemeStyleButtonLabel (DiscardBtn, Lbl);
    lv_obj_add_event_cb (DiscardBtn, OnPopupBtn, LV_EVENT_CLICKED, &mDiscardAction);
    lv_obj_add_event_cb (DiscardBtn, OnPopupKey, LV_EVENT_KEY, NULL);
    lv_group_add_obj (Group, DiscardBtn);
    mPopupLastObj = DiscardBtn;
  }

  //
  // Cancel button.
  //
  CancelBtn = lv_button_create (Shell.BtnRow);
  Lbl       = lv_label_create (CancelBtn);
  lv_label_set_text (Lbl, "Cancel (Esc)");
  LvglThemeStyleButtonLabel (CancelBtn, Lbl);
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
        RequestUserExit (HotKey->Action, HotKey->DefaultId, NULL);
      }

      return TRUE;
    }
  }

  return FALSE;
}

/**
  Indev-level key handler -- UP/DOWN/Tab navigation (including grayed rows)
  and ESC when nothing enabled is focused.
**/
STATIC
VOID
OnIndevFallbackKey (
  lv_event_t  *Event
  )
{
  lv_indev_t  *Indev;
  lv_key_t    Key;
  lv_obj_t    *Focused;
  BOOLEAN     Forward;
  UINTN       Found;
  UINTN       Idx;

  Indev = lv_indev_active ();
  Key   = lv_indev_get_key (Indev);

  //
  // Popup is open -- OnPopupKey handles all keys for popup buttons.
  //
  if (mPopupOverlay != NULL) {
    return;
  }

  Focused = (mSession.Group != NULL) ? lv_group_get_focused (mSession.Group) : NULL;

  //
  // UP/DOWN: walk mNavList (so grayed rows stay reachable). LVGL does not
  // hard-advance on these keys -- it sends them to the focused object.
  //
  // NEXT/PREV (Tab): LVGL always calls focus_next/prev AFTER this indev event
  // returns, and that path ignores event_stop_processing. Only commit-if-dirty
  // here; leave the actual focus move to LVGL when the field is unchanged.
  //
  Forward = (Key == LV_KEY_DOWN) || (Key == LV_KEY_NEXT);
  if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) ||
      (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))
  {
    //
    // ONE_OF dropdown keyboard-edit: UP/DOWN cycles options locally -- no
    // commit, no form rebuild. Commit is deferred to ENTER / leave paths.
    //
    if (((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN)) &&
        (Focused != NULL) &&
        lv_obj_check_type (Focused, &lv_dropdown_class) &&
        lv_group_get_editing (mSession.Group))
    {
      UINT32  Count = (UINT32)lv_dropdown_get_option_count (Focused);

      if (Count > 0) {
        UINT32  Sel = (UINT32)lv_dropdown_get_selected (Focused);

        if (Key == LV_KEY_DOWN) {
          Sel = (Sel + 1) % Count;
        } else {
          Sel = (Sel == 0) ? (Count - 1) : (Sel - 1);
        }

        lv_dropdown_set_selected (Focused, Sel);
      }

      return;
    }

    if (mNavCount == 0) {
      return;
    }

    Found = mNavCount;
    for (Idx = 0; Idx < mNavCount; Idx++) {
      if (mNavList[Idx] == Focused) {
        Found = Idx;
        break;
      }
    }

    if (Found == mNavCount) {
      Found = Forward ? (mNavCount - 1) : 0;
    }

    if (Forward) {
      Idx = (Found + 1) % mNavCount;
    } else {
      Idx = (Found == 0) ? (mNavCount - 1) : (Found - 1);
    }

    //
    // Commit the field being left so multi-field edits survive navigation.
    // After rebuild, focus is restored to Idx (the destination).
    //
    if (TryCommitFocusedThenNav (Idx)) {
      return;
    }

    //
    // Tab: do not move focus here -- keypad_proc will focus_next/prev next.
    // Arrows: we own the move (grayed-row support).
    //
    if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN)) {
      lv_group_focus_obj (mNavList[Idx]);
    }

    return;
  }

  //
  // Remaining handlers (ESC / F-keys) only fire when nothing is focused or
  // the focused widget is disabled -- when something enabled is focused,
  // its OnNavKey will receive the LV_EVENT_KEY directly.
  //
  if ((Focused != NULL) && !lv_obj_has_state (Focused, LV_STATE_DISABLED)) {
    return;
  }

  if (Key == LV_KEY_ESC) {
    //
    // Front page: no parent form to exit to -- swallow ESC so the user can't
    // accidentally drop out of setup.
    //
    if (!IsFrontPageForm ()) {
      RequestUserExit (BROWSER_ACTION_FORM_EXIT, 0, NULL);
    }

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

STATIC
UINT64
ClampU64 (
  IN UINT64  Value,
  IN UINT64  Low,
  IN UINT64  High
  )
{
  if (Value < Low) {
    return Low;
  }

  if (Value > High) {
    return High;
  }

  return Value;
}

STATIC
UINT64
GetStatementNumericValue (
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement,
  IN EFI_IFR_NUMERIC                *NumOp
  )
{
  switch (NumOp->Flags & EFI_IFR_NUMERIC_SIZE) {
    case EFI_IFR_NUMERIC_SIZE_1:
      return Statement->CurrentValue.Value.u8;
    case EFI_IFR_NUMERIC_SIZE_2:
      return Statement->CurrentValue.Value.u16;
    case EFI_IFR_NUMERIC_SIZE_4:
      return Statement->CurrentValue.Value.u32;
    default:
      return Statement->CurrentValue.Value.u64;
  }
}

/**
  Register an editable field for commit-on-nav / commit-on-exit.
**/
STATIC
VOID
RegisterEditEntry (
  IN LVGL_EDIT_KIND  Kind,
  IN VOID            *Ctx
  )
{
  if (Ctx == NULL) {
    return;
  }

  if ((Kind == LVGL_EDIT_NUMERIC) || (Kind == LVGL_EDIT_STRING)) {
    if (((LVGL_STATEMENT_CONTEXT *)Ctx)->Widget == NULL) {
      return;
    }
  }

  if (mEditRegistryCount >= LVGL_EDIT_MAX) {
    DEBUG ((DEBUG_WARN, "LvglRenderer: edit registry full, skipping field\n"));
    return;
  }

  mEditRegistry[mEditRegistryCount].Kind = Kind;
  if (Kind == LVGL_EDIT_DATETIME) {
    mEditRegistry[mEditRegistryCount].u.DateTime = (LVGL_DATETIME_CTX *)Ctx;
  } else {
    mEditRegistry[mEditRegistryCount].u.TextCtx = (LVGL_STATEMENT_CONTEXT *)Ctx;
  }

  mEditRegistryCount++;
}

STATIC
VOID
SetNumericUserInput (
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement,
  IN UINT64                         Value
  )
{
  EFI_IFR_NUMERIC  *NumOp;

  NumOp = (EFI_IFR_NUMERIC *)Statement->OpCode;

  mSession.UserInput->SelectedStatement = Statement;
  mSession.UserInput->InputValue.Type   = Statement->CurrentValue.Type;

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
}

/**
  Return TRUE when the string/password textarea differs from the statement.
**/
STATIC
BOOLEAN
IsStringTextDirty (
  IN LVGL_STATEMENT_CONTEXT  *Ctx,
  IN CONST CHAR8             *Utf8Text
  )
{
  CHAR16  *CurStr16;
  CHAR8   *CurUtf8;
  BOOLEAN Dirty;
  UINTN   Len;

  Len = AsciiStrLen (Utf8Text);
  if ((Ctx->Statement->OpCode->OpCode == EFI_IFR_PASSWORD_OP) ||
      (Ctx->Statement->CurrentValue.Value.string == 0))
  {
    return Len > 0;
  }

  CurStr16 = HiiGetString (
               Ctx->HiiHandle,
               Ctx->Statement->CurrentValue.Value.string,
               NULL
               );
  if (CurStr16 == NULL) {
    return Len > 0;
  }

  CurUtf8 = Ucs2ToUtf8 (CurStr16);
  FreePool (CurStr16);
  if (CurUtf8 == NULL) {
    return Len > 0;
  }

  Dirty = (AsciiStrCmp (Utf8Text, CurUtf8) != 0);
  FreePool (CurUtf8);
  return Dirty;
}

/**
  Stage a numeric USER_INPUT from Ctx->Widget.

  @param[in] Force  TRUE to always stage (Enter). FALSE only if dirty.
**/
STATIC
BOOLEAN
StageNumericUserInput (
  IN LVGL_STATEMENT_CONTEXT  *Ctx,
  IN BOOLEAN                 Force
  )
{
  EFI_IFR_NUMERIC  *NumOp;
  UINT64           Value;
  UINT64           MinVal;
  UINT64           MaxVal;
  lv_obj_t         *Ta;

  if ((Ctx == NULL) || (Ctx->Widget == NULL) ||
      (mSession.UserInput == NULL) || mSession.ExitRequested)
  {
    return FALSE;
  }

  if (!IsStatementInCurrentForm (Ctx->Statement)) {
    return FALSE;
  }

  Ta    = Ctx->Widget;
  NumOp = (EFI_IFR_NUMERIC *)Ctx->Statement->OpCode;
  Value = AsciiDecimalToUint64 (lv_textarea_get_text (Ta));
  GetNumericRange (NumOp, &MinVal, &MaxVal);
  Value = ClampU64 (Value, MinVal, MaxVal);

  if (!Force && (Value == GetStatementNumericValue (Ctx->Statement, NumOp))) {
    return FALSE;
  }

  SetNumericUserInput (Ctx->Statement, Value);
  mSession.ExitRequested = TRUE;
  return TRUE;
}

STATIC
BOOLEAN
StageStringUserInput (
  IN LVGL_STATEMENT_CONTEXT  *Ctx,
  IN BOOLEAN                 Force
  )
{
  CONST CHAR8     *Utf8Text;
  CHAR16          *Str16;
  CHAR16          *PoolBuf;
  UINTN           SrcLen;
  UINTN           BufBytes;
  UINTN           MaxChars;
  UINTN           CopyChars;
  UINTN           Idx;
  EFI_IFR_STRING  *StringOp;
  EFI_STRING_ID   NewStringId;
  lv_obj_t        *Ta;

  if ((Ctx == NULL) || (Ctx->Widget == NULL) ||
      (mSession.UserInput == NULL) || mSession.ExitRequested)
  {
    return FALSE;
  }

  if (!IsStatementInCurrentForm (Ctx->Statement)) {
    return FALSE;
  }

  Ta = Ctx->Widget;
  Utf8Text = lv_textarea_get_text (Ta);
  if (Utf8Text == NULL) {
    Utf8Text = "";
  }

  SrcLen = AsciiStrLen (Utf8Text);

  if (!Force && !IsStringTextDirty (Ctx, Utf8Text)) {
    return FALSE;
  }

  Str16 = AllocateZeroPool ((SrcLen + 1) * sizeof (CHAR16));
  if (Str16 == NULL) {
    return FALSE;
  }

  for (Idx = 0; Idx < SrcLen; Idx++) {
    Str16[Idx] = (CHAR16)(UINT8)Utf8Text[Idx];
  }

  Str16[SrcLen] = L'\0';

  BufBytes = Ctx->Statement->CurrentValue.BufferLen;
  if (BufBytes == 0) {
    StringOp = (EFI_IFR_STRING *)Ctx->Statement->OpCode;
    BufBytes = (UINTN)StringOp->MaxSize * sizeof (CHAR16);
  }

  if (BufBytes < sizeof (CHAR16)) {
    FreePool (Str16);
    return FALSE;
  }

  PoolBuf = AllocateZeroPool (BufBytes);
  if (PoolBuf == NULL) {
    FreePool (Str16);
    return FALSE;
  }

  MaxChars  = (BufBytes / sizeof (CHAR16)) - 1;
  CopyChars = (SrcLen < MaxChars) ? SrcLen : MaxChars;
  CopyMem (PoolBuf, Str16, CopyChars * sizeof (CHAR16));

  NewStringId = HiiSetString (mSession.FormData->HiiHandle, 0, Str16, NULL);
  FreePool (Str16);
  if (NewStringId == 0) {
    FreePool (PoolBuf);
    return FALSE;
  }

  mSession.UserInput->SelectedStatement       = Ctx->Statement;
  mSession.UserInput->InputValue.Type         = EFI_IFR_TYPE_STRING;
  mSession.UserInput->InputValue.Buffer       = (UINT8 *)PoolBuf;
  mSession.UserInput->InputValue.BufferLen    = (UINT16)BufBytes;
  mSession.UserInput->InputValue.Value.string = NewStringId;
  mSession.UserInput->Action                  = 0;
  mSession.ExitRequested                      = TRUE;
  return TRUE;
}

STATIC
VOID
ReadDateTimeFields (
  IN  LVGL_DATETIME_CTX  *Ctx,
  OUT UINT64             *V0,
  OUT UINT64             *V1,
  OUT UINT64             *V2
  )
{
  *V0 = AsciiDecimalToUint64 (lv_textarea_get_text (Ctx->Field0));
  *V1 = AsciiDecimalToUint64 (lv_textarea_get_text (Ctx->Field1));
  *V2 = AsciiDecimalToUint64 (lv_textarea_get_text (Ctx->Field2));

  if (Ctx->IsDate) {
    *V0 = ClampU64 (*V0, 1, 12);
    *V1 = ClampU64 (*V1, 1, 31);
    *V2 = ClampU64 (*V2, 1998, 2099);
  } else {
    *V0 = ClampU64 (*V0, 0, 23);
    *V1 = ClampU64 (*V1, 0, 59);
    *V2 = ClampU64 (*V2, 0, 59);
  }
}

STATIC
BOOLEAN
StageDateTimeUserInput (
  IN LVGL_DATETIME_CTX  *Ctx,
  IN BOOLEAN            Force
  )
{
  UINT64  V0;
  UINT64  V1;
  UINT64  V2;

  if ((Ctx == NULL) || (mSession.UserInput == NULL) || mSession.ExitRequested) {
    return FALSE;
  }

  if (!IsStatementInCurrentForm (Ctx->Statement)) {
    return FALSE;
  }

  ReadDateTimeFields (Ctx, &V0, &V1, &V2);

  if (!Force) {
    if (Ctx->IsDate) {
      if ((V0 == Ctx->Statement->CurrentValue.Value.date.Month) &&
          (V1 == Ctx->Statement->CurrentValue.Value.date.Day) &&
          (V2 == Ctx->Statement->CurrentValue.Value.date.Year))
      {
        return FALSE;
      }
    } else if ((V0 == Ctx->Statement->CurrentValue.Value.time.Hour) &&
               (V1 == Ctx->Statement->CurrentValue.Value.time.Minute) &&
               (V2 == Ctx->Statement->CurrentValue.Value.time.Second))
    {
      return FALSE;
    }
  }

  CopyMem (
    &mSession.UserInput->InputValue,
    &Ctx->Statement->CurrentValue,
    sizeof (EFI_HII_VALUE)
    );

  if (Ctx->IsDate) {
    mSession.UserInput->InputValue.Value.date.Month = (UINT8)V0;
    mSession.UserInput->InputValue.Value.date.Day   = (UINT8)V1;
    mSession.UserInput->InputValue.Value.date.Year  = (UINT16)V2;
  } else {
    mSession.UserInput->InputValue.Value.time.Hour   = (UINT8)V0;
    mSession.UserInput->InputValue.Value.time.Minute = (UINT8)V1;
    mSession.UserInput->InputValue.Value.time.Second = (UINT8)V2;
  }

  mSession.UserInput->SelectedStatement = Ctx->Statement;
  mSession.UserInput->Action            = 0;
  mSession.ExitRequested                = TRUE;
  return TRUE;
}

/**
  Find the edit registry entry for a focused widget, if any.
**/
STATIC
LVGL_EDIT_ENTRY *
FindEditEntryForWidget (
  IN lv_obj_t  *Widget
  )
{
  UINTN            Idx;
  LVGL_EDIT_ENTRY  *Entry;

  if (Widget == NULL) {
    return NULL;
  }

  for (Idx = 0; Idx < mEditRegistryCount; Idx++) {
    Entry = &mEditRegistry[Idx];
    if (((Entry->Kind == LVGL_EDIT_NUMERIC) || (Entry->Kind == LVGL_EDIT_STRING)) &&
        (Entry->u.TextCtx != NULL) &&
        (Entry->u.TextCtx->Widget == Widget))
    {
      return Entry;
    }

    if ((Entry->Kind == LVGL_EDIT_DATETIME) &&
        (Entry->u.DateTime != NULL) &&
        ((Entry->u.DateTime->Field0 == Widget) ||
         (Entry->u.DateTime->Field1 == Widget) ||
         (Entry->u.DateTime->Field2 == Widget)))
    {
      return Entry;
    }
  }

  return NULL;
}

/**
  Commit the focused editable field if its LVGL value differs from the statement.

  @retval TRUE   A value was staged and the event loop should exit.
**/
STATIC
BOOLEAN
CommitFocusedEditIfDirty (
  VOID
  )
{
  LVGL_EDIT_ENTRY  *Entry;
  lv_obj_t         *Focused;

  if (mSession.UserInput == NULL) {
    return FALSE;
  }

  Focused = (mSession.Group != NULL) ? lv_group_get_focused (mSession.Group) : NULL;
  Entry   = FindEditEntryForWidget (Focused);
  if (Entry == NULL) {
    return FALSE;
  }

  switch (Entry->Kind) {
    case LVGL_EDIT_NUMERIC:
      return StageNumericUserInput (Entry->u.TextCtx, FALSE);

    case LVGL_EDIT_STRING:
      return StageStringUserInput (Entry->u.TextCtx, FALSE);

    case LVGL_EDIT_DATETIME:
      return StageDateTimeUserInput (Entry->u.DateTime, FALSE);

    default:
      return FALSE;
  }
}

STATIC
VOID
ApplyUserInput (
  IN UINT32                         Action,
  IN UINT16                         DefaultId,
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement OPTIONAL
  )
{
  mSession.UserInput->Action    = Action;
  mSession.UserInput->DefaultId = DefaultId;
  if (Statement != NULL) {
    mSession.UserInput->SelectedStatement = Statement;
    CopyMem (
      &mSession.UserInput->InputValue,
      &Statement->CurrentValue,
      sizeof (EFI_HII_VALUE)
      );
  } else {
    mSession.UserInput->SelectedStatement = NULL;
  }

  mSession.ExitRequested = TRUE;
}

/**
  Commit focused dirty field (if any), then leave / navigate / hotkey.
**/
STATIC
VOID
RequestUserExit (
  IN UINT32                         Action,
  IN UINT16                         DefaultId,
  IN FORM_DISPLAY_ENGINE_STATEMENT  *Statement OPTIONAL
  )
{
  if (CommitFocusedEditIfDirty ()) {
    ZeroMem (&mPendingLeave, sizeof (mPendingLeave));
    mPendingLeave.Active    = TRUE;
    mPendingLeave.Action    = Action;
    mPendingLeave.DefaultId = DefaultId;
    mPendingLeave.OpCode    = (Statement != NULL) ? Statement->OpCode : NULL;
    return;
  }

  ApplyUserInput (Action, DefaultId, Statement);
}

/**
  Finish deferred leave after a one-shot focused-field commit.
**/
STATIC
BOOLEAN
CompletePendingLeaveIfReady (
  VOID
  )
{
  if (!mPendingLeave.Active) {
    return FALSE;
  }

  ApplyUserInput (
    mPendingLeave.Action,
    mPendingLeave.DefaultId,
    FindStatementByOpCode (mPendingLeave.OpCode)
    );
  ZeroMem (&mPendingLeave, sizeof (mPendingLeave));
  return TRUE;
}

/**
  If the focused field is dirty, commit it and restore focus to @a DestNavIdx
  after the form rebuilds.

  @retval TRUE   Commit staged; caller should return without moving focus.
**/
STATIC
BOOLEAN
TryCommitFocusedThenNav (
  IN UINTN  DestNavIdx
  )
{
  if (!CommitFocusedEditIfDirty ()) {
    return FALSE;
  }

  mRestoreFocusNavIdx  = DestNavIdx;
  mRestoreFocusPending = TRUE;
  return TRUE;
}

/**
  Numeric textarea commit -- fires when the user presses ENTER.
**/
STATIC
VOID
OnNumericReady (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  (VOID)StageNumericUserInput (Ctx, TRUE);
}

/**
  String textarea commit -- fires when the user presses ENTER.
**/
STATIC
VOID
OnStringReady (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  (VOID)StageStringUserInput (Ctx, TRUE);
}

/**
  Date/Time sub-field commit -- fires when ENTER is pressed on any field.
**/
STATIC
VOID
OnDateTimeReady (
  lv_event_t  *Event
  )
{
  LVGL_DATETIME_CTX  *Ctx;

  Ctx = (LVGL_DATETIME_CTX *)lv_event_get_user_data (Event);
  (VOID)StageDateTimeUserInput (Ctx, TRUE);
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
    //
    // If a dropdown popup is open (mouse-click or Enter-toggle), ESC
    // should close the popup, not exit the form.
    //
    Focused = lv_group_get_focused (mSession.Group);
    if ((Focused != NULL) &&
        lv_obj_check_type (Focused, &lv_dropdown_class) &&
        lv_dropdown_is_open (Focused))
    {
      lv_dropdown_close (Focused);
      if (Editing) {
        lv_group_set_editing (mSession.Group, false);
      }

      lv_event_stop_processing (Event);
      return;
    }

    if (Editing &&
        (mEditingDropdown != NULL) &&
        (Focused == mEditingDropdown))
    {
      //
      // ESC during dropdown keyboard-edit: revert the selection and stay
      // on the form. Text fields below fall through to RequestUserExit so
      // a single ESC flushes the edit and leaves.
      //
      lv_dropdown_set_selected (mEditingDropdown, mEditingDropdownOrigSel);
      mEditingDropdown = NULL;
      lv_group_set_editing (mSession.Group, false);
      lv_event_stop_processing (Event);
      return;
    }

    if (Editing) {
      lv_group_set_editing (mSession.Group, false);
    }

    if (!IsFrontPageForm ()) {
      RequestUserExit (BROWSER_ACTION_FORM_EXIT, 0, NULL);
    }

    lv_event_stop_processing (Event);
    return;
  }

  if (HandleFunctionKey (Key)) {
    lv_event_stop_processing (Event);
    return;
  }

  if (Editing) {
    //
    // While a dropdown is in keyboard-edit mode, intercept ENTER (confirm)
    // and UP/DOWN (already handled by OnIndevFallbackKey -- stop them here so
    // LVGL's own class handler doesn't also advance or open the list).
    //
    Focused = lv_group_get_focused (mSession.Group);
    if ((Focused != NULL) && lv_obj_check_type (Focused, &lv_dropdown_class)) {
      if (Key == LV_KEY_ENTER) {
        //
        // Confirm: commit the current selection once, exit editing.
        //
        mEditingDropdown = NULL;
        lv_group_set_editing (mSession.Group, false);
        lv_obj_send_event (Focused, LV_EVENT_VALUE_CHANGED, NULL);
        lv_event_stop_processing (Event);
        return;
      }

      if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) ||
          (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))
      {
        //
        // OnIndevFallbackKey already cycled the selection; stop processing
        // here so the LVGL dropdown class handler doesn't also advance/open.
        //
        lv_event_stop_processing (Event);
        return;
      }
    }

    if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) ||
        (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))
    {
      //
      // Text fields: OnIndevFallbackKey already commits+navigates. Stop so
      // the textarea class doesn't treat arrows as caret motion instead.
      //
      lv_event_stop_processing (Event);
      return;
    }

    //
    // All other editing widget key handling (spinbox LEFT/RIGHT, etc.) falls
    // through to the LVGL class handler unchanged.
    //
    return;
  }

  (void)Ctx;

  if ((Key == LV_KEY_UP) || (Key == LV_KEY_DOWN) ||
      (Key == LV_KEY_NEXT) || (Key == LV_KEY_PREV))
  {
    //
    // UP/DOWN/Tab are handled exclusively by OnIndevFallbackKey so we don't
    // double-advance. LVGL fires LV_EVENT_KEY on the indev BEFORE
    // routing to the focused widget; the indev handler advances focus
    // (and may commit a dirty field), and then if we also advanced here we
    // would skip a row.
    //
    lv_event_stop_processing (Event);
    return;
  } else if (Key == LV_KEY_ENTER) {
    Focused = lv_group_get_focused (mSession.Group);
    if ((Focused != NULL) &&
        !lv_obj_has_state (Focused, LV_STATE_DISABLED) &&
        (lv_obj_check_type (Focused, &lv_dropdown_class) ||
         lv_obj_check_type (Focused, &lv_spinbox_class)))
    {
      //
      // For dropdowns, snapshot the current selection so ESC can revert it.
      //
      if (lv_obj_check_type (Focused, &lv_dropdown_class)) {
        mEditingDropdown        = Focused;
        mEditingDropdownOrigSel = (UINT32)lv_dropdown_get_selected (Focused);
      }

      lv_group_set_editing (mSession.Group, true);
      lv_event_stop_processing (Event);
    }
    //
    // Textarea: do NOT intercept -- Enter passes to the class handler,
    // which fires LV_EVENT_READY -> OnStringReady commits the value.
    //
  }
}

/**
  LV_EVENT_FOCUSED handler -- push the focused statement's Help string into
  the chrome's help pane.
**/
STATIC
VOID
OnFocusUpdateHelp (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;
  CHAR8                   *Utf8;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);

  if ((Ctx == NULL) || (Ctx->Statement == NULL) ||
      (mSession.FormData == NULL) || (mSession.FormData->HiiHandle == NULL))
  {
    AptioSetHelpText ("");
    return;
  }

  Utf8 = GetHelpUtf8 (Ctx->Statement, mSession.FormData->HiiHandle);
  AptioSetHelpText (Utf8 != NULL ? Utf8 : "");
  if (Utf8 != NULL) {
    FreePool (Utf8);
  }
}

/**
  LV_EVENT_HOVER_OVER handler -- update the help pane text while the mouse
  hovers over a statement row.  Bound to both the inner control widget (via
  AddToNavGroup) AND the Row container (via BindRowHover) so the help pane
  updates regardless of which part of the row the cursor is over.

  Keyboard focus changes continue to update the pane via OnFocusUpdateHelp,
  so the pane is never stale -- it just retains the last-hovered row's text
  when the cursor moves off all rows, which is the desired behaviour.
**/
STATIC
VOID
OnHoverUpdateHelp (
  lv_event_t  *Event
  )
{
  LVGL_STATEMENT_CONTEXT  *Ctx;
  CHAR8                   *Utf8;

  Ctx = (LVGL_STATEMENT_CONTEXT *)lv_event_get_user_data (Event);
  if ((Ctx == NULL) || (Ctx->Statement == NULL) ||
      (mSession.FormData == NULL) || (mSession.FormData->HiiHandle == NULL))
  {
    return;
  }

  Utf8 = GetHelpUtf8 (Ctx->Statement, mSession.FormData->HiiHandle);
  AptioSetHelpText (Utf8 != NULL ? Utf8 : "");
  if (Utf8 != NULL) {
    FreePool (Utf8);
  }
}

/**
  Return the content-panel child that should drive scroll positioning for a
  nav widget (the row container when the widget is nested inside one).
**/
STATIC
lv_obj_t *
NavScrollTarget (
  IN lv_obj_t  *NavWidget
  )
{
  lv_obj_t  *Obj;

  if (mContentPanel == NULL) {
    return NavWidget;
  }

  Obj = NavWidget;
  while ((Obj != NULL) &&
         (lv_obj_get_parent (Obj) != NULL) &&
         (lv_obj_get_parent (Obj) != mContentPanel))
  {
    Obj = lv_obj_get_parent (Obj);
  }

  if ((Obj != NULL) && (lv_obj_get_parent (Obj) == mContentPanel)) {
    return Obj;
  }

  return NavWidget;
}

/**
  Scroll a nav row into view. When the row fits entirely in the viewport from
  scroll offset zero, prefer showing content from the top so banner lines and
  other labels above the first controls stay visible after navigating back up.
**/
STATIC
VOID
ScrollNavIntoView (
  IN lv_obj_t  *NavWidget
  )
{
  lv_obj_t  *Target;
  int32_t   ItemY;
  int32_t   ItemH;
  int32_t   ViewH;

  Target = NavScrollTarget (NavWidget);

  if (mContentPanel == NULL) {
    lv_obj_scroll_to_view (Target, LV_ANIM_OFF);
    return;
  }

  lv_obj_update_layout (mContentPanel);

  ItemY = lv_obj_get_y (Target);
  ItemH = lv_obj_get_height (Target);
  ViewH = lv_obj_get_height (mContentPanel);
  ViewH -= lv_obj_get_style_pad_top (mContentPanel, LV_PART_MAIN);
  ViewH -= lv_obj_get_style_pad_bottom (mContentPanel, LV_PART_MAIN);

  if ((ItemY >= 0) && (ItemY + ItemH <= ViewH)) {
    lv_obj_scroll_to_y (mContentPanel, 0, LV_ANIM_OFF);
    return;
  }

  lv_obj_scroll_to_view (Target, LV_ANIM_OFF);
}

STATIC
VOID
OnNavFocused (
  lv_event_t  *Event
  )
{
  ScrollNavIntoView (lv_event_get_target_obj (Event));
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
  lv_obj_set_scroll_on_focus (Widget, false);
  lv_obj_add_event_cb (Widget, OnNavFocused, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb (Widget, OnNavKey, LV_EVENT_KEY | LV_EVENT_PREPROCESS, Ctx);
  if (Ctx != NULL) {
    lv_obj_add_event_cb (Widget, OnFocusUpdateHelp, LV_EVENT_FOCUSED,   Ctx);
    lv_obj_add_event_cb (Widget, OnHoverUpdateHelp, LV_EVENT_HOVER_OVER, Ctx);
  }

  if (mNavCount < LVGL_NAV_MAX) {
    mNavStatement[mNavCount] = (Ctx != NULL) ? Ctx->Statement : NULL;
    mNavList[mNavCount]      = Widget;
    mNavCount++;
  }
}

//
// ---- Row styling: Aptio-style focus highlight ----
//
// All statement rows share one visual: a flush, full-width container
// with a subtle bottom separator. The focused row turns solid blue
// (THEME_COLOR_HIGHLIGHT_ROW) with white text.
//

STATIC
VOID
StyleRow (
  lv_obj_t  *Row
  )
{
  lv_obj_set_size (Row, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Row, LV_FLEX_FLOW_ROW);
  lv_obj_set_scrollable (Row, false);

  // default: panel-tinted capsule
  lv_obj_set_style_bg_color (Row, lv_color_hex (THEME_COLOR_ROW_BG), 0);
  lv_obj_set_style_bg_opa (Row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width (Row, 0, 0);
  lv_obj_set_style_border_side (Row, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_color (Row, lv_color_hex (THEME_COLOR_ACCENT), 0);
  lv_obj_set_style_radius (Row, THEME_RADIUS_ROW, 0);
  lv_obj_set_style_pad_left (Row, THEME_PAD_ROW_X, 0);
  lv_obj_set_style_pad_right (Row, THEME_PAD_ROW_X, 0);
  lv_obj_set_style_pad_top (Row, THEME_PAD_ROW_Y, 0);
  lv_obj_set_style_pad_bottom (Row, THEME_PAD_ROW_Y, 0);
  lv_obj_set_style_text_color (Row, lv_color_hex (THEME_COLOR_ROW_TEXT), 0);
  lv_obj_set_style_shadow_width (Row, 0, 0);
  LvglThemeApplyBodyFont (Row);

  // hovered: slight elevation, no border -- shows intent before click/focus
  lv_obj_set_style_bg_color (Row, lv_color_hex (THEME_COLOR_ROW_BG_HOVER), LV_STATE_HOVERED);
  lv_obj_set_style_bg_opa (Row, LV_OPA_COVER, LV_STATE_HOVERED);
  lv_obj_set_style_text_color (Row, lv_color_hex (THEME_COLOR_ROW_TEXT_FOCUSED), LV_STATE_HOVERED);
  lv_obj_set_style_shadow_width (Row, 0, LV_STATE_HOVERED);

  // focused: elevated panel + accent left bar + brighter text
  lv_obj_set_style_bg_color (Row, lv_color_hex (THEME_COLOR_BG_PANEL_ELEV), LV_STATE_FOCUSED);
  lv_obj_set_style_bg_opa (Row, LV_OPA_COVER, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width (Row, THEME_BORDER_FOCUS, LV_STATE_FOCUSED);
  lv_obj_set_style_text_color (Row, lv_color_hex (THEME_COLOR_ROW_TEXT_FOCUSED), LV_STATE_FOCUSED);
  lv_obj_set_style_shadow_width (Row, 0, LV_STATE_FOCUSED);

  // disabled: whole-row muted text, no opacity fade.
  lv_obj_set_style_text_color (Row, lv_color_hex (THEME_COLOR_ROW_TEXT_DISABLED), LV_STATE_DISABLED);
}

//
// Decorate a row whose underlying question has been modified since the form
// opened: solid amber left bar in the default state. The focused-state
// border (StyleRow above) overrides this when the row gains focus.
//
STATIC
VOID
MarkRowChanged (
  lv_obj_t  *Row
  )
{
  lv_obj_set_style_border_color (Row, lv_color_hex (THEME_COLOR_WARNING), 0);
  lv_obj_set_style_border_side  (Row, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width (Row, THEME_BORDER_FOCUS, 0);
}

STATIC
VOID
OnRowFocusChange (
  lv_event_t  *Event
  )
{
  lv_obj_t         *Row;
  lv_event_code_t  Code;

  Row  = (lv_obj_t *)lv_event_get_user_data (Event);
  Code = lv_event_get_code (Event);
  if (Row == NULL) {
    return;
  }
  if (Code == LV_EVENT_FOCUSED) {
    lv_obj_add_state (Row, LV_STATE_FOCUSED);
  } else if (Code == LV_EVENT_DEFOCUSED) {
    lv_obj_remove_state (Row, LV_STATE_FOCUSED);
  }
}

//
// Propagate pointer hover from an inner widget to its wrapping Row so the
// entire row highlights, not just the part directly under the cursor.
//
STATIC
VOID
OnRowHoverChange (
  lv_event_t  *Event
  )
{
  lv_obj_t         *Row;
  lv_event_code_t  Code;

  Row  = (lv_obj_t *)lv_event_get_user_data (Event);
  Code = lv_event_get_code (Event);
  if (Row == NULL) {
    return;
  }
  if (Code == LV_EVENT_HOVER_OVER) {
    lv_obj_add_state (Row, LV_STATE_HOVERED);
  } else if (Code == LV_EVENT_HOVER_LEAVE) {
    lv_obj_remove_state (Row, LV_STATE_HOVERED);
  }
}

//
// Bind the inner navigable widget's focus events to a wrapping Row so
// the whole row paints highlighted, not just the inner control.
//
STATIC
VOID
BindRowFocus (
  lv_obj_t  *Widget,
  lv_obj_t  *Row
  )
{
  lv_obj_add_event_cb (Widget, OnRowFocusChange, LV_EVENT_FOCUSED, Row);
  lv_obj_add_event_cb (Widget, OnRowFocusChange, LV_EVENT_DEFOCUSED, Row);
}

//
// Bind inner widget's hover events to the wrapping Row so the whole row
// highlights when the mouse is over any part of the inner control.
// Also bind OnHoverUpdateHelp to the Row container itself so the help pane
// updates when the cursor is over the prompt label or row padding (the
// majority of the row's clickable area), not only over the inner control.
//
STATIC
VOID
BindRowHover (
  lv_obj_t               *Widget,
  lv_obj_t               *Row,
  LVGL_STATEMENT_CONTEXT *Ctx
  )
{
  lv_obj_add_event_cb (Widget, OnRowHoverChange, LV_EVENT_HOVER_OVER,  Row);
  lv_obj_add_event_cb (Widget, OnRowHoverChange, LV_EVENT_HOVER_LEAVE, Row);
  if (Ctx != NULL) {
    lv_obj_add_event_cb (Row, OnHoverUpdateHelp, LV_EVENT_HOVER_OVER, Ctx);
  }
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
  lv_label_set_long_mode (Label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width (Label, LV_PCT (100));
  lv_label_set_text (Label, Text);
  THEME_APPLY_BODY_FONT (Label);
  lv_obj_set_style_text_color (Label, lv_palette_main (THEME_ACCENT_PALETTE), 0);
  lv_obj_set_style_pad_top (Label, THEME_PAD_LABEL_TOP, 0);

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
  CHAR8         *Prompt;
  CHAR8         *ValueUtf8;
  CHAR16        *ValueUcs2;
  EFI_IFR_TEXT  *TextOp;
  lv_obj_t      *Row;
  lv_obj_t      *PromptLabel;
  lv_obj_t      *ValueLabel;

  Prompt = GetPromptUtf8 (Statement, HiiHandle);
  if (Prompt == NULL) {
    return;
  }

  TextOp    = (EFI_IFR_TEXT *)Statement->OpCode;
  ValueUtf8 = NULL;
  if (TextOp->TextTwo != 0) {
    ValueUcs2 = HiiGetString (HiiHandle, TextOp->TextTwo, NULL);
    if (ValueUcs2 != NULL) {
      ValueUtf8 = Ucs2ToUtf8 (ValueUcs2);
      FreePool (ValueUcs2);
    }
  }

  Row = lv_obj_create (Parent);
  StyleRow (Row);

  PromptLabel = lv_label_create (Row);
  lv_label_set_text (PromptLabel, Prompt);
  lv_obj_set_flex_grow (PromptLabel, 1);
  LvglThemeApplyBodyFont (PromptLabel);

  if (ValueUtf8 != NULL) {
    ValueLabel = lv_label_create (Row);
    lv_label_set_text (ValueLabel, ValueUtf8);
    LvglThemeApplyBodyFont (ValueLabel);
    FreePool (ValueUtf8);
  }

  FreePool (Prompt);
}

/**
  Render a front-page banner line (device model / BIOS version / CPU / memory /
  battery). These arrive as Tiano GUIDed EFI_IFR_EXTEND_OP_BANNER opcodes, which
  the text-mode engine drew via CustomizedDisplayLib's PrintBannerInfo().

  When SubtitleShowsDeviceModel is enabled in Graphical UI Configuration, the
  centered line (device model) is shown in the subtitle bar instead and is
  skipped here.
**/
STATIC
VOID
CreateBannerWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                 HiiHandle
  )
{
  EFI_IFR_GUID_BANNER  *Banner;
  CHAR16               *Ucs2;
  CHAR8                *Utf8;
  lv_obj_t             *Label;

  Banner = (EFI_IFR_GUID_BANNER *)Statement->OpCode;
  if (Banner->Title == 0) {
    return;
  }

  if (mSubtitleShowsDeviceModel &&
      (Banner->Alignment == EFI_IFR_BANNER_ALIGN_CENTER))
  {
    return;
  }

  Ucs2 = HiiGetString (HiiHandle, Banner->Title, NULL);
  if (Ucs2 == NULL) {
    return;
  }

  Utf8 = Ucs2ToUtf8 (Ucs2);
  FreePool (Ucs2);
  if (Utf8 == NULL) {
    return;
  }

  //
  // Skip placeholder/empty banner lines (e.g. an unpopulated battery row).
  //
  if (Utf8[0] == '\0') {
    FreePool (Utf8);
    return;
  }

  Label = lv_label_create (Parent);
  lv_label_set_long_mode (Label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width (Label, LV_PCT (100));
  lv_label_set_text (Label, Utf8);
  THEME_APPLY_BODY_FONT (Label);
  lv_obj_set_style_text_color (Label, lv_color_hex (THEME_COLOR_TEXT_PRIMARY), 0);

  switch (Banner->Alignment) {
    case EFI_IFR_BANNER_ALIGN_CENTER:
      lv_obj_set_style_text_align (Label, LV_TEXT_ALIGN_CENTER, 0);
      //
      // The centered line is the device model; give it the title font and
      // accent color so it reads as the banner heading.
      //
      lv_obj_set_style_text_font (Label, THEME_FONT_TITLE, 0);
      lv_obj_set_style_text_color (Label, lv_color_hex (THEME_COLOR_ACCENT_HOVER), 0);
      break;

    case EFI_IFR_BANNER_ALIGN_RIGHT:
      lv_obj_set_style_text_align (Label, LV_TEXT_ALIGN_RIGHT, 0);
      break;

    default:
      lv_obj_set_style_text_align (Label, LV_TEXT_ALIGN_LEFT, 0);
      break;
  }

  //
  // Register the label so BannerRefreshTimerCb can keep it in sync with its
  // (possibly changing) HII string, e.g. live battery status.
  //
  if (mBannerCount < LVGL_BANNER_MAX) {
    mBannerRegistry[mBannerCount].Label     = Label;
    mBannerRegistry[mBannerCount].HiiHandle = HiiHandle;
    mBannerRegistry[mBannerCount].StringId  = Banner->Title;
    mBannerCount++;
  }

  FreePool (Utf8);
}

/**
  Timer callback: re-read each registered banner's HII string and update its
  label when the text has changed. Runs while a form with banners is open
  (typically the front page) so live values such as battery status stay
  current without a full form rebuild.
**/
STATIC
VOID
BannerRefreshTimerCb (
  lv_timer_t  *Timer
  )
{
  UINTN   Idx;
  CHAR16  *Ucs2;
  CHAR8   *Utf8;

  (VOID)Timer;

  for (Idx = 0; Idx < mBannerCount; Idx++) {
    if (mBannerRegistry[Idx].Label == NULL) {
      continue;
    }

    Ucs2 = HiiGetString (mBannerRegistry[Idx].HiiHandle, mBannerRegistry[Idx].StringId, NULL);
    if (Ucs2 == NULL) {
      continue;
    }

    Utf8 = Ucs2ToUtf8 (Ucs2);
    FreePool (Ucs2);
    if (Utf8 == NULL) {
      continue;
    }

    if ((Utf8[0] != '\0') &&
        (AsciiStrCmp (lv_label_get_text (mBannerRegistry[Idx].Label), Utf8) != 0))
    {
      lv_label_set_text (mBannerRegistry[Idx].Label, Utf8);
    }

    FreePool (Utf8);
  }
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
  lv_obj_t                *Row;
  lv_obj_t                *Label;
  lv_obj_t                *Cb;
  LVGL_STATEMENT_CONTEXT  *Ctx;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Row = lv_obj_create (Parent);
  StyleRow (Row);
  if (Statement->SettingChangedFlag) {
    MarkRowChanged (Row);
  }

  //
  // Prompt label on the left, growing to fill the row so the checkbox
  // control aligns to the right edge, consistent with ONE_OF and numeric
  // rows.
  //
  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "Checkbox");
  lv_obj_set_flex_grow (Label, 1);
  LvglThemeApplyBodyFont (Label);

  //
  // The checkbox itself renders the indicator only; its prompt text is
  // provided by the label above rather than by the checkbox widget.
  //
  Cb = lv_checkbox_create (Row);
  lv_checkbox_set_text (Cb, "");
  lv_obj_set_style_bg_opa (Cb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width (Cb, 0, LV_PART_MAIN);
  LvglThemeStyleCheckbox (Cb);

  if (Statement->CurrentValue.Value.b) {
    lv_obj_add_state (Cb, LV_STATE_CHECKED);
  }

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Row, LV_STATE_DISABLED);
    lv_obj_add_state (Cb, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Cb;
    lv_obj_add_event_cb (Cb, OnCheckboxChanged, LV_EVENT_VALUE_CHANGED, Ctx);
  }

  AddToNavGroup (Group, Cb, Ctx);
  BindRowFocus (Cb, Row);
  BindRowHover (Cb, Row, Ctx);

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
  StyleRow (Row);
  if (Statement->SettingChangedFlag) {
    MarkRowChanged (Row);
  }

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "Numeric");
  lv_obj_set_flex_grow (Label, 1);
  LvglThemeApplyBodyFont (Label);

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
  // Digit-only one-line textarea. ENTER, Tab/arrows, and ESC commit when dirty.
  //
  Ta = lv_textarea_create (Row);
  lv_textarea_set_one_line (Ta, true);
  lv_textarea_set_accepted_chars (Ta, "0123456789");

  Uint64ToAsciiDecimal (MaxVal, MaxStr, sizeof (MaxStr));
  lv_textarea_set_max_length (Ta, (uint32_t)AsciiStrLen (MaxStr));

  Uint64ToAsciiDecimal (CurVal, Initial, sizeof (Initial));
  lv_textarea_set_text (Ta, Initial);
  LvglThemeStyleTextarea (Ta);

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Row, LV_STATE_DISABLED);
    lv_obj_add_state (Ta, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Ta;
    lv_obj_add_event_cb (Ta, OnNumericReady, LV_EVENT_READY, Ctx);
    RegisterEditEntry (LVGL_EDIT_NUMERIC, Ctx);
  }

  //
  // On-screen keyboard in NUMBER mode for the digit-only textarea.
  //
  lv_obj_add_event_cb (Ta, OnTextareaFocused,   LV_EVENT_FOCUSED,   (void *)(UINTN)1);
  lv_obj_add_event_cb (Ta, OnTextareaFocused,   LV_EVENT_CLICKED,   (void *)(UINTN)1);
  lv_obj_add_event_cb (Ta, OnTextareaDefocused, LV_EVENT_DEFOCUSED, NULL);

  AddToNavGroup (Group, Ta, Ctx);
  BindRowFocus (Ta, Row);
  BindRowHover (Ta, Row, Ctx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

/**
  Create one digit-only textarea sub-field for a Date/Time row. ENTER commits
  the whole question via OnDateTimeReady (shared @a DtCtx). @a HelpCtx drives
  the help pane on focus/hover.
**/
STATIC
lv_obj_t *
CreateDateTimeField (
  IN lv_obj_t                *Row,
  IN UINT64                  Value,
  IN UINTN                   Digits,
  IN lv_group_t              *Group,
  IN LVGL_DATETIME_CTX       *DtCtx,
  IN LVGL_STATEMENT_CONTEXT  *HelpCtx
  )
{
  lv_obj_t  *Ta;
  CHAR8     Buf[16];

  Ta = lv_textarea_create (Row);
  lv_textarea_set_one_line (Ta, true);
  lv_textarea_set_accepted_chars (Ta, "0123456789");
  lv_textarea_set_max_length (Ta, (uint32_t)Digits);

  Uint64ToAsciiDecimal (Value, Buf, sizeof (Buf));
  lv_textarea_set_text (Ta, Buf);

  //
  // Roughly size the field to its digit count (glyph ~12 px + padding).
  //
  lv_obj_set_width (Ta, (lv_coord_t)LvglThemeDateTimeFieldWidth (Digits));
  LvglThemeStyleTextarea (Ta);

  lv_obj_add_event_cb (Ta, OnDateTimeReady, LV_EVENT_READY, DtCtx);

  //
  // Reuse the numeric on-screen keyboard (user_data 1 = NUMBER mode).
  //
  lv_obj_add_event_cb (Ta, OnTextareaFocused,   LV_EVENT_FOCUSED,   (void *)(UINTN)1);
  lv_obj_add_event_cb (Ta, OnTextareaFocused,   LV_EVENT_CLICKED,   (void *)(UINTN)1);
  lv_obj_add_event_cb (Ta, OnTextareaDefocused, LV_EVENT_DEFOCUSED, NULL);

  AddToNavGroup (Group, Ta, HelpCtx);
  return Ta;
}

/**
  Render an EFI_IFR_DATE_OP (M/D/Y) or EFI_IFR_TIME_OP (H:M:S) question as a
  row of three digit-only textareas separated by "/" or ":". Editing any
  field and pressing ENTER commits the full date/time value to the browser.
**/
STATIC
VOID
CreateDateTimeWidget (
  lv_obj_t                        *Parent,
  FORM_DISPLAY_ENGINE_STATEMENT   *Statement,
  EFI_HII_HANDLE                  HiiHandle,
  lv_group_t                      *Group,
  BOOLEAN                         IsDate
  )
{
  CHAR8                   *Text;
  lv_obj_t                *Row;
  lv_obj_t                *Label;
  lv_obj_t                *Sep;
  LVGL_DATETIME_CTX       *DtCtx;
  LVGL_STATEMENT_CONTEXT  *HelpCtx;
  UINT64                  C0;
  UINT64                  C1;
  UINT64                  C2;
  CONST CHAR8             *SepText;
  UINTN                   Field2Digits;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Row = lv_obj_create (Parent);
  StyleRow (Row);
  if (Statement->SettingChangedFlag) {
    MarkRowChanged (Row);
  }

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : (IsDate ? "Date" : "Time"));
  lv_obj_set_flex_grow (Label, 1);
  LvglThemeApplyBodyFont (Label);

  if (IsDate) {
    C0           = ClampU64 (Statement->CurrentValue.Value.date.Month, 1, 12);
    C1           = ClampU64 (Statement->CurrentValue.Value.date.Day, 1, 31);
    C2           = ClampU64 (Statement->CurrentValue.Value.date.Year, 1998, 2099);
    SepText      = "/";
    Field2Digits = 4;
  } else {
    C0           = ClampU64 (Statement->CurrentValue.Value.time.Hour, 0, 23);
    C1           = ClampU64 (Statement->CurrentValue.Value.time.Minute, 0, 59);
    C2           = ClampU64 (Statement->CurrentValue.Value.time.Second, 0, 59);
    SepText      = ":";
    Field2Digits = 2;
  }

  DtCtx   = AllocateZeroPool (sizeof (LVGL_DATETIME_CTX));
  HelpCtx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if ((DtCtx == NULL) || (HelpCtx == NULL)) {
    if (DtCtx != NULL) {
      FreePool (DtCtx);
    }

    if (HelpCtx != NULL) {
      FreePool (HelpCtx);
    }

    if (Text != NULL) {
      FreePool (Text);
    }

    return;
  }

  DtCtx->Statement   = Statement;
  DtCtx->IsDate      = IsDate;
  HelpCtx->Statement = Statement;

  DtCtx->Field0 = CreateDateTimeField (Row, C0, 2, Group, DtCtx, HelpCtx);

  Sep = lv_label_create (Row);
  lv_label_set_text (Sep, SepText);
  LvglThemeApplyBodyFont (Sep);

  DtCtx->Field1 = CreateDateTimeField (Row, C1, 2, Group, DtCtx, HelpCtx);

  Sep = lv_label_create (Row);
  lv_label_set_text (Sep, SepText);
  LvglThemeApplyBodyFont (Sep);

  DtCtx->Field2 = CreateDateTimeField (Row, C2, Field2Digits, Group, DtCtx, HelpCtx);

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Row, LV_STATE_DISABLED);
    lv_obj_add_state (DtCtx->Field0, LV_STATE_DISABLED);
    lv_obj_add_state (DtCtx->Field1, LV_STATE_DISABLED);
    lv_obj_add_state (DtCtx->Field2, LV_STATE_DISABLED);
  }

  BindRowFocus (DtCtx->Field0, Row);
  BindRowFocus (DtCtx->Field1, Row);
  BindRowFocus (DtCtx->Field2, Row);
  BindRowHover (DtCtx->Field0, Row, HelpCtx);
  BindRowHover (DtCtx->Field1, Row, HelpCtx);
  BindRowHover (DtCtx->Field2, Row, HelpCtx);

  RegisterEditEntry (LVGL_EDIT_DATETIME, DtCtx);

  if (Text != NULL) {
    FreePool (Text);
  }
}

/**
  LV_EVENT_DEFOCUSED handler for ONE_OF dropdowns.
  If the user moves away (mouse click, Tab, etc.) while a dropdown is in
  keyboard-editing mode, commit the current selection once and clear the
  tracking pointer so it isn't committed a second time.
**/
STATIC
VOID
OnDropdownDefocused (
  lv_event_t  *Event
  )
{
  lv_obj_t  *Dd;

  Dd = lv_event_get_target_obj (Event);
  if (Dd == mEditingDropdown) {
    mEditingDropdown = NULL;
    lv_obj_send_event (Dd, LV_EVENT_VALUE_CHANGED, NULL);
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
  UINTN                     MaxChars;
  CHAR16                    *OptStr16;
  CHAR8                     *OptStr8;
  UINT32                    SelectedIdx;
  UINT32                    CurIdx;
  LVGL_STATEMENT_CONTEXT    *Ctx;

  Text = GetPromptUtf8 (Statement, HiiHandle);

  Row = lv_obj_create (Parent);
  StyleRow (Row);
  if (Statement->SettingChangedFlag) {
    MarkRowChanged (Row);
  }

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "OneOf");
  lv_obj_set_flex_grow (Label, 1);
  LvglThemeApplyBodyFont (Label);

  //
  // Build newline-separated option string for LVGL dropdown.
  //
  OptLen      = 0;
  OptBuf[0]   = '\0';
  MaxChars    = 0;
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

        if (ItemLen > MaxChars) {
          MaxChars = ItemLen;
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
  LvglThemeStyleDropdown (Dd);

  //
  // Pixel-width estimate so the closed button fits its longest option:
  // ~10 px per glyph for the body font + slack for chevron and padding.
  // LV_SIZE_CONTENT collapses the dropdown to just the chevron, so we use
  // an explicit width instead.
  //
  lv_obj_set_width (Dd, (lv_coord_t)LvglThemeDropdownWidth (MaxChars));

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Row, LV_STATE_DISABLED);
    lv_obj_add_state (Dd, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Dd;
    lv_obj_add_event_cb (Dd, OnDropdownChanged, LV_EVENT_VALUE_CHANGED, Ctx);
  }

  lv_obj_add_event_cb (Dd, OnDropdownOpened, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb (Dd, OnDropdownDefocused, LV_EVENT_DEFOCUSED, NULL);

  AddToNavGroup (Group, Dd, Ctx);
  BindRowFocus (Dd, Row);
  BindRowHover (Dd, Row, Ctx);

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
  // Outer panel -- vertical flex column holding header + one row per entry.
  //
  Panel = lv_obj_create (Parent);
  lv_obj_set_size (Panel, LV_PCT (100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow (Panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all (Panel, THEME_PAD_ROW, 0);
  lv_obj_set_style_border_width (Panel, 0, 0);
  lv_obj_set_style_bg_opa (Panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_text_color (Panel, lv_color_hex (
      Grayout ? THEME_COLOR_ROW_TEXT_DISABLED : THEME_COLOR_TEXT_PRIMARY), 0);
  THEME_APPLY_BODY_FONT (Panel);

  Header = lv_label_create (Panel);
  lv_label_set_text (Header, PromptText != NULL ? PromptText : "Ordered List");
  THEME_APPLY_BODY_FONT (Header);

  for (i = 0; i < ActiveCount; i++) {
    Value = GetArrayData (Statement->CurrentValue.Buffer, ValueType, i);

    //
    // Find the option whose Value matches this slot. Read the option's
    // Value at its native width -- IFR stores it at ValueType size, so
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
    lv_obj_set_style_pad_all (Row, THEME_PAD_ROW_TIGHT, 0);
    lv_obj_set_style_border_width (Row, 0, 0);
    lv_obj_set_style_bg_opa (Row, LV_OPA_TRANSP, 0);

    Label = lv_label_create (Row);
    lv_label_set_text (Label, OptStr8 != NULL ? OptStr8 : "?");
    lv_obj_set_flex_grow (Label, 1);
    lv_obj_set_style_text_color (Label, lv_color_hex (
        Grayout ? THEME_COLOR_ROW_TEXT_DISABLED : THEME_COLOR_TEXT_PRIMARY), 0);
    LvglThemeApplyBodyFont (Label);

    if (OptStr8 != NULL) {
      FreePool (OptStr8);
    }

    //
    // Up button.
    //
    UpBtn    = lv_button_create (Row);
    BtnLabel = lv_label_create (UpBtn);
    lv_label_set_text (BtnLabel, LV_SYMBOL_UP);
    LvglThemeStyleButtonLabel (UpBtn, BtnLabel);

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
    DownBtn  = lv_button_create (Row);
    BtnLabel = lv_label_create (DownBtn);
    lv_label_set_text (BtnLabel, LV_SYMBOL_DOWN);
    LvglThemeStyleButtonLabel (DownBtn, BtnLabel);

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
  StyleRow (Row);
  if (Statement->SettingChangedFlag) {
    MarkRowChanged (Row);
  }

  Label = lv_label_create (Row);
  lv_label_set_text (Label, Text != NULL ? Text : "String");
  lv_obj_set_flex_grow (Label, 1);
  LvglThemeApplyBodyFont (Label);

  Ta = lv_textarea_create (Row);
  lv_textarea_set_one_line (Ta, true);

  if (Statement->OpCode->OpCode == EFI_IFR_PASSWORD_OP) {
    lv_textarea_set_password_mode (Ta, true);
  } else {
    //
    // For string fields the current value is stored as an HII string token
    // in CurrentValue.Value.string -- NOT in CurrentValue.Buffer (which is
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

  LvglThemeStyleTextarea (Ta);

  if (Statement->Attribute & HII_DISPLAY_GRAYOUT) {
    lv_obj_add_state (Row, LV_STATE_DISABLED);
    lv_obj_add_state (Ta, LV_STATE_DISABLED);
  }

  Ctx = AllocateZeroPool (sizeof (LVGL_STATEMENT_CONTEXT));
  if (Ctx != NULL) {
    Ctx->Statement = Statement;
    Ctx->Widget    = Ta;
    Ctx->HiiHandle = HiiHandle;
    lv_obj_add_event_cb (Ta, OnStringReady, LV_EVENT_READY, Ctx);
    RegisterEditEntry (LVGL_EDIT_STRING, Ctx);
  }

  //
  // Show the on-screen keyboard when the textarea is focused or clicked.
  // user_data = 0 -> text mode (also covers password, since the keyboard
  // forwards keys; the textarea's password_mode hides the rendering).
  //
  lv_obj_add_event_cb (Ta, OnTextareaFocused,   LV_EVENT_FOCUSED,   (void *)(UINTN)0);
  lv_obj_add_event_cb (Ta, OnTextareaFocused,   LV_EVENT_CLICKED,   (void *)(UINTN)0);
  lv_obj_add_event_cb (Ta, OnTextareaDefocused, LV_EVENT_DEFOCUSED, NULL);

  AddToNavGroup (Group, Ta, Ctx);
  BindRowFocus (Ta, Row);
  BindRowHover (Ta, Row, Ctx);

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

  Btn = lv_button_create (Parent);
  StyleRow (Btn);
  lv_obj_set_style_text_align (Btn, LV_TEXT_ALIGN_LEFT, 0);

  Label = lv_label_create (Btn);
  lv_label_set_text (Label, Text != NULL ? Text : "Goto");
  lv_obj_align (Label, LV_ALIGN_LEFT_MID, 0, 0);
  LvglThemeApplyBodyFont (Label);

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

      case EFI_IFR_DATE_OP:
        CreateDateTimeWidget (Session->Screen, Statement, HiiHandle, Session->Group, TRUE);
        break;

      case EFI_IFR_TIME_OP:
        CreateDateTimeWidget (Session->Screen, Statement, HiiHandle, Session->Group, FALSE);
        break;

      case EFI_IFR_GUID_OP:
        //
        // Front-page banner lines (model / BIOS version / CPU / memory /
        // battery) are Tiano GUIDed EFI_IFR_EXTEND_OP_BANNER opcodes. The
        // text engine drew these via CustomizedDisplayLib; render them here
        // so the front-page banner isn't lost. All other GUIDed opcodes
        // (labels, class/subclass) are display-only and safely ignored.
        //
        if (CompareGuid (
              (EFI_GUID *)((UINT8 *)Statement->OpCode + sizeof (EFI_IFR_OP_HEADER)),
              &gEfiIfrTianoGuid
              ) &&
            (((EFI_IFR_GUID_LABEL *)Statement->OpCode)->ExtendOpCode == EFI_IFR_EXTEND_OP_BANNER))
        {
          CreateBannerWidget (Session->Screen, Statement, HiiHandle);
        }

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
      DEBUG ((DEBUG_ERROR, "LvglRenderer: UefiLvglInit failed -- %r\n", Status));
      return Status;
    }

    mLvglReady = TRUE;
  }

  //
  // Clear the console and hide cursor for graphical mode.
  //
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  //
  // Tear down any chrome-owned timers from the previous form before we
  // build a new one (LVGL timers are not children of the screen object).
  //
  AptioChromeTeardown ();

  //
  // Set up session state.
  //
  ZeroMem (&mSession, sizeof (mSession));
  mSession.FormData       = FormData;
  mSession.UserInput      = UserInputData;
  mSession.ExitRequested  = FALSE;
  mNavCount               = 0;
  mEditRegistryCount      = 0;
  mEditingDropdown        = NULL;

  //
  // Drop any banner-refresh timer/registry from the previous form before the
  // new form's banners are (re)registered in BuildFormWidgets.
  //
  if (mBannerTimer != NULL) {
    lv_timer_delete (mBannerTimer);
    mBannerTimer = NULL;
  }

  mBannerCount = 0;

  ZeroMem (UserInputData, sizeof (USER_INPUT));

  {
    LVGL_UI_CONFIG_VARSTORE_DATA  UiConfig;

    LvglUiConfigLoad (&UiConfig);
    mSubtitleShowsDeviceModel = (UiConfig.SubtitleShowsDeviceModel != 0);
  }

  //
  // Create a new screen and the AMI Aptio-style chrome around it.
  // AptioBuildChrome returns the inner content panel; form widgets are
  // appended to that panel below.
  //
  mSession.Screen = lv_obj_create (NULL);
  ContentPanel    = AptioBuildChrome (mSession.Screen, FormData);
  mContentPanel   = ContentPanel;

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
  // If the form registered any banner lines, poll them at 1 Hz so live values
  // (e.g. battery status updated via HiiSetString) stay current on screen.
  //
  if (mBannerCount > 0) {
    mBannerTimer = lv_timer_create (BannerRefreshTimerCb, 1000, NULL);
  }

  //
  // Bottom-docked on-screen keyboard. Created on the outer Screen so it
  // overlays the chrome; hidden until a String/Password/Numeric textarea
  // gains focus (see OnTextareaFocused).
  //
  mSession.OnScreenKb = lv_keyboard_create (mSession.Screen);
  lv_obj_set_size (mSession.OnScreenKb, LV_PCT (100), LV_PCT (40));
  lv_obj_align (mSession.OnScreenKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  LvglThemeStyleKeyboard (mSession.OnScreenKb);
  lv_obj_set_hidden (mSession.OnScreenKb, true);
  lv_obj_add_event_cb (mSession.OnScreenKb, OnKeyboardCancel, LV_EVENT_CANCEL, NULL);
  //
  // Don't add the keyboard to the form's nav group: it's mouse-only.
  // Arrow-key navigation between form fields keeps working unchanged.
  //

  //
  // Load the screen.
  //
  lv_screen_load (mSession.Screen);

  //
  // Restore keyboard focus. Prefer the destination field after a nav-time
  // commit; otherwise use the browser highlight.
  //
  lv_obj_update_layout (mSession.Screen);
  if (mRestoreFocusPending) {
    if (mRestoreFocusNavIdx < mNavCount) {
      lv_group_focus_obj (mNavList[mRestoreFocusNavIdx]);
    }

    mRestoreFocusPending = FALSE;
  } else if (FormData->HighLightedStatement != NULL) {
    UINTN  FocusIdx;

    for (FocusIdx = 0; FocusIdx < mNavCount; FocusIdx++) {
      if (mNavStatement[FocusIdx] == FormData->HighLightedStatement) {
        lv_group_focus_obj (mNavList[FocusIdx]);
        break;
      }
    }
  }

  //
  // Finish a deferred leave after flushing the focused field on ESC/back.
  //
  if (CompletePendingLeaveIfReady ()) {
    lv_uefi_keypad_drain ();
    return EFI_SUCCESS;
  }

  //
  // LVGL event loop -- run until user makes a selection or presses ESC.
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
        RequestUserExit (mPopupResult, mPendingDefaultId, NULL);
      }

      mPopupResult = LVGL_POPUP_PENDING;
    }
  }

  //
  // We exit on a key PRESS event (ENTER to commit, ESC to leave). LVGL
  // hasn't yet seen the matching RELEASE -- without draining, LVGL would
  // deliver the pending LV_EVENT_CLICKED to whatever widget is focused when
  // FormDisplay() is re-invoked, triggering a spurious submenu navigation.
  // lv_indev_wait_release() is NOT suitable here because it eats the entire
  // next keystroke (it waits for a RELEASED poll, which swallows any new
  // key the user presses before an idle poll).
  //
  lv_uefi_keypad_drain ();

  DEBUG ((DEBUG_INFO, "LvglRenderer: exiting event loop -- Action=0x%x\n", UserInputData->Action));

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
  // Restore keyboard indev to the form group (still valid -- ExitDisplay
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

//
// ---- EFI_HII_POPUP_PROTOCOL backing popup ----
//

/**
  Dismiss the HII popup, recording the chosen selection.
**/
STATIC
VOID
CloseHiiPopup (
  IN INT32  Selection
  )
{
  mHiiPopupSel = Selection;
  if (mHiiPopupOverlay != NULL) {
    lv_obj_delete (mHiiPopupOverlay);
    mHiiPopupOverlay = NULL;
    mHiiPopupFirst   = NULL;
    mHiiPopupLast    = NULL;
  }
}

/**
  HII popup button click -- user_data encodes (selection + 1).
**/
STATIC
VOID
OnHiiPopupBtn (
  lv_event_t  *Event
  )
{
  CloseHiiPopup ((INT32)(UINTN)lv_event_get_user_data (Event) - 1);
}

/**
  HII popup keyboard handling: Y/N shortcuts, ESC (cancel/no), and
  LEFT/RIGHT/UP/DOWN to move between buttons without escaping the popup.
**/
STATIC
VOID
OnHiiPopupKey (
  lv_event_t  *Event
  )
{
  lv_key_t  Key;
  lv_obj_t  *Focused;

  Key = lv_indev_get_key (lv_indev_active ());

  if (((Key == 'Y') || (Key == 'y')) && (mHiiPopupYesSel != LVGL_HII_POPUP_PENDING)) {
    CloseHiiPopup (mHiiPopupYesSel);
    lv_event_stop_processing (Event);
    return;
  }

  if (((Key == 'N') || (Key == 'n')) && (mHiiPopupNoSel != LVGL_HII_POPUP_PENDING)) {
    CloseHiiPopup (mHiiPopupNoSel);
    lv_event_stop_processing (Event);
    return;
  }

  if (Key == LV_KEY_ESC) {
    CloseHiiPopup (mHiiPopupEscSel);
    lv_event_stop_processing (Event);
    return;
  }

  if (mHiiPopupGroup == NULL) {
    return;
  }

  if ((Key == LV_KEY_LEFT) || (Key == LV_KEY_UP)) {
    Focused = lv_group_get_focused (mHiiPopupGroup);
    if (Focused != mHiiPopupFirst) {
      lv_group_focus_prev (mHiiPopupGroup);
    }

    lv_event_stop_processing (Event);
  } else if ((Key == LV_KEY_RIGHT) || (Key == LV_KEY_DOWN)) {
    Focused = lv_group_get_focused (mHiiPopupGroup);
    if (Focused != mHiiPopupLast) {
      lv_group_focus_next (mHiiPopupGroup);
    }

    lv_event_stop_processing (Event);
  }
}

/**
  Add one button to the HII popup button row.
**/
STATIC
lv_obj_t *
AddHiiPopupButton (
  IN lv_obj_t     *Row,
  IN lv_group_t   *Group,
  IN CONST CHAR8  *Label,
  IN INT32        Selection
  )
{
  lv_obj_t  *Btn;
  lv_obj_t  *Lbl;

  Btn = lv_button_create (Row);
  Lbl = lv_label_create (Btn);
  lv_label_set_text (Lbl, Label);
  LvglThemeStyleButtonLabel (Btn, Lbl);
  lv_obj_add_event_cb (Btn, OnHiiPopupBtn, LV_EVENT_CLICKED, (void *)(UINTN)(Selection + 1));
  lv_obj_add_event_cb (Btn, OnHiiPopupKey, LV_EVENT_KEY, NULL);
  lv_group_add_obj (Group, Btn);
  return Btn;
}

/**
  Build the modal HII popup UI on the top layer.
**/
STATIC
VOID
ShowHiiPopup (
  IN lv_group_t           *Group,
  IN CONST CHAR8          *Title,
  IN CONST CHAR8          *Message,
  IN EFI_HII_POPUP_TYPE   PopupType
  )
{
  LVGL_DIALOG_SHELL  Shell;

  mHiiPopupSel    = LVGL_HII_POPUP_PENDING;
  mHiiPopupYesSel = LVGL_HII_POPUP_PENDING;
  mHiiPopupNoSel  = LVGL_HII_POPUP_PENDING;
  mHiiPopupEscSel = LVGL_HII_POPUP_PENDING;
  mHiiPopupGroup  = Group;

  CreateDialogShell (&Shell, Title);
  mHiiPopupOverlay = Shell.Overlay;
  CreateDialogMessageLabel (Shell.Card, Message, TRUE);

  switch (PopupType) {
    case EfiHiiPopupTypeOk:
      mHiiPopupFirst  = AddHiiPopupButton (Shell.BtnRow, Group, "OK", EfiHiiPopupSelectionOk);
      mHiiPopupLast   = mHiiPopupFirst;
      mHiiPopupEscSel = EfiHiiPopupSelectionOk;
      break;

    case EfiHiiPopupTypeOkCancel:
      mHiiPopupFirst  = AddHiiPopupButton (Shell.BtnRow, Group, "OK", EfiHiiPopupSelectionOk);
      mHiiPopupLast   = AddHiiPopupButton (Shell.BtnRow, Group, "Cancel", EfiHiiPopupSelectionCancel);
      mHiiPopupEscSel = EfiHiiPopupSelectionCancel;
      break;

    case EfiHiiPopupTypeYesNoCancel:
      mHiiPopupFirst  = AddHiiPopupButton (Shell.BtnRow, Group, "Yes (Y)", EfiHiiPopupSelectionYes);
      (VOID)AddHiiPopupButton (Shell.BtnRow, Group, "No (N)", EfiHiiPopupSelectionNo);
      mHiiPopupLast   = AddHiiPopupButton (Shell.BtnRow, Group, "Cancel", EfiHiiPopupSelectionCancel);
      mHiiPopupYesSel = EfiHiiPopupSelectionYes;
      mHiiPopupNoSel  = EfiHiiPopupSelectionNo;
      mHiiPopupEscSel = EfiHiiPopupSelectionCancel;
      break;

    case EfiHiiPopupTypeYesNo:
    default:
      mHiiPopupFirst  = AddHiiPopupButton (Shell.BtnRow, Group, "Yes (Y)", EfiHiiPopupSelectionYes);
      mHiiPopupLast   = AddHiiPopupButton (Shell.BtnRow, Group, "No (N)", EfiHiiPopupSelectionNo);
      mHiiPopupYesSel = EfiHiiPopupSelectionYes;
      mHiiPopupNoSel  = EfiHiiPopupSelectionNo;
      mHiiPopupEscSel = EfiHiiPopupSelectionNo;
      break;
  }

  if (mHiiPopupFirst != NULL) {
    lv_group_focus_obj (mHiiPopupFirst);
  }
}

EFI_STATUS
EFIAPI
LvglHiiCreatePopup (
  IN  EFI_HII_POPUP_STYLE      PopupStyle,
  IN  EFI_HII_POPUP_TYPE       PopupType,
  IN  EFI_HII_HANDLE           HiiHandle,
  IN  EFI_STRING_ID            Message,
  OUT EFI_HII_POPUP_SELECTION  *UserSelection OPTIONAL
  )
{
  lv_group_t      *PopupGroup;
  lv_indev_t      *Indev;
  CHAR16          *Ucs2;
  CHAR8           *Utf8;
  CONST CHAR8     *Title;
  extern BOOLEAN  mTickSupport;

  if (!mLvglReady) {
    return EFI_DEVICE_ERROR;
  }

  Ucs2 = HiiGetString (HiiHandle, Message, NULL);
  if (Ucs2 == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Utf8 = Ucs2ToUtf8 (Ucs2);
  FreePool (Ucs2);
  if (Utf8 == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  switch (PopupStyle) {
    case EfiHiiPopupStyleError:
      Title = "Error";
      break;
    case EfiHiiPopupStyleWarning:
      Title = "Warning";
      break;
    default:
      Title = "Confirm";
      break;
  }

  //
  // Isolate the keypad indev on a dedicated group so keys drive the popup
  // buttons, not the (still-loaded) form behind it.
  //
  PopupGroup = lv_group_create ();
  Indev      = NULL;
  while ((Indev = lv_indev_get_next (Indev)) != NULL) {
    if (lv_indev_get_type (Indev) == LV_INDEV_TYPE_KEYPAD) {
      lv_indev_set_group (Indev, PopupGroup);
    }
  }

  ShowHiiPopup (PopupGroup, Title, Utf8, PopupType);

  //
  // Drain pending keystrokes so the popup isn't dismissed by a stale key.
  //
  lv_uefi_keypad_drain ();

  while (mHiiPopupSel == LVGL_HII_POPUP_PENDING) {
    lv_timer_handler ();
    gBS->Stall (10 * 1000);
    if (!mTickSupport) {
      lv_tick_inc (10);
    }
  }

  //
  // Restore the keypad indev to the form group and drain the confirming key.
  //
  Indev = NULL;
  while ((Indev = lv_indev_get_next (Indev)) != NULL) {
    if (lv_indev_get_type (Indev) == LV_INDEV_TYPE_KEYPAD) {
      lv_indev_set_group (Indev, mSession.Group);
    }
  }

  lv_group_delete (PopupGroup);
  mHiiPopupGroup = NULL;
  lv_uefi_keypad_drain ();

  if (UserSelection != NULL) {
    *UserSelection = (EFI_HII_POPUP_SELECTION)mHiiPopupSel;
  }

  FreePool (Utf8);
  return EFI_SUCCESS;
}

VOID
EFIAPI
LvglRendererCleanup (
  VOID
  )
{
  AptioChromeTeardown ();

  if (mBannerTimer != NULL) {
    lv_timer_delete (mBannerTimer);
    mBannerTimer = NULL;
  }

  mBannerCount = 0;

  if (mSession.Group != NULL) {
    lv_group_delete (mSession.Group);
    mSession.Group = NULL;
  }

  if (mSession.Screen != NULL) {
    lv_obj_delete (mSession.Screen);
    mSession.Screen = NULL;
  }

  mContentPanel = NULL;

  //
  // Note: LVGL_STATEMENT_CONTEXT objects were allocated with AllocateZeroPool
  // and are cleaned up when the screen is deleted (LVGL deletes children
  // recursively). However, user_data pointers are NOT freed by LVGL.
  // For a production implementation, we'd track and free them.
  // For now this is acceptable as the browser re-calls FormDisplay() in a loop
  // and the DXE pool is reclaimed on reboot.
  //
}
