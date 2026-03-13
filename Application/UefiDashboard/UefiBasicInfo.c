/** @file
  This sample application bases on LvglLib.

  Copyright (c) 2025, Yang Gang. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UefiDashboardCommon.h"


LV_ATTRIBUTE_MEM_ALIGN UINT8 *mLogoData = NULL;


static void uefilogo_delete_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_DELETE && mLogoData != NULL) {
        FreePool (mLogoData);
        mLogoData = NULL;
    }
}

lv_obj_t *
DisplayUefiLogo (
  EFI_IMAGE_INPUT *EfiImageLogo,
  lv_obj_t        *parent
  )
{
  UINT32 Width, Height, Stride, DataSize;

  Width = EfiImageLogo->Width;
  Height = EfiImageLogo->Height;
  Stride = EfiImageLogo->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  DataSize = EfiImageLogo->Width * EfiImageLogo->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  mLogoData = AllocateZeroPool (DataSize);
  CopyMem (mLogoData, EfiImageLogo->Bitmap, DataSize);

  for (UINTN Index = 3; Index < DataSize; Index += 4) {
    mLogoData[Index] = 0xff;
  }

  static lv_draw_buf_t draw_buf;
  lv_draw_buf_init(&draw_buf, Width, Height, LV_COLOR_FORMAT_ARGB8888, Stride, mLogoData, DataSize);

  static lv_image_dsc_t img_dsc;
  lv_draw_buf_to_image(&draw_buf, &img_dsc);

  lv_obj_t * img1 = lv_image_create(parent);
  lv_obj_add_event_cb(img1, uefilogo_delete_event_cb, LV_EVENT_DELETE, NULL);
  lv_image_set_src(img1, &img_dsc);

  return img1;
}

lv_obj_t *
UefiLogoInfo (
  lv_obj_t * parent
  )
{
  EFI_STATUS                             Status;
  EDKII_PLATFORM_LOGO_PROTOCOL           *PlatformLogo;
  EDKII_PLATFORM_LOGO_DISPLAY_ATTRIBUTE  Attribute;
  INTN                                   OffsetX;
  INTN                                   OffsetY;
  UINT32                                 Instance;
  EFI_IMAGE_INPUT                        Image;

  Instance = 0;

  Status = gBS->LocateProtocol (&gEdkiiPlatformLogoProtocolGuid, NULL, (VOID **)&PlatformLogo);
  if (!EFI_ERROR (Status)) {
    //
    // Get EfiImageLogo from PlatformLogo protocol.
    //
    Status = PlatformLogo->GetImage (
                             PlatformLogo,
                             &Instance,
                             &Image,
                             &Attribute,
                             &OffsetX,
                             &OffsetY
                             );
  }

  return DisplayUefiLogo(&Image, parent);
}


/**
  Return SMBIOS string for the given string number.

  @param[in] Smbios         Pointer to SMBIOS structure.
  @param[in] StringNumber   String number to return. -1 is used to skip all strings and
                            point to the next SMBIOS structure.

  @return Pointer to string, or pointer to next SMBIOS strcuture if StringNumber == -1
**/
STATIC
CHAR8 *
LibGetSmbiosString (
  IN  SMBIOS_STRUCTURE_POINTER  *Smbios,
  IN  UINT16                    StringNumber
  )
{
  UINT16  Index;
  CHAR8   *String;

  ASSERT (Smbios != NULL);

  //
  // Skip over formatted section
  //
  String = (CHAR8 *)(Smbios->Raw + Smbios->Hdr->Length);

  //
  // Look through unformated section
  //
  for (Index = 1; Index <= StringNumber; Index++) {
    if (StringNumber == Index) {
      return String;
    }

    //
    // Skip string
    //
    for ( ; *String != 0; String++) {
    }

    String++;

    if (*String == 0) {
      //
      // If double NULL then we are done.
      //  Return pointer to next structure in Smbios.
      //  if you pass in a -1 you will always get here
      //
      Smbios->Raw = (UINT8 *)++String;
      return NULL;
    }
  }

  return NULL;
}


