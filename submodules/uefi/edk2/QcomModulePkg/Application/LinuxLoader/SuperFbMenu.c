/*
 * Console UI for the super-fastboot boot menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"
#include "SuperFbFont.h"
#include "CanoeUiStyle.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EFIChargerEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbMenuModuleTag = "SuperFbMenu";

/*
 * Keep the UI on Simple Text Output instead of depending on a particular GOP
 * pixel format or a bundled font. Qualcomm targets consistently expose this
 * protocol, and filling complete text rows still gives us a predictable,
 * polished UI on both portrait handsets and development boards.
 */
#define SFB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define SFB_ATTR_MUTED     EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK)
#define SFB_ATTR_ACCENT    EFI_TEXT_ATTR (EFI_CYAN, EFI_BLACK)
#define SFB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE)
#define SFB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE)
#define SFB_ATTR_SUCCESS   EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLACK)
#define SFB_ATTR_ERROR     EFI_TEXT_ATTR (EFI_LIGHTRED, EFI_BLACK)

#define SFB_UI_LINE_CHARS  160

/* Leave the final physical column unused: several firmware consoles wrap as
 * soon as it is written, which would otherwise insert a blank line per row. */
STATIC UINTN  mSfbColumns = 79;
STATIC BOOLEAN                       mSfbGraphical = FALSE;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mSfbGop = NULL;
STATIC UINTN                         mSfbGfxY = 0;
STATIC UINTN                         mSfbSafeTop = 0;
STATIC UINTN                         mSfbRowHeight = 104;
STATIC UINTN                         mSfbRowStep = 120;
STATIC UINTN                         mSfbVisibleRows = CANOE_UI_VISIBLE_MIN;
STATIC BOOLEAN                       mSfbClockCalibrationLoaded = FALSE;
STATIC UINTN                         mSfbClockOffsetSeconds = 0;

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorBackground = { 0x18, 0x12, 0x0d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorSurface    = { 0x2d, 0x25, 0x1d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorPrimary    = { 0xe8, 0x79, 0x24, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorText       = { 0xf4, 0xf4, 0xf4, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorMuted      = { 0xb0, 0xa8, 0x9f, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorDisabled   = { 0x68, 0x62, 0x5d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorSuccess    = { 0x78, 0xd6, 0x55, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorWarning    = { 0x42, 0xa5, 0xff, 0x00 };

#define SFB_THEME_COUNT  4
#define SFB_LOCK_OFF     0
#define SFB_LOCK_SIMPLE  1
#define SFB_LOCK_PIN     2

STATIC CONST CANOE_UI_PALETTE  mSfbPalettes[SFB_THEME_COUNT] =
  CANOE_UI_PALETTE_INITIALIZERS;

STATIC UINTN    mSfbTheme = 0;
STATIC UINTN    mSfbLockMode = SFB_LOCK_OFF;
STATIC UINTN    mSfbLanguage = 0; /* 0 = Chinese, 1 = English */
STATIC CHAR8    mSfbPin[5] = "1234";
STATIC BOOLEAN  mSfbSettingsLoaded = FALSE;

STATIC
VOID
SfbApplyPalette (VOID)
{
  mSfbColorBackground = mSfbPalettes[mSfbTheme].Background;
  mSfbColorSurface = mSfbPalettes[mSfbTheme].Surface;
  mSfbColorPrimary = mSfbPalettes[mSfbTheme].Primary;
  mSfbColorText = mSfbPalettes[mSfbTheme].Text;
  mSfbColorMuted = mSfbPalettes[mSfbTheme].Muted;
  mSfbColorDisabled = mSfbPalettes[mSfbTheme].Disabled;
  mSfbColorSuccess = mSfbPalettes[mSfbTheme].Success;
  mSfbColorWarning = mSfbPalettes[mSfbTheme].Warning;
}

STATIC
VOID
SfbLoadSettings (VOID)
{
  CHAR8  Record[SFB_STORE_SLOT_BYTES];
  UINTN  Index;

  if (mSfbSettingsLoaded) {
    return;
  }
  mSfbSettingsLoaded = TRUE;
  ZeroMem (Record, sizeof (Record));

  if (!EFI_ERROR (SfbStoreRead (SFB_STORE_SETTINGS, Record,
                                sizeof (Record))) &&
      AsciiStrnCmp (Record, "SFC2|", 5) == 0 &&
      Record[5] >= '0' && Record[5] < '0' + SFB_THEME_COUNT &&
      Record[6] == '|' &&
      Record[7] >= '0' && Record[7] <= '2' &&
      Record[8] == '|' &&
      (Record[9] == '0' || Record[9] == '1') &&
      Record[10] == '|' &&
      (Record[11] == '0' || Record[11] == '1') &&
      Record[12] == '|') {
    mSfbTheme = Record[5] - '0';
    mSfbLockMode = Record[7] - '0';
    mSfbLanguage = Record[9] - '0';
    /* Record[11] belonged to a short-lived physical-key guard. Keep accepting
     * the field so an already-written SFC2 record remains compatible. */
    for (Index = 0; Index < 4; Index++) {
      if (Record[13 + Index] < '0' || Record[13 + Index] > '9') {
        mSfbLockMode = SFB_LOCK_OFF;
        break;
      }
      mSfbPin[Index] = Record[13 + Index];
    }
    mSfbPin[4] = '\0';
  } else if (AsciiStrnCmp (Record, "SFC1|", 5) == 0 &&
             Record[5] >= '0' && Record[5] < '0' + SFB_THEME_COUNT &&
             Record[6] == '|' &&
             Record[7] >= '0' && Record[7] <= '2' &&
             Record[8] == '|') {
    /* Backward-compatible import of the first settings format. */
    mSfbTheme = Record[5] - '0';
    mSfbLockMode = Record[7] - '0';
    for (Index = 0; Index < 4; Index++) {
      if (Record[9 + Index] < '0' || Record[9 + Index] > '9') {
        mSfbLockMode = SFB_LOCK_OFF;
        break;
      }
      mSfbPin[Index] = Record[9 + Index];
    }
    mSfbPin[4] = '\0';
  }
  SfbApplyPalette ();
}

STATIC
EFI_STATUS
SfbSaveSettings (VOID)
{
  CHAR8  Record[32];

  AsciiSPrint (Record, sizeof (Record), "SFC2|%u|%u|%u|%u|%a",
               (UINT32)mSfbTheme, (UINT32)mSfbLockMode,
               (UINT32)mSfbLanguage, 0U, mSfbPin);
  return SfbStoreWrite (SFB_STORE_SETTINGS, Record);
}

UINTN
SfbUiLanguage (VOID)
{
  SfbLoadSettings ();
  return mSfbLanguage;
}

UINTN
SfbUiTheme (VOID)
{
  SfbLoadSettings ();
  return mSfbTheme;
}

CONST CHAR16 *
SfbLocalize (IN CONST CHAR16 *Text)
{
  if (Text == NULL)                         return L"";
  if (mSfbLanguage != 0)                    return Text;
  if (StrCmp (Text, L"Boot Menu") == 0)     return L"启动菜单";
  if (StrCmp (Text, L"EFI Program Selector") == 0) return L"EFI 程序选择器";
  if (StrCmp (Text, L"Action failed") == 0) return L"操作失败";
  if (StrCmp (Text, L"Action complete") == 0) return L"操作完成";
  if (StrCmp (Text, L"Launching") == 0)     return L"正在启动";
  if (StrCmp (Text, L"Power") == 0)         return L"电源选项";
  if (StrCmp (Text, L"Boot control") == 0)  return L"启动控制";
  if (StrCmp (Text, L"Fastboot") == 0)      return L"Fastboot 模式";
  if (StrCmp (Text, L"Settings") == 0)      return L"设置";
  if (StrCmp (Text, L"Enter PIN") == 0)     return L"输入 PIN";
  if (StrCmp (Text, L"Set PIN") == 0)       return L"设置 PIN";
  if (StrCmp (Text, L"Simple lock") == 0)   return L"简易锁";
  if (StrCmp (Text, L"Wrong password") == 0)return L"密码错误";
  if (StrCmp (Text, L"Try again") == 0)     return L"请重试";
  if (StrCmp (Text, L"Back") == 0)          return L"返回";
  if (StrCmp (Text, L"Select") == 0)        return L"确认";
  if (StrCmp (Text, L"Continue") == 0)      return L"继续";
  if (StrCmp (Text, L"Next") == 0)          return L"下一位";
  if (StrCmp (Text, L"Unlock") == 0)        return L"解锁";
  if (StrCmp (Text, L"Retry") == 0)         return L"重试";
  if (StrCmp (Text, L"Open") == 0)          return L"打开";
  if (StrCmp (Text, L"Powering off...") == 0) return L"正在关机...";
  if (StrCmp (Text, L"Restarting...") == 0) return L"正在重启...";
  if (StrCmp (Text, L"Please wait") == 0)   return L"请稍候";
  if (StrCmp (Text, L"USB service is ready") == 0) return L"USB 服务已就绪";
  if (StrCmp (Text, L"Starting the selected EFI application") == 0) return L"正在启动所选 EFI 程序";
  if (StrCmp (Text, L"Preparing devices and boot entries") == 0) return L"正在准备设备和启动项";
  if (StrCmp (Text, L"EFI Driver") == 0) return L"EFI 驱动程序";
  if (StrCmp (Text, L"EFI Application") == 0) return L"EFI 应用程序";
  if (StrCmp (Text, L"Load") == 0) return L"加载";
  if (StrCmp (Text, L"Boot (temporary)") == 0) return L"临时启动";
  if (StrCmp (Text, L"Add to BootMenu") == 0) return L"添加到启动菜单";
  if (StrCmp (Text, L"Driver load failed") == 0) return L"驱动加载失败";
  if (StrCmp (Text, L"Driver loaded") == 0) return L"驱动已加载";
  if (StrCmp (Text, L"Cannot address that file") == 0) return L"无法访问该文件";
  if (StrCmp (Text, L"Boot failed") == 0) return L"启动失败";
  if (StrCmp (Text, L"Could not save entry") == 0) return L"无法保存启动项";
  if (StrCmp (Text, L"Added to boot menu") == 0) return L"已添加到启动菜单";
  if (StrCmp (Text, L"Out of memory") == 0) return L"内存不足";
  if (StrCmp (Text, L"Cannot read directory") == 0) return L"无法读取目录";
  if (StrCmp (Text, L"Not an EFI application") == 0) return L"不是 EFI 应用程序";
  if (StrCmp (Text, L"No FAT32 volumes found") == 0) return L"未找到 FAT32 卷";
  if (StrCmp (Text, L"Choose a FAT32 volume to browse.") == 0) return L"选择要浏览的 FAT32 卷";
  return Text;
}

/* Entry descriptions loaded from BOOTENTRIES/ENTRIES are user data. Never
 * translate them by matching their text: a custom entry named "Android" or
 * "Android Tools" must remain exactly as configured. Only built-in rows are
 * localized, selected by their semantic kind. */
STATIC
CONST CHAR16 *
SfbUiEntryText (IN SFB_ENTRY_KIND Kind, IN CONST CHAR16 *Text)
{
  switch (Kind) {
  case SfbEntryFastboot: return mSfbLanguage == 0 ? L"进入 Fastboot" : Text;
  case SfbEntrySelector: return mSfbLanguage == 0 ? L"选择 EFI 程序" : Text;
  case SfbEntrySettings: return mSfbLanguage == 0 ? L"设置" : Text;
  case SfbEntryBack:     return mSfbLanguage == 0 ? L"返回" : Text;
  case SfbEntryPowerOff: return mSfbLanguage == 0 ? L"关机" : Text;
  case SfbEntryRestart:  return mSfbLanguage == 0 ? L"重新启动" : Text;
  default:               return Text != NULL ? Text : L"";
  }
}

STATIC
VOID
SfbGfxFill (IN UINTN X, IN UINTN Y, IN UINTN Width, IN UINTN Height,
            IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  if (!mSfbGraphical || Width == 0 || Height == 0) {
    return;
  }
  mSfbGop->Blt (mSfbGop, Color, EfiBltVideoFill, 0, 0, X, Y,
                Width, Height, 0);
}

STATIC
UINTN
SfbGfxMeasureText (IN UINT16 Size, IN CONST CHAR16 *Text)
{
  CONST SFB_FONT_GLYPH  *Glyph;
  UINTN                 GlyphIndex;
  UINTN                 Index;
  UINTN                 Width = 0;

  if (Text == NULL || Size == 0) return 0;
  for (Index = 0; Text[Index] != L'\0'; Index++) {
    Glyph = NULL;
    for (GlyphIndex = 0; GlyphIndex < ARRAY_SIZE (mSfbFontGlyphs); GlyphIndex++) {
      if (mSfbFontGlyphs[GlyphIndex].Codepoint == Text[Index]) {
        Glyph = &mSfbFontGlyphs[GlyphIndex];
        break;
      }
    }
    if (Glyph != NULL) {
      Width += ((UINTN)Glyph->Advance * Size + SFB_FONT_HEIGHT - 1) /
               SFB_FONT_HEIGHT;
    }
  }
  return Width;
}

STATIC
UINT16
SfbGfxFitText (IN UINT16 Preferred, IN UINT16 Minimum,
               IN UINTN Available, IN CONST CHAR16 *Text)
{
  UINT16 Size = Preferred;
  while (Size > Minimum && SfbGfxMeasureText (Size, Text) > Available) {
    Size = (UINT16)(Size - 2);
  }
  return Size;
}

STATIC
EFI_STATUS
SfbGfxText (IN UINTN X, IN UINTN Y, IN UINT16 Size,
            IN CONST CHAR16 *Text,
            IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color);

STATIC
VOID
SfbGfxCenteredText (IN UINTN Y, IN UINT16 Preferred, IN UINT16 Minimum,
                    IN CONST CHAR16 *Text,
                    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  UINTN Width;
  UINTN TextWidth;
  UINT16 Size;

  if (!mSfbGraphical || Text == NULL) return;
  Width = mSfbGop->Mode->Info->HorizontalResolution;
  Size = SfbGfxFitText (Preferred, Minimum,
                        Width - 2 * CANOE_UI_SIDE_MARGIN, Text);
  TextWidth = SfbGfxMeasureText (Size, Text);
  SfbGfxText ((Width > TextWidth) ? (Width - TextWidth) / 2 : 0,
              Y, Size, Text, Color);
}

STATIC
VOID
SfbGfxIcon (IN UINTN X, IN UINTN Y, IN UINTN Size, IN CANOE_UI_ICON Icon,
            IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  UINTN U = MAX (2, Size / 9);
  UINTN C = Size / 2;

  if (Icon == CanoeIconNone) return;
  /* Every icon is made from filled primitives so it works on bare GOP. */
  if (Icon == CanoeIconBoot || Icon == CanoeIconRestart ||
      Icon == CanoeIconBack) {
    SfbGfxFill (X + U, Y + C - U, Size - 2 * U, 2 * U, Color);
    SfbGfxFill (Icon == CanoeIconBack ? X + U : X + Size - 3 * U,
                Y + C - 3 * U, 2 * U, 6 * U, Color);
  } else if (Icon == CanoeIconFile) {
    SfbGfxFill (X + 2 * U, Y + U, Size - 4 * U, U, Color);
    SfbGfxFill (X + 2 * U, Y + U, U, Size - 2 * U, Color);
    SfbGfxFill (X + 2 * U, Y + Size - 2 * U, Size - 4 * U, U, Color);
    SfbGfxFill (X + Size - 3 * U, Y + 3 * U, U, Size - 4 * U, Color);
  } else if (Icon == CanoeIconUsb) {
    SfbGfxFill (X + C - U / 2, Y + U, U, Size - 3 * U, Color);
    SfbGfxFill (X + C, Y + 3 * U, 3 * U, U, Color);
    SfbGfxFill (X + C - 3 * U, Y + 5 * U, 3 * U, U, Color);
    SfbGfxFill (X + C - U, Y + Size - 2 * U, 3 * U, U, Color);
  } else if (Icon == CanoeIconLock || Icon == CanoeIconPin) {
    SfbGfxFill (X + 2 * U, Y + 4 * U, Size - 4 * U, Size - 5 * U, Color);
    SfbGfxFill (X + 3 * U, Y + U, U, 4 * U, Color);
    SfbGfxFill (X + Size - 4 * U, Y + U, U, 4 * U, Color);
    SfbGfxFill (X + 3 * U, Y + U, Size - 6 * U, U, Color);
  } else if (Icon == CanoeIconGame) {
    SfbGfxFill (X + U, Y + 3 * U, Size - 2 * U, 4 * U, Color);
    SfbGfxFill (X + 3 * U, Y + 2 * U, U, 6 * U, Color);
    SfbGfxFill (X + 2 * U, Y + 4 * U, 3 * U, U, Color);
    SfbGfxFill (X + Size - 4 * U, Y + 4 * U, U, U, &mSfbColorBackground);
  } else if (Icon == CanoeIconWarning) {
    SfbGfxFill (X + C - U / 2, Y + U, U, 5 * U, Color);
    SfbGfxFill (X + C - U / 2, Y + 7 * U, U, U, Color);
  } else if (Icon == CanoeIconPalette) {
    SfbGfxFill (X + U, Y + 2 * U, Size - 2 * U, 5 * U, Color);
    SfbGfxFill (X + 3 * U, Y + 3 * U, U, U, &mSfbColorBackground);
    SfbGfxFill (X + 5 * U, Y + 3 * U, U, U, &mSfbColorBackground);
  } else {
    /* Settings/tool/language/info use a stable cross-in-box glyph. */
    SfbGfxFill (X + U, Y + U, Size - 2 * U, U, Color);
    SfbGfxFill (X + U, Y + Size - 2 * U, Size - 2 * U, U, Color);
    SfbGfxFill (X + U, Y + U, U, Size - 2 * U, Color);
    SfbGfxFill (X + Size - 2 * U, Y + U, U, Size - 2 * U, Color);
    SfbGfxFill (X + C - U / 2, Y + 3 * U, U, 3 * U, Color);
  }
}

STATIC
CANOE_UI_ICON
SfbIconFromMarker (IN CONST CHAR16 *Marker)
{
  if (Marker == NULL || Marker[0] == L'\0' || StrCmp (Marker, L" ") == 0) return CanoeIconNone;
  if (StrCmp (Marker, L"USB") == 0) return CanoeIconUsb;
  if (StrCmp (Marker, L"FILES") == 0 || StrCmp (Marker, L"[V]") == 0) return CanoeIconFile;
  if (StrCmp (Marker, L"POWER") == 0) return CanoeIconPower;
  if (StrCmp (Marker, L"COLOR") == 0) return CanoeIconPalette;
  if (StrCmp (Marker, L"LANG") == 0) return CanoeIconLanguage;
  if (StrCmp (Marker, L"LOCK") == 0) return CanoeIconLock;
  if (StrCmp (Marker, L"PIN") == 0) return CanoeIconPin;
  if (StrCmp (Marker, L"BACK") == 0) return CanoeIconBack;
  if (StrCmp (Marker, L"MENU") == 0) return CanoeIconTool;
  return CanoeIconBoot;
}

STATIC
EFI_STATUS
SfbGfxText (IN UINTN X, IN UINTN Y, IN UINT16 Size,
            IN CONST CHAR16 *Text,
            IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer;
  EFI_STATUS                     Status;
  CONST SFB_FONT_GLYPH           *Glyph;
  UINTN                          TextWidth = 0;
  UINTN                          Width;
  UINTN                          Height;
  UINTN                          Cursor;
  UINTN                          Index;
  UINTN                          Dx;
  UINTN                          Dy;
  UINTN                          Sx;
  UINTN                          Sy;
  UINTN                          Advance;
  UINTN                          Alpha;
  UINTN                          GlyphIndex;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel;

  if (!mSfbGraphical || Text == NULL || Size == 0) {
    return EFI_UNSUPPORTED;
  }

  /* Measure with the same scaled advance used by the raster loop. */
  for (Index = 0; Text[Index] != L'\0'; Index++) {
    Glyph = NULL;
    for (GlyphIndex = 0; GlyphIndex < ARRAY_SIZE (mSfbFontGlyphs); GlyphIndex++) {
      if (mSfbFontGlyphs[GlyphIndex].Codepoint == Text[Index]) {
        Glyph = &mSfbFontGlyphs[GlyphIndex];
        break;
      }
    }
    if (Glyph == NULL) {
      for (GlyphIndex = 0; GlyphIndex < ARRAY_SIZE (mSfbFontGlyphs); GlyphIndex++) {
        if (mSfbFontGlyphs[GlyphIndex].Codepoint == L'?') {
          Glyph = &mSfbFontGlyphs[GlyphIndex];
          break;
        }
      }
    }
    if (Glyph != NULL) {
      TextWidth += ((UINTN)Glyph->Advance * Size + SFB_FONT_HEIGHT - 1) /
                   SFB_FONT_HEIGHT;
    }
  }

  if (X >= mSfbGop->Mode->Info->HorizontalResolution ||
      Y >= mSfbGop->Mode->Info->VerticalResolution || TextWidth == 0) {
    return EFI_SUCCESS;
  }
  Width = MIN (TextWidth,
               mSfbGop->Mode->Info->HorizontalResolution - X);
  Height = MIN ((UINTN)Size,
                mSfbGop->Mode->Info->VerticalResolution - Y);
  Buffer = AllocatePool (Width * Height * sizeof (*Buffer));
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* Preserve the card/background pixels so the 1-bpp glyph has transparent
   * off-pixels without depending on HII font or alpha-blending support. */
  Status = mSfbGop->Blt (mSfbGop, Buffer, EfiBltVideoToBltBuffer,
                         X, Y, 0, 0, Width, Height,
                         Width * sizeof (*Buffer));
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    return Status;
  }

  Cursor = 0;
  for (Index = 0; Text[Index] != L'\0' && Cursor < Width; Index++) {
    Glyph = NULL;
    for (GlyphIndex = 0; GlyphIndex < ARRAY_SIZE (mSfbFontGlyphs); GlyphIndex++) {
      if (mSfbFontGlyphs[GlyphIndex].Codepoint == Text[Index]) {
        Glyph = &mSfbFontGlyphs[GlyphIndex];
        break;
      }
    }
    if (Glyph == NULL) {
      continue;
    }
    Advance = ((UINTN)Glyph->Advance * Size + SFB_FONT_HEIGHT - 1) /
              SFB_FONT_HEIGHT;
    for (Dy = 0; Dy < Height; Dy++) {
      Sy = Dy * SFB_FONT_HEIGHT / Size;
      for (Dx = 0; Dx < Advance && Cursor + Dx < Width; Dx++) {
        Sx = Dx * Glyph->Advance / Advance;
        Alpha = Glyph->Bitmap[Sy * SFB_FONT_STRIDE + Sx / 2];
        Alpha = ((Sx & 1) == 0) ? (Alpha >> 4) : (Alpha & 0x0F);
        if (Alpha != 0) {
          Pixel = &Buffer[Dy * Width + Cursor + Dx];
          Pixel->Blue = (UINT8)((Color->Blue * Alpha +
                                 Pixel->Blue * (15 - Alpha) + 7) / 15);
          Pixel->Green = (UINT8)((Color->Green * Alpha +
                                  Pixel->Green * (15 - Alpha) + 7) / 15);
          Pixel->Red = (UINT8)((Color->Red * Alpha +
                                Pixel->Red * (15 - Alpha) + 7) / 15);
        }
      }
    }
    Cursor += Advance;
  }

  Status = mSfbGop->Blt (mSfbGop, Buffer, EfiBltBufferToVideo,
                         0, 0, X, Y, Width, Height,
                         Width * sizeof (*Buffer));
  FreePool (Buffer);
  return Status;
}

/* ChargerEx exposes voltage rather than fuel-gauge SOC on this open-source
 * interface. Use a conservative single-cell Li-ion curve for the status-bar
 * percentage; the exact millivolt reading remains the source of truth. */
STATIC
UINTN
SfbBatteryPercentFromVoltage (IN UINT32 Millivolts)
{
  STATIC CONST struct {
    UINT32  Millivolts;
    UINTN   Percent;
  } Curve[] = {
    { 3400,   0 }, { 3600,   5 }, { 3700,  12 }, { 3800,  25 },
    { 3900,  40 }, { 4000,  55 }, { 4100,  68 }, { 4200,  78 },
    { 4300,  86 }, { 4400,  93 }, { 4550, 100 }
  };
  UINTN  Index;

  if (Millivolts <= Curve[0].Millivolts) {
    return Curve[0].Percent;
  }
  for (Index = 1; Index < ARRAY_SIZE (Curve); Index++) {
    if (Millivolts <= Curve[Index].Millivolts) {
      UINT32  Span = Curve[Index].Millivolts - Curve[Index - 1].Millivolts;
      return Curve[Index - 1].Percent +
             (UINTN)((Millivolts - Curve[Index - 1].Millivolts) *
                     (Curve[Index].Percent - Curve[Index - 1].Percent) / Span);
    }
  }
  return 100;
}

STATIC
VOID
SfbReadPowerStatus (OUT BOOLEAN *Available,
                    OUT UINTN   *Percent,
                    OUT BOOLEAN *Charging)
{
  EFI_CHARGER_EX_PROTOCOL  *Charger = NULL;
  EFI_STATUS               Status;
  UINT32                   Millivolts = 0;
  BOOLEAN                  Present = FALSE;

  *Available = FALSE;
  *Percent = 0;
  *Charging = FALSE;

  Status = gBS->LocateProtocol (&gChargerExProtocolGuid, NULL,
                                (VOID **)&Charger);
  if (EFI_ERROR (Status) || Charger == NULL) {
    return;
  }
  if (Charger->GetChargerPresence != NULL) {
    (VOID)Charger->GetChargerPresence (&Present);
    *Charging = Present;
  }
  if (Charger->GetBatteryVoltage == NULL ||
      EFI_ERROR (Charger->GetBatteryVoltage (&Millivolts)) ||
      Millivolts < 2500 || Millivolts > 5000) {
    return;
  }

  *Available = TRUE;
  *Percent = SfbBatteryPercentFromVoltage (Millivolts);
}

/* Android keeps the user-visible clock independently from the Qualcomm RTC on
 * this device.  The latter can remain in 1970 while still ticking normally.
 * A Magisk service therefore records the local-time offset in persist; using
 * the offset instead of setting the RTC avoids disturbing firmware/Android. */
STATIC
BOOLEAN
SfbLoadClockCalibration (VOID)
{
  EFI_HANDLE         *Volumes = NULL;
  UINTN              VolumeCount = 0;
  UINTN              VolumeIndex;
  EFI_FILE_PROTOCOL  *Root = NULL;
  CHAR8              Buffer[32];
  UINTN              BytesRead;
  UINTN              Index;
  UINTN              Value;
  BOOLEAN            Found = FALSE;

  if (mSfbClockCalibrationLoaded) {
    return TRUE;
  }
  if (EFI_ERROR (SfbLocateVolumes (&Volumes, &VolumeCount)) || Volumes == NULL) {
    return FALSE;
  }

  for (VolumeIndex = 0; VolumeIndex < VolumeCount && !Found; VolumeIndex++) {
    if (!SfbIsExt4Volume (Volumes[VolumeIndex]) ||
        EFI_ERROR (SfbOpenVolumeRoot (Volumes[VolumeIndex], &Root)) ||
        Root == NULL) {
      continue;
    }
    ZeroMem (Buffer, sizeof (Buffer));
    BytesRead = 0;
    if (!EFI_ERROR (SfbReadFileBytes (Root, L"\\efisp\\CLOCKOFFSET",
                                      Buffer, sizeof (Buffer) - 1,
                                      &BytesRead)) &&
        BytesRead > 10 &&
        CompareMem (Buffer, "SFCLOCK1 ", 9) == 0) {
      Value = 0;
      Found = TRUE;
      for (Index = 9; Index < BytesRead && Buffer[Index] != '\n' &&
                      Buffer[Index] != '\r' && Buffer[Index] != '\0'; Index++) {
        if (Buffer[Index] < '0' || Buffer[Index] > '9') {
          Found = FALSE;
          break;
        }
        Value = Value * 10 + (UINTN)(Buffer[Index] - '0');
        if (Value >= 86400) {
          Found = FALSE;
          break;
        }
      }
      if (Index == 9) {
        Found = FALSE;
      }
      if (Found) {
        mSfbClockOffsetSeconds = Value;
        mSfbClockCalibrationLoaded = TRUE;
      }
    }
    Root->Close (Root);
    Root = NULL;
  }
  FreePool (Volumes);
  return Found;
}

BOOLEAN
SfbUiClockOffsetSeconds (OUT UINTN *OffsetSeconds)
{
  if (OffsetSeconds == NULL || !SfbLoadClockCalibration ()) {
    return FALSE;
  }
  *OffsetSeconds = mSfbClockOffsetSeconds;
  return TRUE;
}

STATIC
VOID
SfbDrawStatusBar (VOID)
{
  EFI_TIME  Time;
  CHAR16    TimeText[16];
  CHAR16    PercentText[16];
  BOOLEAN   BatteryAvailable;
  BOOLEAN   Charging;
  UINTN     BatteryPercent;
  UINTN     Width;
  UINTN     StatusY;
  UINTN     BatteryX;
  UINTN     BatteryY;
  UINTN     FillWidth;
  UINTN     LocalSeconds;

  if (!mSfbGraphical || mSfbSafeTop < 48) {
    return;
  }

  Width = mSfbGop->Mode->Info->HorizontalResolution;
  StatusY = (mSfbSafeTop - 32) / 2;
  if (!EFI_ERROR (gRT->GetTime (&Time, NULL)) &&
      Time.Hour < 24 && Time.Minute < 60 && Time.Second < 60 &&
      SfbLoadClockCalibration ()) {
    LocalSeconds = ((UINTN)Time.Hour * 3600 + (UINTN)Time.Minute * 60 +
                    (UINTN)Time.Second + mSfbClockOffsetSeconds) % 86400;
    UnicodeSPrint (TimeText, sizeof (TimeText), L"%02u:%02u",
                   (UINT32)(LocalSeconds / 3600),
                   (UINT32)((LocalSeconds / 60) % 60));
  } else {
    StrCpyS (TimeText, ARRAY_SIZE (TimeText), L"--:--");
  }
  SfbGfxText (72, StatusY, 32, TimeText, &mSfbColorText);

  SfbReadPowerStatus (&BatteryAvailable, &BatteryPercent, &Charging);
  UnicodeSPrint (PercentText, sizeof (PercentText),
                 BatteryAvailable ? L"%u%%" : L"--%%",
                 (UINT32)BatteryPercent);

  BatteryX = Width - 220;
  BatteryY = StatusY + 3;
  /* Battery outline and terminal. */
  SfbGfxFill (BatteryX, BatteryY, 56, 3, &mSfbColorMuted);
  SfbGfxFill (BatteryX, BatteryY + 25, 56, 3, &mSfbColorMuted);
  SfbGfxFill (BatteryX, BatteryY, 3, 28, &mSfbColorMuted);
  SfbGfxFill (BatteryX + 53, BatteryY, 3, 28, &mSfbColorMuted);
  SfbGfxFill (BatteryX + 56, BatteryY + 8, 5, 12, &mSfbColorMuted);
  if (BatteryAvailable && BatteryPercent != 0) {
    FillWidth = 46 * BatteryPercent / 100;
    FillWidth = MAX (FillWidth, 2);
    SfbGfxFill (BatteryX + 5, BatteryY + 5, FillWidth, 18,
                Charging ? &mSfbColorPrimary : &mSfbColorText);
  }
  if (Charging) {
    /* Compact lightning mark immediately before the battery. */
    SfbGfxFill (BatteryX - 30, BatteryY + 2, 13, 8, &mSfbColorPrimary);
    SfbGfxFill (BatteryX - 24, BatteryY + 8, 13, 8, &mSfbColorPrimary);
    SfbGfxFill (BatteryX - 18, BatteryY + 14, 7, 11, &mSfbColorPrimary);
  }
  SfbGfxText (Width - 140, StatusY, 32, PercentText, &mSfbColorText);
}

/* SimpleTextIn reports repeats but does not reliably expose a key-up event on
 * these handsets. After Power confirms an action, wait until the input stream
 * has stayed quiet for a complete debounce window. This prevents one slightly
 * long press from confirming a second item on the next screen. */
STATIC
VOID
SfbWaitForInputQuiet (IN UINT32 LeadMs, IN UINT32 QuietMs)
{
  EFI_EVENT      TimerEvent;
  EFI_EVENT      WaitList[2];
  EFI_INPUT_KEY  Key;
  EFI_STATUS     Status;
  UINTN          EventIndex;

  gBS->Stall ((UINTN)LeadMs * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
  if (EFI_ERROR (Status)) {
    return;
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;
  while (TRUE) {
    gBS->SetTimer (TimerEvent, TimerRelative, (UINT64)QuietMs * 10000);
    Status = gBS->WaitForEvent (2, WaitList, &EventIndex);
    if (EFI_ERROR (Status) || EventIndex == 1) {
      break;
    }
    while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      /* Drain every repeat, then require another full quiet window. */
    }
  }
  gBS->CloseEvent (TimerEvent);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

STATIC
VOID
SfbWaitForSelectRelease (VOID)
{
  SfbWaitForInputQuiet (200, 220);
}

STATIC
VOID
SfbUiInitGraphics (VOID)
{
  EFI_STATUS  GopStatus;

  if (mSfbGraphical) {
    return;
  }
  GopStatus = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                   (VOID **)&mSfbGop);
  mSfbGraphical = (BOOLEAN)(!EFI_ERROR (GopStatus) &&
                            mSfbGop != NULL && mSfbGop->Mode != NULL &&
                            mSfbGop->Mode->Info != NULL &&
                            mSfbGop->Mode->Info->HorizontalResolution <= MAX_UINT16 &&
                            mSfbGop->Mode->Info->VerticalResolution <= MAX_UINT16);
}

STATIC
VOID
SfbUiRefreshGeometry (VOID)
{
  UINTN       Columns;
  UINTN       Rows;
  EFI_STATUS  Status;

  if (gST->ConOut->Mode == NULL || gST->ConOut->Mode->Mode < 0) {
    return;
  }

  Status = gST->ConOut->QueryMode (
                         gST->ConOut,
                         (UINTN)gST->ConOut->Mode->Mode,
                         &Columns,
                         &Rows
                         );
  if (!EFI_ERROR (Status) && Columns >= 40) {
    mSfbColumns = MIN (Columns - 1, SFB_UI_LINE_CHARS - 1);
  }
  (VOID)Rows;
}

/* Print a complete, padded row. Besides looking like a real selection card,
 * this also erases remnants when a shorter label replaces a longer one. */
STATIC
VOID
SfbUiFullRow (IN UINTN Attribute, IN CONST CHAR16 *Text)
{
  CHAR16  Line[SFB_UI_LINE_CHARS];
  UINTN   Length;
  UINTN   Index;

  if (Text == NULL) {
    Text = L"";
  }
  Text = SfbLocalize (Text);

  if (mSfbGraphical) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color;
    UINTN Width;
    UINT16 Size;

    if (Text[0] == L'\0') {
      mSfbGfxY += 28;
      return;
    }

    Color = (Attribute == SFB_ATTR_ERROR) ? &mSfbColorWarning :
            (Attribute == SFB_ATTR_SUCCESS) ? &mSfbColorSuccess :
            (Attribute == SFB_ATTR_ACCENT) ? &mSfbColorPrimary :
            (Attribute == SFB_ATTR_MUTED) ? &mSfbColorMuted : &mSfbColorText;
    Width = mSfbGop->Mode->Info->HorizontalResolution;
    SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY,
                Width - 2 * CANOE_UI_SIDE_MARGIN, 76, &mSfbColorSurface);
    SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY, 4, 76, Color);
    Size = SfbGfxFitText (CANOE_UI_SUBTITLE_FONT, 22,
                          Width - 2 * CANOE_UI_SIDE_MARGIN - 48, Text);
    SfbGfxText (CANOE_UI_SIDE_MARGIN + 28,
                mSfbGfxY + (76 - Size) / 2, Size, Text, Color);
    mSfbGfxY += 90;
    return;
  }

  StrnCpyS (Line, ARRAY_SIZE (Line), Text, mSfbColumns);
  Length = StrLen (Line);
  for (Index = Length; Index < mSfbColumns; Index++) {
    Line[Index] = L' ';
  }
  Line[mSfbColumns] = L'\0';

  gST->ConOut->SetAttribute (gST->ConOut, Attribute);
  gST->ConOut->OutputString (gST->ConOut, Line);
  gST->ConOut->OutputString (gST->ConOut, L"\r\n");
}

STATIC
VOID
SfbUiRule (VOID)
{
  CHAR16  Rule[SFB_UI_LINE_CHARS];
  UINTN   Index;

  for (Index = 0; Index < mSfbColumns; Index++) {
    Rule[Index] = L'-';
  }
  Rule[mSfbColumns] = L'\0';
  SfbUiFullRow (SFB_ATTR_ACCENT, Rule);
}

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  SFB_KEY        Result = SfbKeyTimeout;

  if (TimeoutMs != 0) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
    if (EFI_ERROR (Status)) {
      TimerEvent = NULL;
    } else {
      /* Boot services timers count in 100ns units. */
      Status = gBS->SetTimer (TimerEvent, TimerRelative,
                              (UINT64)TimeoutMs * 10000);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (TimerEvent);
        TimerEvent = NULL;
      }
    }
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitCount = 1;
  if (TimerEvent != NULL) {
    WaitList[1] = TimerEvent;
    WaitCount = 2;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 1) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }

    /*
     * On the handset the Qualcomm keypad driver reports the volume keys as
     * SCAN_UP and SCAN_DOWN, and power arrives as a carriage return.
     *
     * Anything left over counts as confirm: on a three-key handset there is
     * nothing else it can be, so the menu stays usable even if a platform
     * reports power differently from what is expected here.
     */
    if (Key.ScanCode == SCAN_UP) {
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Result = SfbKeyDown;
    } else {
      DEBUG ((EFI_D_VERBOSE, "SFB: confirm key scan=0x%x char=0x%x\n",
              Key.ScanCode, Key.UnicodeChar));
      Result = SfbKeySelect;
      SfbWaitForSelectRelease ();
    }
    break;
  }

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  return Result;
}

