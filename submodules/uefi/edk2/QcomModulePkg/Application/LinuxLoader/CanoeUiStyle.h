/** @file
 * Shared visual tokens for Canoe's handset UEFI interfaces.
 *
 * This header deliberately contains no boot or tool logic.  LinuxLoader and
 * AndroidToolsUi consume the same measurements and semantic icon vocabulary
 * while retaining their own rendering and navigation code.
 */
#ifndef __CANOE_UI_STYLE_H__
#define __CANOE_UI_STYLE_H__

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>

#define CANOE_UI_SIDE_MARGIN       72
#define CANOE_UI_STATUS_FONT       32
#define CANOE_UI_TITLE_FONT        52
#define CANOE_UI_SUBTITLE_FONT     30
#define CANOE_UI_BODY_FONT         42
#define CANOE_UI_BODY_FONT_MIN     26
#define CANOE_UI_FOOTER_FONT       30
#define CANOE_UI_HEADER_HEIGHT     156
#define CANOE_UI_FOOTER_HEIGHT     112
#define CANOE_UI_SAFE_TOP_MIN      192
#define CANOE_UI_CARD_GAP          14
#define CANOE_UI_CARD_MIN_HEIGHT   82
#define CANOE_UI_CARD_MAX_HEIGHT   112
#define CANOE_UI_ICON_BOX          54
#define CANOE_UI_TEXT_GAP          28
#define CANOE_UI_VISIBLE_MIN       5
#define CANOE_UI_VISIBLE_MAX       7

typedef enum {
  CanoeIconNone = 0,
  CanoeIconBoot,
  CanoeIconUsb,
  CanoeIconFile,
  CanoeIconSettings,
  CanoeIconPower,
  CanoeIconRestart,
  CanoeIconBack,
  CanoeIconLock,
  CanoeIconPalette,
  CanoeIconLanguage,
  CanoeIconPin,
  CanoeIconTool,
  CanoeIconGame,
  CanoeIconInfo,
  CanoeIconWarning
} CANOE_UI_ICON;

typedef struct {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Background;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Surface;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Primary;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Text;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Muted;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Success;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Warning;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Disabled;
} CANOE_UI_PALETTE;

/* Blue, violet, green and amber themes.  BLT stores channels as B,G,R. */
#define CANOE_UI_PALETTE_INITIALIZERS {                                      \
  { {0x18,0x12,0x0d,0}, {0x2d,0x25,0x1d,0}, {0xff,0x8a,0x3d,0},            \
    {0xf4,0xf4,0xf4,0}, {0xb8,0xb0,0xa8,0}, {0x78,0xd6,0x55,0},            \
    {0x42,0xa5,0xff,0}, {0x68,0x62,0x5d,0} },                               \
  { {0x19,0x10,0x14,0}, {0x32,0x21,0x2a,0}, {0xff,0x68,0xa8,0},            \
    {0xfa,0xf4,0xf8,0}, {0xc5,0xad,0xb9,0}, {0x78,0xd6,0x55,0},            \
    {0x42,0xa5,0xff,0}, {0x70,0x5b,0x65,0} },                               \
  { {0x13,0x16,0x0d,0}, {0x26,0x2c,0x1d,0}, {0x86,0xd7,0x36,0},            \
    {0xf4,0xf7,0xef,0}, {0xaf,0xba,0xa2,0}, {0x78,0xd6,0x55,0},            \
    {0x42,0xa5,0xff,0}, {0x5b,0x65,0x50,0} },                               \
  { {0x10,0x12,0x18,0}, {0x20,0x26,0x32,0}, {0x3d,0x8a,0xff,0},            \
    {0xf4,0xf6,0xfa,0}, {0xa9,0xb2,0xc0,0}, {0x78,0xd6,0x55,0},            \
    {0x42,0xa5,0xff,0}, {0x5c,0x63,0x70,0} }                                \
}

#endif /* __CANOE_UI_STYLE_H__ */
