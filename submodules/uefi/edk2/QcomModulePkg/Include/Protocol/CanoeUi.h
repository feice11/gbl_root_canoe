/** Shared graphical text service exported by the CANOE BDS. */
#ifndef __CANOE_UI_PROTOCOL_H__
#define __CANOE_UI_PROTOCOL_H__

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

#define CANOE_UI_PROTOCOL_REVISION  0x00010000ULL

typedef struct _CANOE_UI_PROTOCOL CANOE_UI_PROTOCOL;

typedef UINTN (EFIAPI *CANOE_UI_MEASURE_TEXT)(
  IN CANOE_UI_PROTOCOL *This,
  IN UINT16             Size,
  IN CONST CHAR16      *Text
  );

typedef EFI_STATUS (EFIAPI *CANOE_UI_DRAW_TEXT)(
  IN CANOE_UI_PROTOCOL                 *This,
  IN UINTN                              X,
  IN UINTN                              Y,
  IN UINT16                             Size,
  IN CONST CHAR16                      *Text,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *Color
  );

struct _CANOE_UI_PROTOCOL {
  UINT64                 Revision;
  CANOE_UI_MEASURE_TEXT  MeasureText;
  CANOE_UI_DRAW_TEXT     DrawText;
};

extern EFI_GUID gCanoeUiProtocolGuid;

#endif