/* ---- drawing ------------------------------------------------------------ */

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  CHAR16  Header[SFB_UI_LINE_CHARS];
  UINT16  TitleSize;
  UINTN   Width;
  UINTN   Height;
  UINTN   ContentTop;
  UINTN   Available;

  SfbLoadSettings ();
  Title = SfbLocalize (Title);
  if (Subtitle != NULL) {
    Subtitle = SfbLocalize (Subtitle);
  }
  SfbUiInitGraphics ();
  if (mSfbGraphical) {
    Width = mSfbGop->Mode->Info->HorizontalResolution;
    Height = mSfbGop->Mode->Info->VerticalResolution;
    mSfbSafeTop = MAX (CANOE_UI_SAFE_TOP_MIN, Height / 16);
    SfbGfxFill (0, 0, Width, Height, &mSfbColorBackground);
    SfbDrawStatusBar ();
    SfbGfxFill (0, mSfbSafeTop, Width, CANOE_UI_HEADER_HEIGHT, &mSfbColorSurface);
    SfbGfxFill (0, mSfbSafeTop + CANOE_UI_HEADER_HEIGHT - 4,
                Width, 4, &mSfbColorPrimary);
    UnicodeSPrint (Header, sizeof (Header), L"%s", Title);
    TitleSize = SfbGfxFitText (CANOE_UI_TITLE_FONT, 34,
                              Width - 2 * CANOE_UI_SIDE_MARGIN, Header);
    SfbGfxText ((Width - SfbGfxMeasureText (TitleSize, Header)) / 2,
                mSfbSafeTop + (CANOE_UI_HEADER_HEIGHT - TitleSize) / 2,
                TitleSize, Header, &mSfbColorText);
    if (Subtitle != NULL) {
      SfbGfxText (CANOE_UI_SIDE_MARGIN,
                  mSfbSafeTop + CANOE_UI_HEADER_HEIGHT + 24,
                  SfbGfxFitText (CANOE_UI_SUBTITLE_FONT, 22,
                                 Width - 2 * CANOE_UI_SIDE_MARGIN, Subtitle),
                  Subtitle, &mSfbColorMuted);
      ContentTop = mSfbSafeTop + CANOE_UI_HEADER_HEIGHT + 82;
    } else {
      ContentTop = mSfbSafeTop + CANOE_UI_HEADER_HEIGHT + 34;
    }
    mSfbGfxY = ContentTop;
    Available = (Height > ContentTop + CANOE_UI_FOOTER_HEIGHT + 20)
                  ? Height - ContentTop - CANOE_UI_FOOTER_HEIGHT - 20 : 0;
    mSfbRowStep = (mSfbVisibleRows != 0) ? Available / mSfbVisibleRows : 0;
    mSfbRowStep = MIN (CANOE_UI_CARD_MAX_HEIGHT + CANOE_UI_CARD_GAP,
                       MAX ((UINTN)44, mSfbRowStep));
    mSfbRowHeight = mSfbRowStep - CANOE_UI_CARD_GAP;
    return;
  }

  SfbUiRefreshGeometry ();
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  UnicodeSPrint (Header, sizeof (Header), L"  %s", Title);
  SfbUiFullRow (SFB_ATTR_TITLE, Header);
  SfbUiRule ();
  if (Subtitle != NULL) {
    UnicodeSPrint (Header, sizeof (Header), L"  %s", Subtitle);
    SfbUiFullRow (SFB_ATTR_MUTED, Header);
  }
  SfbUiFullRow (SFB_ATTR_NORMAL, L"");
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  CHAR16  Hint[SFB_UI_LINE_CHARS];

  if (mSfbGraphical) {
    UINTN  Width = mSfbGop->Mode->Info->HorizontalResolution;
    UINTN  Height = mSfbGop->Mode->Info->VerticalResolution;

    SfbGfxFill (0, Height - CANOE_UI_FOOTER_HEIGHT, Width,
                CANOE_UI_FOOTER_HEIGHT, &mSfbColorSurface);
    SfbGfxFill (0, Height - CANOE_UI_FOOTER_HEIGHT - 4,
                Width, 4, &mSfbColorPrimary);
    SfbGfxText (CANOE_UI_SIDE_MARGIN, Height - 76,
                CANOE_UI_FOOTER_FONT,
                mSfbLanguage == 0
                  ? L"音量 +/-：移动      电源键：确认"
                  : L"VOL +/-: Move      POWER: Select",
                &mSfbColorMuted);
    return;
  }

  SfbUiFullRow (SFB_ATTR_NORMAL, L"");
  SfbUiRule ();
  if (mSfbLanguage == 0) {
    UnicodeSPrint (Hint, sizeof (Hint), L"  [音量 +/-] 移动    [电源键] %s",
                   Footer != NULL ? SfbLocalize (Footer) : L"确认");
  } else {
    UnicodeSPrint (Hint, sizeof (Hint), L"  [VOL +/-] Navigate    [POWER] %s",
                   Footer != NULL ? Footer : L"Select");
  }
  SfbUiFullRow (SFB_ATTR_MUTED, Hint);
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  SfbDrawRowIcon (Selected, SfbIconFromMarker (Marker), Marker, Text);
}

