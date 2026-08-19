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
#define CANOE_UI_STATUS_FONT       40
#define CANOE_UI_STATUS_INSET      112
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
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Text;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Muted;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Disabled;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Shadow;
} CANOE_UI_BASE;

typedef struct {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Primary;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Success;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Warning;
} CANOE_UI_ACCENT;

#define CANOE_UI_BASE_COUNT    4
#define CANOE_UI_ACCENT_COUNT  8

/* Graphite, porcelain, smoke and OLED neutral bases. BLT is B,G,R. */
#define CANOE_UI_BASE_INITIALIZERS {                                         \
  { {0x1b,0x19,0x18,0}, {0x2e,0x2b,0x29,0}, {0xf5,0xf4,0xf4,0},            \
    {0xaa,0xa8,0xa6,0}, {0x65,0x60,0x5e,0}, {0x48,0x44,0x42,0} },           \
  { {0xf4,0xf5,0xf5,0}, {0xff,0xff,0xff,0}, {0x24,0x21,0x20,0},            \
    {0x70,0x6a,0x68,0}, {0xc0,0xbb,0xb9,0}, {0xcc,0xc7,0xc5,0} },           \
  { {0x40,0x3d,0x3c,0}, {0x51,0x4d,0x4b,0}, {0xf7,0xf7,0xf7,0},            \
    {0xc7,0xc3,0xc1,0}, {0x7c,0x76,0x73,0}, {0x24,0x21,0x20,0} },           \
  { {0x00,0x00,0x00,0}, {0x14,0x14,0x14,0}, {0xff,0xff,0xff,0},            \
    {0xa0,0xa0,0xa0,0}, {0x55,0x55,0x55,0}, {0x34,0x34,0x34,0} }            \
}

/* Monochrome, cyan, coral, mint, amber, rose, lime and violet accents. */
#define CANOE_UI_ACCENT_INITIALIZERS {                                       \
  { {0xff,0xff,0xff,0}, {0xd4,0xd4,0xd4,0}, {0xb8,0xb8,0xb8,0} },           \
  { {0xc7,0xa6,0x00,0}, {0x78,0xac,0x2f,0}, {0x19,0x86,0xd1,0} },           \
  { {0x5b,0x6a,0xef,0}, {0x7c,0xac,0x2f,0}, {0x19,0x8a,0xd5,0} },           \
  { {0x7c,0xac,0x2f,0}, {0x71,0xb4,0x24,0}, {0x38,0x82,0xdc,0} },           \
  { {0x00,0x8a,0xd5,0}, {0x73,0xa9,0x32,0}, {0x4b,0x5f,0xd9,0} },           \
  { {0x83,0x4e,0xd8,0}, {0x79,0xaa,0x35,0}, {0x16,0x88,0xd7,0} },           \
  { {0x24,0xa5,0x6d,0}, {0x62,0xb0,0x29,0}, {0x18,0x83,0xd4,0} },           \
  { {0xd5,0x5a,0x80,0}, {0x78,0xac,0x30,0}, {0x16,0x86,0xd3,0} }            \
}

#endif /* __CANOE_UI_STYLE_H__ */