static void basic_info_draw_event_cb(lv_event_t * e)
{
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    /*If the cells are drawn...*/
    if(base_dsc->part == LV_PART_ITEMS) {
        uint32_t row = base_dsc->id1;
        uint32_t col = base_dsc->id2;

        /*Make the texts in the first cell center aligned*/
        if(row == 0) {
            lv_draw_label_dsc_t * label_draw_dsc = lv_draw_task_get_label_dsc(draw_task);
            if(label_draw_dsc) {
                label_draw_dsc->align = LV_TEXT_ALIGN_CENTER;
                label_draw_dsc->font = &lv_font_montserrat_24;
            }
        }
        /*In the first column align the texts to the right*/
        else if(col == 0) {
            lv_draw_label_dsc_t * label_draw_dsc = lv_draw_task_get_label_dsc(draw_task);
            if(label_draw_dsc) {
                label_draw_dsc->align = LV_TEXT_ALIGN_LEFT;
            }
        }

        lv_draw_fill_dsc_t * fill_draw_dsc = lv_draw_task_get_fill_dsc(draw_task);
        if(fill_draw_dsc) {
            fill_draw_dsc->opa = LV_OPA_TRANSP;
        }
    }
}


VOID
UefiBiosInfo (
  lv_obj_t                  *parent,
  EFI_SMBIOS_PROTOCOL       *Smbios
  )
{
  EFI_STATUS                Status;
  EFI_SMBIOS_HANDLE         SmbiosHandle;
  EFI_SMBIOS_TYPE           SmbiosType;
  EFI_SMBIOS_TABLE_HEADER   *SmbiosHdr;
  SMBIOS_STRUCTURE_POINTER  SmbiosStructureP;
  CHAR8                     *BiosVendor, *BiosVersion;

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  SmbiosType = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
  Status = Smbios->GetNext(Smbios, &SmbiosHandle, &SmbiosType, &SmbiosHdr, NULL);
  ASSERT(!EFI_ERROR(Status));
  SmbiosStructureP.Hdr = SmbiosHdr;
  BiosVendor = LibGetSmbiosString(&SmbiosStructureP, SmbiosStructureP.Type0->Vendor);
  BiosVersion = LibGetSmbiosString(&SmbiosStructureP, SmbiosStructureP.Type0->BiosVersion);

  lv_obj_t * table = lv_table_create(parent);
  lv_obj_set_width(table, lv_pct(45));
  lv_obj_set_style_max_height(table, lv_pct(50), 0);
  lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
  lv_obj_add_event_cb(table, basic_info_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

  lv_obj_set_style_border_width(table, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(table, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_FULL, LV_PART_MAIN);

  lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(table, lv_color_black(), LV_PART_ITEMS);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_FULL, LV_PART_ITEMS);

  lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, 0);

  lv_obj_update_layout(table);
  const int32_t col_w = lv_obj_get_content_width(table) / 2;

  lv_table_set_column_width(table, 0, col_w);
  lv_table_set_column_width(table, 1, col_w);

  lv_table_set_cell_value(table, 0, 0, "BIOS Info");
  lv_table_set_cell_value(table, 1, 0, "Vendor");
  lv_table_set_cell_value(table, 2, 0, "Version");

  lv_table_set_cell_value(table, 0, 1, " ");
  lv_table_set_cell_value(table, 1, 1, BiosVendor);
  lv_table_set_cell_value(table, 2, 1, BiosVersion);

  lv_table_set_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
}