VOID
SfbSetVisibleRows (IN UINTN Rows)
{
  mSfbVisibleRows = MIN (CANOE_UI_VISIBLE_MAX,
                         MAX (CANOE_UI_VISIBLE_MIN, Rows));
}

VOID
SfbDrawRowIcon (IN BOOLEAN Selected, IN CANOE_UI_ICON Icon,
                IN CONST CHAR16 *FallbackMarker, IN CONST CHAR16 *Text)
{
  CHAR16  Row[SFB_UI_LINE_CHARS];

  if (mSfbGraphical) {
    UINTN  Width = mSfbGop->Mode->Info->HorizontalResolution;
    UINTN  CardWidth = Width - 2 * CANOE_UI_SIDE_MARGIN;
    UINTN  IconSize = MIN (CANOE_UI_ICON_BOX,
                           (mSfbRowHeight > 18) ? mSfbRowHeight - 18 : 12);
    UINTN  IconY = mSfbGfxY + (mSfbRowHeight - IconSize) / 2;
    UINTN  TextX = CANOE_UI_SIDE_MARGIN + 34 + CANOE_UI_ICON_BOX +
                   CANOE_UI_TEXT_GAP;
    UINTN  TextWidth = Width - CANOE_UI_SIDE_MARGIN - TextX - 28;
    UINT16 TextSize = SfbGfxFitText (CANOE_UI_BODY_FONT,
                                    CANOE_UI_BODY_FONT_MIN,
                                    TextWidth, Text);
    TextSize = (UINT16)MIN ((UINTN)TextSize,
                            MAX ((UINTN)20, mSfbRowHeight - 16));
    UINTN  TextY = mSfbGfxY + (mSfbRowHeight - TextSize) / 2;

    SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY, CardWidth, mSfbRowHeight,
                Selected ? &mSfbColorPrimary : &mSfbColorSurface);
    if (!Selected) {
      SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY, 4,
                  mSfbRowHeight, &mSfbColorDisabled);
    }
    SfbGfxIcon (CANOE_UI_SIDE_MARGIN + 28, IconY, IconSize, Icon,
                Selected ? &mSfbColorBackground : &mSfbColorMuted);
    SfbGfxText (TextX, TextY, TextSize, Text,
                Selected ? &mSfbColorBackground : &mSfbColorText);
    mSfbGfxY += mSfbRowStep;
    return;
  }

  UnicodeSPrint (Row, sizeof (Row), L"  %s  %-6s  %s",
                 Selected ? L">" : L" ",
                 FallbackMarker != NULL ? FallbackMarker : L" ", Text);
  SfbUiFullRow (Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL, Row);
}