VOID
UefiCpuInfo (
  lv_obj_t                  *parent,
  EFI_SMBIOS_PROTOCOL       *Smbios
  )
{
  EFI_STATUS                Status;
  EFI_SMBIOS_HANDLE         SmbiosHandle;
  EFI_SMBIOS_TYPE           SmbiosType;
  EFI_SMBIOS_TABLE_HEADER   *SmbiosHdr;
  SMBIOS_STRUCTURE_POINTER  SmbiosStructureP;
  UINT8                     SocketCount = 0;
  UINT16                    CpuSpeed = 0;
  CHAR8                     *Manufacturer = NULL, *Version = NULL;

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  SmbiosType = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
  while (TRUE) {
    Status = Smbios->GetNext(Smbios, &SmbiosHandle, &SmbiosType, &SmbiosHdr, NULL);
    if (EFI_ERROR (Status)) {
      break;
    }
    SmbiosStructureP.Hdr = SmbiosHdr;

    if (SmbiosStructureP.Type4->CoreCount == 0) {
      continue;
    }

    if (Manufacturer == NULL) {
      Manufacturer = LibGetSmbiosString(&SmbiosStructureP, SmbiosStructureP.Type4->ProcessorManufacturer);
    }
    if (Version == NULL) {
      Version = LibGetSmbiosString(&SmbiosStructureP, SmbiosStructureP.Type4->ProcessorVersion);
    }
    if (CpuSpeed == 0) {
      CpuSpeed = SmbiosStructureP.Type4->CurrentSpeed;
    }
    SocketCount++;
  }

  lv_obj_t * table = lv_table_create(parent);
  lv_obj_set_width(table, lv_pct(45));
  lv_obj_set_style_max_height(table, lv_pct(60), 0);
  lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
  lv_obj_add_event_cb(table, basic_info_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

  lv_obj_set_style_border_width(table, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(table, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_FULL, LV_PART_MAIN);

  lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(table, lv_color_black(), LV_PART_ITEMS);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_FULL, LV_PART_ITEMS);

  lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, 0);

  lv_obj_update_layout(table);
  const int32_t col_w = lv_obj_get_content_width(table) / 2;

  lv_table_set_column_width(table, 0, col_w);
  lv_table_set_column_width(table, 1, col_w);

  lv_table_set_cell_value(table, 0, 0, "CPU Info");
  lv_table_set_cell_value(table, 1, 0, "Manufacturer");
  lv_table_set_cell_value(table, 2, 0, "Version");
  lv_table_set_cell_value(table, 3, 0, "Speed");
  lv_table_set_cell_value(table, 4, 0, "SocketCount");

  lv_table_set_cell_value(table, 0, 1, " ");
  lv_table_set_cell_value(table, 1, 1, Manufacturer);
  lv_table_set_cell_value(table, 2, 1, Version);
  lv_table_set_cell_value_fmt(table, 3, 1, "%dMHz", CpuSpeed);
  lv_table_set_cell_value_fmt(table, 4, 1, "%d", SocketCount);

  lv_table_set_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
}


VOID
UefiDimmInfo (
  lv_obj_t                  *parent,
  EFI_SMBIOS_PROTOCOL       *Smbios
  )
{
  EFI_STATUS                Status;
  EFI_SMBIOS_HANDLE         SmbiosHandle;
  EFI_SMBIOS_TYPE           SmbiosType;
  EFI_SMBIOS_TABLE_HEADER   *SmbiosHdr;
  SMBIOS_STRUCTURE_POINTER  SmbiosStructureP;
  UINT8                     DimmCount = 0;
  UINT16                    DimmSpeed = 0;
  UINT64                    DimmSize = 0;
  UINT32                    ExtendedSize = 0;
  CHAR8                     *Manufacturer = NULL, *SerialNumber = NULL;
  CHAR8                     DimmSizeStr[10];

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  SmbiosType = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  while (TRUE) {
    Status = Smbios->GetNext(Smbios, &SmbiosHandle, &SmbiosType, &SmbiosHdr, NULL);
    if (EFI_ERROR (Status)) {
      break;
    }
    SmbiosStructureP.Hdr = SmbiosHdr;

    if (SmbiosStructureP.Type17->Size == 0) {
      continue;
    }

    if (Manufacturer == NULL) {
      Manufacturer = LibGetSmbiosString(&SmbiosStructureP, SmbiosStructureP.Type17->Manufacturer);
    }
    if (SerialNumber == NULL) {
      SerialNumber = LibGetSmbiosString(&SmbiosStructureP, SmbiosStructureP.Type17->SerialNumber);
    }
    if (DimmSpeed == 0) {
      DimmSpeed = SmbiosStructureP.Type17->ConfiguredMemoryClockSpeed;
    }
    if (DimmSize == 0) {
      DimmSize = SmbiosStructureP.Type17->Size;
    }
    if (ExtendedSize == 0) {
      ExtendedSize = SmbiosStructureP.Type17->ExtendedSize;
    }
    DimmCount++;
  }

  ZeroMem (DimmSizeStr, sizeof (DimmSizeStr));
  if (DimmSize == 0xFFFF || DimmSize == 0x0) {
    //
    // Using T19
    //
    DimmSize = 0;
    SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
    SmbiosType = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
    while (TRUE) {
      Status = Smbios->GetNext(Smbios, &SmbiosHandle, &SmbiosType, &SmbiosHdr, NULL);
      if (EFI_ERROR (Status)) {
        break;
      }
      SmbiosStructureP.Hdr = SmbiosHdr;
      if (SmbiosStructureP.Type19->StartingAddress != 0xFFFFFFFF ) {
        DimmSize += RShiftU64 (
                             SmbiosStructureP.Type19->EndingAddress -
                             SmbiosStructureP.Type19->StartingAddress + 1,
                             10
                             );
      } else {
        DimmSize += RShiftU64 (
                             SmbiosStructureP.Type19->ExtendedEndingAddress -
                             SmbiosStructureP.Type19->ExtendedStartingAddress + 1,
                             20
                             );
      }
    }
    AsciiSPrint (DimmSizeStr, sizeof (DimmSizeStr), "%dMB ", DimmSize);
  } else if (DimmSize == 0x7FFF) {
    AsciiSPrint (DimmSizeStr, sizeof (DimmSizeStr), "%dMB ", (ExtendedSize & (~BIT31)));
  } else {
    if (DimmSize & BIT15) {
      DimmSize = (DimmSize&(~BIT15)) >> 10;
    }
    AsciiSPrint (DimmSizeStr, sizeof (DimmSizeStr), "%dMB ", DimmSize);
  }

  lv_obj_t * table = lv_table_create(parent);
  lv_obj_set_width(table, lv_pct(45));
  lv_obj_set_style_max_height(table, lv_pct(70), 0);
  lv_obj_add_flag(table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
  lv_obj_add_event_cb(table, basic_info_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

  lv_obj_set_style_border_width(table, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(table, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_FULL, LV_PART_MAIN);

  lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(table, lv_color_black(), LV_PART_ITEMS);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_FULL, LV_PART_ITEMS);

  lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, 0);

  lv_obj_update_layout(table);
  const int32_t col_w = lv_obj_get_content_width(table) / 2;

  lv_table_set_column_width(table, 0, col_w);
  lv_table_set_column_width(table, 1, col_w);

  lv_table_set_cell_value(table, 0, 0, "Memory Info");
  lv_table_set_cell_value(table, 1, 0, "Manufacturer");
  lv_table_set_cell_value(table, 2, 0, "DimmSize");
  lv_table_set_cell_value(table, 3, 0, "SerialNumber");
  lv_table_set_cell_value(table, 4, 0, "DimmSpeed");
  lv_table_set_cell_value(table, 5, 0, "DimmCount");

  lv_table_set_cell_value(table, 0, 1, " ");
  lv_table_set_cell_value(table, 1, 1, Manufacturer);
  lv_table_set_cell_value(table, 2, 1, DimmSizeStr);
  lv_table_set_cell_value(table, 3, 1, (SerialNumber != NULL) ? SerialNumber : "Null SN");
  lv_table_set_cell_value_fmt(table, 4, 1, "%dMT/s", DimmSpeed);
  lv_table_set_cell_value_fmt(table, 5, 1, "%d", DimmCount);

  lv_table_set_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
}


VOID
UefiBasicInfo (
  lv_obj_t * parent
  )
{
  EFI_STATUS                Status;
  EFI_SMBIOS_PROTOCOL       *Smbios;

  UefiLogoInfo (parent);

  Status = gBS->LocateProtocol(&gEfiSmbiosProtocolGuid, NULL, (VOID**)&Smbios);
  if (!EFI_ERROR (Status)) {
    lv_obj_t * cont_info = lv_obj_create(parent);
    lv_obj_set_size(cont_info, lv_pct(90), lv_pct(90));
    lv_obj_align(cont_info, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(cont_info, LV_FLEX_FLOW_COLUMN_WRAP);
    lv_obj_set_style_bg_opa(cont_info, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_opa(cont_info, LV_OPA_TRANSP, LV_PART_MAIN);

    UefiBiosInfo (cont_info, Smbios);
    UefiCpuInfo (cont_info, Smbios);
    UefiDimmInfo (cont_info, Smbios);
  }
}