/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
{
  if (Count <= Rows) {
    return 0;
  }
  if (Cursor < Rows / 2) {
    return 0;
  }
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }

  return Cursor - Rows / 2;
}

VOID
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key)
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  CHAR16  Detail[SFB_UI_LINE_CHARS];

  SfbBeginScreen (EFI_ERROR (Status) ? L"Action failed" : L"Action complete",
                  What);
  UnicodeSPrint (Detail, sizeof (Detail),
                 mSfbLanguage == 0 ? L"  状态  %r" : L"  Status  %r", Status);
  SfbUiFullRow (EFI_ERROR (Status) ? SFB_ATTR_ERROR : SFB_ATTR_SUCCESS,
                Detail);
  SfbEndScreen (L"Continue");
  SfbWaitForKey (0);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.
 */
VOID
SfbShowFastbootMode (VOID)
{
  UINTN Width;
  UINTN Height;
  UINTN CardTop;
  UINTN CardHeight;

  SfbLoadSettings ();
  SfbUiInitGraphics ();
  if (mSfbGraphical) {
    Width = mSfbGop->Mode->Info->HorizontalResolution;
    Height = mSfbGop->Mode->Info->VerticalResolution;
    mSfbSafeTop = MAX (CANOE_UI_SAFE_TOP_MIN, Height / 16);
    SfbGfxFill (0, 0, Width, Height, &mSfbColorBackground);
    SfbDrawStatusBar ();

    SfbGfxFill (0, mSfbSafeTop, Width,
                CANOE_UI_HEADER_HEIGHT, &mSfbColorSurface);
    SfbGfxFill (0, mSfbSafeTop + CANOE_UI_HEADER_HEIGHT - 4,
                Width, 4, &mSfbColorPrimary);
    SfbGfxCenteredText (
      mSfbSafeTop + (CANOE_UI_HEADER_HEIGHT - CANOE_UI_TITLE_FONT) / 2,
      CANOE_UI_TITLE_FONT, 34, L"SUPERFASTBOOT", &mSfbColorText);

    CardTop = mSfbSafeTop + CANOE_UI_HEADER_HEIGHT + 54;
    CardHeight = (Height > CardTop + CANOE_UI_FOOTER_HEIGHT + 50)
                   ? Height - CardTop - CANOE_UI_FOOTER_HEIGHT - 50 : 420;
    CardHeight = MIN ((UINTN)540, MAX ((UINTN)360, CardHeight));
    SfbGfxFill (CANOE_UI_SIDE_MARGIN, CardTop,
                Width - 2 * CANOE_UI_SIDE_MARGIN, CardHeight,
                &mSfbColorSurface);
    SfbGfxFill (CANOE_UI_SIDE_MARGIN, CardTop, 6, CardHeight,
                &mSfbColorPrimary);
    SfbGfxIcon (Width / 2 - 52, CardTop + 54, 104,
                CanoeIconUsb, &mSfbColorPrimary);
    SfbGfxCenteredText (CardTop + 196, 76, 48, L"ONLINE",
                        &mSfbColorSuccess);
    SfbGfxCenteredText (
      CardTop + 310, CANOE_UI_SUBTITLE_FONT, 22,
      mSfbLanguage == 0 ? L"USB 已就绪，可连接电脑"
                        : L"USB is ready for a host connection",
      &mSfbColorMuted);

    SfbGfxFill (0, Height - CANOE_UI_FOOTER_HEIGHT, Width,
                CANOE_UI_FOOTER_HEIGHT, &mSfbColorSurface);
    SfbGfxFill (0, Height - CANOE_UI_FOOTER_HEIGHT - 4,
                Width, 4, &mSfbColorPrimary);
    SfbGfxCenteredText (
      Height - 76, CANOE_UI_FOOTER_FONT, 22,
      mSfbLanguage == 0 ? L"使用 fastboot 命令管理设备"
                        : L"Manage this device with fastboot commands",
      &mSfbColorMuted);
    return;
  }

  SfbBeginScreen (L"Fastboot", L"USB service is ready");
  SfbUiFullRow (SFB_ATTR_SUCCESS, L"  ONLINE");
  SfbUiFullRow (SFB_ATTR_NORMAL,
                mSfbLanguage == 0
                  ? L"  连接电脑后使用 fastboot 管理设备。"
                  : L"  Connect a host and use fastboot to manage this device.");
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen)
{
  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  if (ClearScreen) {
    SfbBeginScreen (L"Launching", L"Starting the selected EFI application");
  }
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_SUCCESS);
  Print (L"  Booting %s ...\r\n",
         (Name != NULL && Name[0] != L'\0') ? Name : L"application");
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
VOID
SfbShowActionScreen (IN CONST CHAR16 *Text)
{
  SfbBeginScreen (L"Power", L"Please wait");
  SfbUiFullRow (SFB_ATTR_ACCENT, Text);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (VOID)
{
  SfbBeginScreen (L"Boot control", L"Preparing devices and boot entries");
  SfbUiFullRow (SFB_ATTR_ACCENT,
                mSfbLanguage == 0 ? L"  正在初始化 ..." : L"  INITIALIZING ...");

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

STATIC
CONST CHAR16 *
SfbThemeName (VOID)
{
  if (mSfbLanguage != 0) {
    switch (mSfbTheme) {
    case 1: return L"Purple";
    case 2: return L"Green";
    case 3: return L"Orange";
    default: return L"Blue";
    }
  }
  switch (mSfbTheme) {
  case 1: return L"紫色";
  case 2: return L"绿色";
  case 3: return L"橙色";
  default: return L"蓝色";
  }
}

STATIC
CONST CHAR16 *
SfbLockName (VOID)
{
  if (mSfbLanguage != 0) {
    switch (mSfbLockMode) {
    case SFB_LOCK_SIMPLE: return L"Simple";
    case SFB_LOCK_PIN:    return L"PIN";
    default:              return L"Off";
    }
  }
  switch (mSfbLockMode) {
  case SFB_LOCK_SIMPLE: return L"简易锁";
  case SFB_LOCK_PIN:    return L"PIN 密码";
  default:              return L"关闭";
  }
}

/* Three physical keys are available. Volume changes the current digit and
 * Power accepts it; after four digits the caller receives the result. */
STATIC
VOID
SfbEditPin (IN BOOLEAN MaskPrevious, OUT CHAR8 Pin[5])
{
  UINTN    Position;
  UINTN    Digit = 0;
  SFB_KEY  Key;

  for (Position = 0; Position < 4;) {
    CHAR16  Display[32];
    CHAR16  Progress[32];
    UINTN   Index;

    for (Index = 0; Index < 4; Index++) {
      if (Index < Position && MaskPrevious) {
        Display[Index * 2] = L'*';
      } else if (Index < Position) {
        Display[Index * 2] = (CHAR16)Pin[Index];
      } else if (Index == Position) {
        Display[Index * 2] = (CHAR16)(L'0' + Digit);
      } else {
        Display[Index * 2] = L'-';
      }
      Display[Index * 2 + 1] = L' ';
    }
    Display[7] = L'\0';
    UnicodeSPrint (Progress, sizeof (Progress),
                   mSfbLanguage == 0 ? L"当前第 %u 位" : L"Digit %u of 4",
                   (UINT32)(Position + 1));
    SfbBeginScreen (MaskPrevious ? L"Enter PIN" : L"Set PIN",
                    Progress);
    SfbDrawRow (TRUE, L"PIN", Display);
    SfbEndScreen (L"Next");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp) {
      Digit = (Digit + 1) % 10;
    } else if (Key == SfbKeyDown) {
      Digit = (Digit + 9) % 10;
    } else if (Key == SfbKeySelect) {
      Pin[Position++] = (CHAR8)('0' + Digit);
      Digit = 0;
    }
  }
  Pin[4] = '\0';
}

STATIC
VOID
SfbUnlock (VOID)
{
  if (mSfbLockMode == SFB_LOCK_OFF) {
    return;
  }

  if (mSfbLockMode == SFB_LOCK_SIMPLE) {
    CONST SFB_KEY  Sequence[4] = {
      SfbKeyUp, SfbKeyDown, SfbKeyUp, SfbKeySelect
    };
    UINTN  Position = 0;

    while (Position < ARRAY_SIZE (Sequence)) {
      CHAR16  Progress[32];
      SFB_KEY Key;

      UnicodeSPrint (Progress, sizeof (Progress),
                     mSfbLanguage == 0 ? L"输入进度  %u / 4" : L"Progress  %u / 4",
                     (UINT32)Position);
      SfbBeginScreen (L"Simple lock", Progress);
      SfbUiFullRow (SFB_ATTR_ACCENT,
                    mSfbLanguage == 0
                      ? L"顺序：音量+  音量-  音量+  电源键"
                      : L"Sequence: VOL+  VOL-  VOL+  POWER");
      SfbEndScreen (L"Unlock");
      Key = SfbWaitForKey (0);
      if (Key == Sequence[Position]) {
        Position++;
      } else {
        Position = (Key == Sequence[0]) ? 1 : 0;
      }
    }
    return;
  }

  while (TRUE) {
    CHAR8  Attempt[5];

    ZeroMem (Attempt, sizeof (Attempt));
    SfbEditPin (TRUE, Attempt);
    if (CompareMem (Attempt, mSfbPin, 4) == 0) {
      return;
    }
    SfbBeginScreen (L"Wrong password", L"Try again");
    SfbUiFullRow (SFB_ATTR_ERROR,
                  mSfbLanguage == 0 ? L"PIN 不正确" : L"Incorrect PIN");
    SfbEndScreen (L"Retry");
    SfbWaitForKey (0);
  }
}

STATIC
VOID
SfbRunSettings (VOID)
{
  UINTN    Cursor = 0;
  SFB_KEY  Key;

  while (TRUE) {
    CHAR16  Theme[48];
    CHAR16  Language[48];
    CHAR16  Lock[48];
    UINTN   Count = (mSfbLockMode == SFB_LOCK_PIN) ? 5 : 4;

    UnicodeSPrint (Theme, sizeof (Theme),
                   mSfbLanguage == 0 ? L"配色主题    %s" : L"Color theme    %s",
                   SfbThemeName ());
    UnicodeSPrint (Language, sizeof (Language),
                   mSfbLanguage == 0 ? L"语言    中文" : L"Language    English");
    UnicodeSPrint (Lock, sizeof (Lock),
                   mSfbLanguage == 0 ? L"锁定方式    %s" : L"Lock mode    %s",
                   SfbLockName ());
    SfbSetVisibleRows (Count);
    SfbBeginScreen (L"Settings",
                    mSfbLanguage == 0 ? L"选择一项进行更改"
                                      : L"Select an item to change");
    SfbDrawRow ((BOOLEAN)(Cursor == 0), L"COLOR", Theme);
    SfbDrawRow ((BOOLEAN)(Cursor == 1), L"LANG", Language);
    SfbDrawRow ((BOOLEAN)(Cursor == 2), L"LOCK", Lock);
    if (mSfbLockMode == SFB_LOCK_PIN) {
      SfbDrawRow ((BOOLEAN)(Cursor == 3), L"PIN",
                  mSfbLanguage == 0 ? L"更改 PIN 密码" : L"Change PIN");
      SfbDrawRow ((BOOLEAN)(Cursor == 4), L"BACK", SfbLocalize (L"Back"));
    } else {
      SfbDrawRow ((BOOLEAN)(Cursor == 3), L"BACK", SfbLocalize (L"Back"));
    }
    SfbEndScreen (L"Select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Count, Key);
      continue;
    }
    if (Cursor == 0) {
      mSfbTheme = (mSfbTheme + 1) % SFB_THEME_COUNT;
      SfbApplyPalette ();
      SfbSaveSettings ();
    } else if (Cursor == 1) {
      mSfbLanguage = (mSfbLanguage + 1) % 2;
      SfbSaveSettings ();
      Cursor = 1;
    } else if (Cursor == 2) {
      UINTN  NewMode = (mSfbLockMode + 1) % 3;

      if (NewMode == SFB_LOCK_PIN) {
        CHAR8  NewPin[5];

        ZeroMem (NewPin, sizeof (NewPin));
        SfbEditPin (FALSE, NewPin);
        CopyMem (mSfbPin, NewPin, sizeof (mSfbPin));
      }
      mSfbLockMode = NewMode;
      SfbSaveSettings ();
      Cursor = 2;
    } else if (mSfbLockMode == SFB_LOCK_PIN && Cursor == 3) {
      CHAR8  NewPin[5];

      ZeroMem (NewPin, sizeof (NewPin));
      SfbEditPin (FALSE, NewPin);
      CopyMem (mSfbPin, NewPin, sizeof (mSfbPin));
      SfbSaveSettings ();
    } else {
      return;
    }
  }
}

/* ---- boot menu ---------------------------------------------------------- */

STATIC
VOID
SfbDrawMenu (IN CONST SFB_MENU_STATE *Menu,
             IN UINTN                Cursor,
             IN CONST CHAR16         *Title)
{
  UINTN  Start;
  UINTN  Index;
  UINTN  Last;

  CHAR16  Summary[SFB_UI_LINE_CHARS];

  UnicodeSPrint (Summary, sizeof (Summary),
                 mSfbLanguage == 0
                   ? L"%u 个启动项  /  * 表示默认项"
                   : L"%u boot entries  /  * marks default",
                 (UINT32)Menu->Count);
  SfbSetVisibleRows (MIN (Menu->Count, (UINTN)SFB_VISIBLE_ROWS));
  SfbBeginScreen (Title, Summary);

  if (Menu->Count == 0) {
    Print (L"  No boot entries found.\r\n");
  }

  Start = SfbWindowStart (Cursor, Menu->Count, SFB_VISIBLE_ROWS);
  Last = Start + SFB_VISIBLE_ROWS;
  if (Last > Menu->Count) {
    Last = Menu->Count;
  }

  for (Index = Start; Index < Last; Index++) {
    CONST SFB_BOOT_ENTRY  *Entry = &Menu->Entry[Index];
    CONST CHAR16          *Marker;
    CANOE_UI_ICON         Icon;

    if (Index == Menu->DefaultIndex) {
      Marker = L"*";
    } else {
      switch (Entry->Kind) {
      case SfbEntryEfiFile:   Marker = L"EFI";   break;
      case SfbEntrySubmenu:   Marker = L"MENU";  break;
      case SfbEntryFastboot:  Marker = L"USB";   break;
      case SfbEntrySelector:  Marker = L"FILES"; break;
      case SfbEntrySettings:  Marker = L"SET";   break;
      case SfbEntryBack:      Marker = L"BACK";  break;
      case SfbEntryPowerOff:
      case SfbEntryRestart:   Marker = L"POWER"; break;
      default:                Marker = L"";      break;
      }
    }

    switch (Entry->Kind) {
    case SfbEntryEfiFile:   Icon = (Index == Menu->DefaultIndex) ? CanoeIconInfo : CanoeIconBoot; break;
    case SfbEntrySubmenu:   Icon = CanoeIconTool; break;
    case SfbEntryFastboot:  Icon = CanoeIconUsb; break;
    case SfbEntrySelector:  Icon = CanoeIconFile; break;
    case SfbEntrySettings:  Icon = CanoeIconSettings; break;
    case SfbEntryBack:      Icon = CanoeIconBack; break;
    case SfbEntryPowerOff:  Icon = CanoeIconPower; break;
    case SfbEntryRestart:   Icon = CanoeIconRestart; break;
    default:                Icon = CanoeIconNone; break;
    }

    /* Submenu rows get a trailing '>' so it is obvious they open another list
     * rather than launch an image. */
    if (Entry->Kind == SfbEntrySubmenu) {
      CHAR16  Text[SFB_DESC_CHARS + 4];

      UnicodeSPrint (Text, sizeof (Text), L"%s >",
                     SfbUiEntryText (Entry->Kind, Entry->Desc));
      SfbDrawRowIcon ((BOOLEAN)(Index == Cursor), Icon, Marker, Text);
    } else {
      SfbDrawRowIcon ((BOOLEAN)(Index == Cursor), Icon, Marker,
                      SfbUiEntryText (Entry->Kind, Entry->Desc));
    }
  }

  if (Last < Menu->Count) {
    Print (L"    ... %u more\r\n", (UINT32)(Menu->Count - Last));
  }

  SfbEndScreen (L"Open");
}

/*
 * Run a submenu defined by the ENTRIES file at EntriesPath on Volume. The file
 * is parsed exactly like the root BOOTENTRIES, and may itself contain further
 * '%' submenu rows; Depth bounds the nesting so a chain of files that points at
 * one another cannot recurse without limit. The submenu state is heap-allocated
 * (a single SFB_MENU_STATE is ~17 KB) so deep nesting stays off the call stack.
 *
 * Returns when the user picks the trailing "Back" row, or when the file could
 * not be built at all; the caller then redraws its own menu.
 */
STATIC
VOID
SfbRunSubMenu (IN EFI_HANDLE   Volume,
               IN CONST CHAR16 *EntriesPath,
               IN CONST CHAR16 *Title,
               IN UINTN        Depth)
{
  SFB_MENU_STATE  *Menu = NULL;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  Menu = AllocateZeroPool (sizeof (*Menu));
  if (Menu == NULL) {
    return;
  }
  Menu->DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (Menu);
      Status = SfbBuildSubMenu (Menu, Volume, EntriesPath);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (Title, Status);
        break;
      }
      Cursor = 0;
      Rebuild = FALSE;
    }

    SfbDrawMenu (Menu, Cursor, Title);

    /* Same input model as the root menu: volume keys move, power confirms. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu->Count, Key);
      continue;
    }

    if (Menu->Count == 0) {
      continue;
    }

    Chosen = Cursor;
    switch (Menu->Entry[Chosen].Kind) {
    case SfbEntryBack:
      goto done;

    case SfbEntrySubmenu:
      if (Depth >= SFB_MAX_SUBMENU_DEPTH) {
        SfbReportStatus (L"Submenu too deep", EFI_BUFFER_TOO_SMALL);
      } else {
        SfbRunSubMenu (Menu->Entry[Chosen].Volume,
                       Menu->Entry[Chosen].Path,
                       Menu->Entry[Chosen].Desc,
                       Depth + 1);
      }
      /* Media may have changed while the child menu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu->Entry[Chosen], TRUE, TRUE);//Entries in submenu never defaults
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      Rebuild = TRUE;
      break;
    }
  }

done:
  SfbFreeMenu (Menu);
  FreePool (Menu);
}

BOOLEAN
SfbRunBootMenu (VOID)
{
  SFB_MENU_STATE  Menu;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  SfbLoadSettings ();
  SfbUnlock ();

  ZeroMem (&Menu, sizeof (Menu));
  Menu.DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (&Menu);
      SfbBuildMenu (&Menu);
      Cursor = (Menu.DefaultIndex == SFB_NO_INDEX) ? 0 : Menu.DefaultIndex;
      Rebuild = FALSE;
    }

    SfbDrawMenu (&Menu, Cursor, L"Boot Menu");

    /* The menu is purely interactive: it waits for a key indefinitely and
     * never launches anything unattended. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu.Count, Key);
      continue;
    }

    Chosen = Cursor;

    if (Menu.Count == 0) {
      continue;
    }

    switch (Menu.Entry[Chosen].Kind) {
    case SfbEntryFastboot:
      SfbFreeMenu (&Menu);
      return TRUE;

    case SfbEntrySelector:
      SfbRunFileBrowser ();
      /* The browser may have added a custom entry. */
      Rebuild = TRUE;
      break;

    case SfbEntrySettings:
      SfbRunSettings ();
      Rebuild = TRUE;
      break;

    case SfbEntrySubmenu:
      SfbRunSubMenu (Menu.Entry[Chosen].Volume,
                     Menu.Entry[Chosen].Path,
                     Menu.Entry[Chosen].Desc,
                     1);
      /* Media may have changed while the submenu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryBack:
      /* Only submenus carry a Back row; the root menu never adds one. */
      Rebuild = TRUE;
      break;

    case SfbEntryPowerOff:
      SfbShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      break;

    case SfbEntryRestart:
      SfbShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu.Entry[Chosen], FALSE, TRUE);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      /* Media or variables may have changed while the image ran. */
      Rebuild = TRUE;
      break;
    }
  }
}
