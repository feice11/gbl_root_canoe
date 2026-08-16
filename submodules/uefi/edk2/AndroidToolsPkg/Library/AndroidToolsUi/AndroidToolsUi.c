/** @file
 * Shared graphical UI for every AndroidToolsPkg application.
 *
 * BootMenu passes CUI2|<language>|<theme>|<clock-offset>|<clock-valid> through
 * EFI LoadOptions.  Legacy CUI1 records remain accepted.  This keeps every
 * tool visually and linguistically in sync without writing a second settings
 * store. Volume up/down move, power confirms; menu confirmation is protected
 * by the same quiet-window debounce used by BootMenu.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EFIChargerEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include "AndroidToolsUi.h"
#include "../../../QcomModulePkg/Application/LinuxLoader/SuperFbFont.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)  (sizeof (a) / sizeof ((a)[0]))
#endif

#define AT_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define AT_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE)
#define AT_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)
#define AT_ENTER_MENU_DELAY_S  2
#define AT_THEME_COUNT  4

STATIC CONST CANOE_UI_PALETTE  mAtPalettes[AT_THEME_COUNT] =
  CANOE_UI_PALETTE_INITIALIZERS;

STATIC BOOLEAN                       mAtChinese = TRUE;
STATIC UINTN                         mAtTheme = 0;
STATIC BOOLEAN                       mAtGraphical = FALSE;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mAtGop = NULL;
STATIC UINTN                         mAtSafeTop = 192;
STATIC UINTN                         mAtY = 0;
STATIC UINTN                         mAtRowHeight = 104;
STATIC UINTN                         mAtRowStep = 118;
STATIC UINTN                         mAtVisibleRows = CANOE_UI_VISIBLE_MIN;
STATIC UINTN                         mAtClockOffsetSeconds = 0;
STATIC BOOLEAN                       mAtClockValid = FALSE;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtBackground;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtSurface;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtPrimary;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtText = { 0xf4, 0xf4, 0xf4, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtMuted = { 0xb0, 0xa8, 0x9f, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtDisabled = { 0x68, 0x62, 0x5d, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtSuccess = { 0x78, 0xd6, 0x55, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtWarning = { 0x42, 0xa5, 0xff, 0 };

STATIC
VOID
AtGfxFill (IN UINTN X, IN UINTN Y, IN UINTN Width, IN UINTN Height,
           IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  if (!mAtGraphical || Width == 0 || Height == 0) {
    return;
  }
  mAtGop->Blt (mAtGop, Color, EfiBltVideoFill, 0, 0, X, Y, Width, Height, 0);
}

STATIC
UINTN
AtGfxMeasureText (IN UINT16 Size, IN CONST CHAR16 *Text)
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
AtGfxFitText (IN UINT16 Preferred, IN UINT16 Minimum,
              IN UINTN Available, IN CONST CHAR16 *Text)
{
  UINT16 Size = Preferred;
  while (Size > Minimum && AtGfxMeasureText (Size, Text) > Available) {
    Size = (UINT16)(Size - 2);
  }
  return Size;
}

STATIC
VOID
AtGfxIcon (IN UINTN X, IN UINTN Y, IN UINTN Size, IN CANOE_UI_ICON Icon,
           IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  UINTN U = MAX (2, Size / 9);
  UINTN C = Size / 2;

  if (Icon == CanoeIconNone) return;
  if (Icon == CanoeIconBoot || Icon == CanoeIconRestart || Icon == CanoeIconBack) {
    AtGfxFill (X + U, Y + C - U, Size - 2 * U, 2 * U, Color);
    AtGfxFill (Icon == CanoeIconBack ? X + U : X + Size - 3 * U,
               Y + C - 3 * U, 2 * U, 6 * U, Color);
  } else if (Icon == CanoeIconFile) {
    AtGfxFill (X + 2 * U, Y + U, Size - 4 * U, U, Color);
    AtGfxFill (X + 2 * U, Y + U, U, Size - 2 * U, Color);
    AtGfxFill (X + 2 * U, Y + Size - 2 * U, Size - 4 * U, U, Color);
    AtGfxFill (X + Size - 3 * U, Y + 3 * U, U, Size - 4 * U, Color);
  } else if (Icon == CanoeIconUsb) {
    AtGfxFill (X + C - U / 2, Y + U, U, Size - 3 * U, Color);
    AtGfxFill (X + C, Y + 3 * U, 3 * U, U, Color);
    AtGfxFill (X + C - 3 * U, Y + 5 * U, 3 * U, U, Color);
    AtGfxFill (X + C - U, Y + Size - 2 * U, 3 * U, U, Color);
  } else if (Icon == CanoeIconLock || Icon == CanoeIconPin) {
    AtGfxFill (X + 2 * U, Y + 4 * U, Size - 4 * U, Size - 5 * U, Color);
    AtGfxFill (X + 3 * U, Y + U, U, 4 * U, Color);
    AtGfxFill (X + Size - 4 * U, Y + U, U, 4 * U, Color);
    AtGfxFill (X + 3 * U, Y + U, Size - 6 * U, U, Color);
  } else if (Icon == CanoeIconGame) {
    AtGfxFill (X + U, Y + 3 * U, Size - 2 * U, 4 * U, Color);
    AtGfxFill (X + 3 * U, Y + 2 * U, U, 6 * U, Color);
    AtGfxFill (X + 2 * U, Y + 4 * U, 3 * U, U, Color);
    AtGfxFill (X + Size - 4 * U, Y + 4 * U, U, U, &mAtBackground);
  } else if (Icon == CanoeIconWarning) {
    AtGfxFill (X + C - U / 2, Y + U, U, 5 * U, Color);
    AtGfxFill (X + C - U / 2, Y + 7 * U, U, U, Color);
  } else if (Icon == CanoeIconPalette) {
    AtGfxFill (X + U, Y + 2 * U, Size - 2 * U, 5 * U, Color);
    AtGfxFill (X + 3 * U, Y + 3 * U, U, U, &mAtBackground);
    AtGfxFill (X + 5 * U, Y + 3 * U, U, U, &mAtBackground);
  } else {
    AtGfxFill (X + U, Y + U, Size - 2 * U, U, Color);
    AtGfxFill (X + U, Y + Size - 2 * U, Size - 2 * U, U, Color);
    AtGfxFill (X + U, Y + U, U, Size - 2 * U, Color);
    AtGfxFill (X + Size - 2 * U, Y + U, U, Size - 2 * U, Color);
    AtGfxFill (X + C - U / 2, Y + 3 * U, U, 3 * U, Color);
  }
}

STATIC
CANOE_UI_ICON
AtIconForText (IN CONST CHAR16 *Text)
{
  if (Text == NULL) return CanoeIconNone;
  if (StrStr (Text, L"Back") != NULL || StrStr (Text, L"返回") != NULL) return CanoeIconBack;
  if (StrStr (Text, L"Reboot") != NULL || StrStr (Text, L"重启") != NULL) return CanoeIconRestart;
  if (StrStr (Text, L"Lock") != NULL || StrStr (Text, L"Unlock") != NULL ||
      StrStr (Text, L"锁") != NULL) return CanoeIconLock;
  if (StrStr (Text, L"ARB") != NULL) return CanoeIconWarning;
  if (StrStr (Text, L"Game") != NULL || StrStr (Text, L"游戏") != NULL ||
      StrStr (Text, L"Reaction") != NULL || StrStr (Text, L"Guess") != NULL) return CanoeIconGame;
  return CanoeIconTool;
}

STATIC
EFI_STATUS
AtGfxText (IN UINTN X, IN UINTN Y, IN UINT16 Size, IN CONST CHAR16 *Text,
           IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer;
  CONST SFB_FONT_GLYPH           *Glyph;
  EFI_STATUS                     Status;
  UINTN                          TextWidth = 0;
  UINTN                          Width;
  UINTN                          Height;
  UINTN                          Cursor;
  UINTN                          Index;
  UINTN                          GlyphIndex;
  UINTN                          Dx;
  UINTN                          Dy;
  UINTN                          Sx;
  UINTN                          Sy;
  UINTN                          Advance;
  UINTN                          Alpha;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel;

  if (!mAtGraphical || Text == NULL || Size == 0) {
    return EFI_UNSUPPORTED;
  }
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
  if (X >= mAtGop->Mode->Info->HorizontalResolution ||
      Y >= mAtGop->Mode->Info->VerticalResolution || TextWidth == 0) {
    return EFI_SUCCESS;
  }
  Width = MIN (TextWidth, mAtGop->Mode->Info->HorizontalResolution - X);
  Height = MIN ((UINTN)Size, mAtGop->Mode->Info->VerticalResolution - Y);
  Buffer = AllocatePool (Width * Height * sizeof (*Buffer));
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = mAtGop->Blt (mAtGop, Buffer, EfiBltVideoToBltBuffer,
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
        Alpha = ((Sx & 1) == 0) ? (Alpha >> 4) : (Alpha & 0x0f);
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
  Status = mAtGop->Blt (mAtGop, Buffer, EfiBltBufferToVideo,
                        0, 0, X, Y, Width, Height,
                        Width * sizeof (*Buffer));
  FreePool (Buffer);
  return Status;
}

STATIC
UINTN
AtBatteryPercentFromVoltage (IN UINT32 Millivolts)
{
  STATIC CONST struct { UINT32 Millivolts; UINTN Percent; } Curve[] = {
    {3400,0}, {3600,5}, {3700,12}, {3800,25}, {3900,40}, {4000,55},
    {4100,68}, {4200,78}, {4300,86}, {4400,93}, {4550,100}
  };
  UINTN Index;
  if (Millivolts <= Curve[0].Millivolts) return 0;
  for (Index = 1; Index < ARRAY_SIZE (Curve); Index++) {
    if (Millivolts <= Curve[Index].Millivolts) {
      UINT32 Span = Curve[Index].Millivolts - Curve[Index - 1].Millivolts;
      return Curve[Index - 1].Percent +
             (UINTN)((Millivolts - Curve[Index - 1].Millivolts) *
             (Curve[Index].Percent - Curve[Index - 1].Percent) / Span);
    }
  }
  return 100;
}

STATIC
VOID
AtReadPowerStatus (OUT BOOLEAN *Available, OUT UINTN *Percent,
                   OUT BOOLEAN *Charging)
{
  EFI_CHARGER_EX_PROTOCOL *Charger = NULL;
  UINT32 Millivolts = 0;
  BOOLEAN Present = FALSE;
  *Available = FALSE; *Percent = 0; *Charging = FALSE;
  if (EFI_ERROR (gBS->LocateProtocol (&gChargerExProtocolGuid, NULL,
                                      (VOID **)&Charger)) || Charger == NULL) return;
  if (Charger->GetChargerPresence != NULL) {
    (VOID)Charger->GetChargerPresence (&Present);
    *Charging = Present;
  }
  if (Charger->GetBatteryVoltage == NULL ||
      EFI_ERROR (Charger->GetBatteryVoltage (&Millivolts)) ||
      Millivolts < 2500 || Millivolts > 5000) return;
  *Available = TRUE;
  *Percent = AtBatteryPercentFromVoltage (Millivolts);
}

STATIC
VOID
AtDrawStatusBar (VOID)
{
  EFI_TIME Time;
  CHAR16 TimeText[16];
  CHAR16 PercentText[16];
  BOOLEAN Available;
  BOOLEAN Charging;
  UINTN Percent;
  UINTN Width;
  UINTN StatusY;
  UINTN BatteryX;
  UINTN BatteryY;
  UINTN FillWidth;
  UINTN LocalSeconds;

  if (!mAtGraphical || mAtSafeTop < 48) return;
  Width = mAtGop->Mode->Info->HorizontalResolution;
  StatusY = (mAtSafeTop - CANOE_UI_STATUS_FONT) / 2;
  if (mAtClockValid && !EFI_ERROR (gRT->GetTime (&Time, NULL)) &&
      Time.Hour < 24 && Time.Minute < 60 && Time.Second < 60) {
    LocalSeconds = ((UINTN)Time.Hour * 3600 + (UINTN)Time.Minute * 60 +
                    (UINTN)Time.Second + mAtClockOffsetSeconds) % 86400;
    UnicodeSPrint (TimeText, sizeof (TimeText), L"%02u:%02u",
                   (UINT32)(LocalSeconds / 3600),
                   (UINT32)((LocalSeconds / 60) % 60));
  } else {
    StrCpyS (TimeText, ARRAY_SIZE (TimeText), L"--:--");
  }
  AtGfxText (CANOE_UI_SIDE_MARGIN, StatusY, CANOE_UI_STATUS_FONT,
             TimeText, &mAtText);
  AtReadPowerStatus (&Available, &Percent, &Charging);
  UnicodeSPrint (PercentText, sizeof (PercentText),
                 Available ? L"%u%%" : L"--%%", (UINT32)Percent);
  BatteryX = Width - 220;
  BatteryY = StatusY + 3;
  AtGfxFill (BatteryX, BatteryY, 56, 3, &mAtMuted);
  AtGfxFill (BatteryX, BatteryY + 25, 56, 3, &mAtMuted);
  AtGfxFill (BatteryX, BatteryY, 3, 28, &mAtMuted);
  AtGfxFill (BatteryX + 53, BatteryY, 3, 28, &mAtMuted);
  AtGfxFill (BatteryX + 56, BatteryY + 8, 5, 12, &mAtMuted);
  if (Available && Percent != 0) {
    FillWidth = MAX (2, 46 * Percent / 100);
    AtGfxFill (BatteryX + 5, BatteryY + 5, FillWidth, 18,
               Charging ? &mAtPrimary : &mAtText);
  }
  if (Charging) {
    AtGfxFill (BatteryX - 30, BatteryY + 2, 13, 8, &mAtPrimary);
    AtGfxFill (BatteryX - 24, BatteryY + 8, 13, 8, &mAtPrimary);
    AtGfxFill (BatteryX - 18, BatteryY + 14, 7, 11, &mAtPrimary);
  }
  AtGfxText (Width - 140, StatusY, CANOE_UI_STATUS_FONT,
             PercentText, &mAtText);
}

VOID
AtUiInitialize (IN EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage = NULL;
  CONST CHAR16               *Options;
  EFI_STATUS                 Status;
  CONST CHAR16               *ClockFlag;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle,
                                       &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&LoadedImage)) &&
      LoadedImage != NULL && LoadedImage->LoadOptions != NULL &&
      LoadedImage->LoadOptionsSize >= 9 * sizeof (CHAR16)) {
    Options = (CONST CHAR16 *)LoadedImage->LoadOptions;
    if ((StrnCmp (Options, L"CUI1|", 5) == 0 ||
         StrnCmp (Options, L"CUI2|", 5) == 0) &&
        (Options[5] == L'0' || Options[5] == L'1') &&
        Options[6] == L'|' && Options[7] >= L'0' &&
        Options[7] < L'0' + AT_THEME_COUNT) {
      mAtChinese = (BOOLEAN)(Options[5] == L'0');
      mAtTheme = Options[7] - L'0';
      if (StrnCmp (Options, L"CUI2|", 5) == 0 && Options[8] == L'|') {
        mAtClockOffsetSeconds = StrDecimalToUintn (Options + 9) % 86400;
        ClockFlag = StrStr (Options + 9, L"|");
        mAtClockValid = (BOOLEAN)(ClockFlag != NULL && ClockFlag[1] == L'1');
      }
    }
  }
  mAtBackground = mAtPalettes[mAtTheme].Background;
  mAtSurface = mAtPalettes[mAtTheme].Surface;
  mAtPrimary = mAtPalettes[mAtTheme].Primary;
  mAtText = mAtPalettes[mAtTheme].Text;
  mAtMuted = mAtPalettes[mAtTheme].Muted;
  mAtDisabled = mAtPalettes[mAtTheme].Disabled;
  mAtSuccess = mAtPalettes[mAtTheme].Success;
  mAtWarning = mAtPalettes[mAtTheme].Warning;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&mAtGop);
  mAtGraphical = (BOOLEAN)(!EFI_ERROR (Status) && mAtGop != NULL &&
                            mAtGop->Mode != NULL && mAtGop->Mode->Info != NULL);
  if (mAtGraphical) {
    mAtSafeTop = MAX (CANOE_UI_SAFE_TOP_MIN,
                      mAtGop->Mode->Info->VerticalResolution / 16);
  }
}

BOOLEAN
AtUiIsChinese (VOID)
{
  return mAtChinese;
}

CONST CHAR16 *
AtUiLocalize (IN CONST CHAR16 *Text)
{
  if (Text == NULL) return L"";
  if (!mAtChinese) return Text;
  if (StrCmp (Text, L"Reboot Tools") == 0) return L"重启工具";
  if (StrCmp (Text, L"Reboot to Fastbootd") == 0) return L"重启到 Fastbootd";
  if (StrCmp (Text, L"Reboot to Bootloader") == 0) return L"重启到 Bootloader";
  if (StrCmp (Text, L"Reboot to Recovery") == 0) return L"重启到 Recovery";
  if (StrCmp (Text, L"Reboot to System") == 0) return L"重启到安卓系统";
  if (StrCmp (Text, L"Rebooting to Fastbootd...") == 0) return L"正在重启到 Fastbootd...";
  if (StrCmp (Text, L"Rebooting to Bootloader...") == 0) return L"正在重启到 Bootloader...";
  if (StrCmp (Text, L"Rebooting to Recovery...") == 0) return L"正在重启到 Recovery...";
  if (StrCmp (Text, L"Rebooting to System...") == 0) return L"正在重启到安卓系统...";
  if (StrCmp (Text, L"ARB Tools") == 0) return L"ARB 工具";
  if (StrCmp (Text, L"Get ARB Value") == 0) return L"查看 ARB 值";
  if (StrCmp (Text, L"Reset ARB Value") == 0) return L"重置 ARB 值";
  if (StrCmp (Text, L"ARB Rollback Index (non-zero)") == 0) return L"ARB 回滚索引（非零）";
  if (StrCmp (Text, L"Reset ARB Index") == 0) return L"重置 ARB 索引";
  if (StrCmp (Text, L"All rollback slots are 0") == 0) return L"所有回滚槽位均为 0";
  if (StrCmp (Text, L"Reset cancelled") == 0) return L"已取消重置";
  if (StrCmp (Text, L"Resetting ARB index...") == 0) return L"正在重置 ARB 索引...";
  if (StrCmp (Text, L"ARB index reset complete") == 0) return L"ARB 索引重置完成";
  if (StrCmp (Text, L"BL Tools") == 0) return L"BL 状态工具";
  if (StrCmp (Text, L"Unlock Device") == 0) return L"解锁设备";
  if (StrCmp (Text, L"Lock Device") == 0) return L"锁定设备";
  if (StrCmp (Text, L"Unlock Critical") == 0) return L"解锁关键分区";
  if (StrCmp (Text, L"Lock Critical") == 0) return L"锁定关键分区";
  if (StrCmp (Text, L"State unchanged") == 0) return L"状态没有变化";
  if (StrCmp (Text, L"Applying...") == 0) return L"正在应用...";
  if (StrCmp (Text, L"Done. Reboot for the change to take effect.") == 0) return L"操作完成，重启后生效。";
  if (StrCmp (Text, L"DeviceInfo not initialized") == 0) return L"DeviceInfo 尚未初始化";
  if (StrCmp (Text, L"Cancelled") == 0) return L"已取消";
  if (StrCmp (Text, L"Back") == 0) return L"返回";
  if (StrCmp (Text, L"Cancel") == 0) return L"取消";
  if (StrCmp (Text, L"Confirm action") == 0) return L"确认执行";
  if (StrCmp (Text, L"Review warning") == 0) return L"请阅读风险说明";
  if (StrCmp (Text, L"Press power to review") == 0) return L"按电源键进入最终确认";
  if (StrCmp (Text, L"Notice") == 0) return L"提示";
  if (StrCmp (Text, L"Error") == 0) return L"错误";
  if (StrCmp (Text, L"Entering") == 0) return L"正在进入";
  if (StrCmp (Text, L"Read DeviceInfo") == 0) return L"读取 DeviceInfo";
  if (StrCmp (Text, L"Write DeviceInfo") == 0) return L"写入 DeviceInfo";
  if (StrCmp (Text, L"DeviceInfo magic") == 0) return L"DeviceInfo 校验";
  if (StrCmp (Text, L"Write BCB") == 0) return L"写入 BCB";
  if (StrCmp (Text, L"Alloc") == 0) return L"分配内存";
  if (StrCmp (Text, L"Mini Games") == 0) return L"小游戏";
  if (StrCmp (Text, L"Guess Number") == 0) return L"猜数字";
  if (StrCmp (Text, L"Reaction Challenge") == 0) return L"反应挑战";
  if (StrCmp (Text, L"You got it!") == 0) return L"猜中了！";
  if (StrCmp (Text, L"False start!") == 0) return L"抢跑了！";
  if (StrCmp (Text, L"Too high") == 0) return L"太大了";
  if (StrCmp (Text, L"Too low") == 0) return L"太小了";
  if (StrCmp (Text, L"Wait for GO") == 0) return L"等待开始信号";
  if (StrCmp (Text, L"GO! Press power") == 0) return L"开始！按下电源键";
  if (StrCmp (Text, L"Vol+/- move, power select") == 0) return L"音量 +/- 移动，电源键确认";
  if (StrCmp (Text, L"Vol+/- scroll, power to return") == 0) return L"音量 +/- 滚动，电源键返回";
  if (StrCmp (Text, L"Vol+/- change, power submit") == 0) return L"音量 +/- 调整，电源键提交";
  if (StrCmp (Text, L"Press power to continue") == 0) return L"按电源键继续";
  return Text;
}

AT_KEY
AtUiWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount = 1;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  AT_KEY         Result = AtKeyTimeout;

  if (TimeoutMs != 0 &&
      !EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL,
                                    &TimerEvent))) {
    if (EFI_ERROR (gBS->SetTimer (TimerEvent, TimerRelative,
                                  (UINT64)TimeoutMs * 10000))) {
      gBS->CloseEvent (TimerEvent);
      TimerEvent = NULL;
    }
  }
  WaitList[0] = gST->ConIn->WaitForKey;
  if (TimerEvent != NULL) {
    WaitList[1] = TimerEvent;
    WaitCount = 2;
  }
  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status) || EventIndex == 1) break;
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) continue;
    if (Key.ScanCode == SCAN_UP) Result = AtKeyUp;
    else if (Key.ScanCode == SCAN_DOWN) Result = AtKeyDown;
    else Result = AtKeySelect;
    break;
  }
  if (TimerEvent != NULL) gBS->CloseEvent (TimerEvent);
  return Result;
}

VOID
AtUiDebounce (VOID)
{
  EFI_EVENT      TimerEvent;
  EFI_EVENT      WaitList[2];
  EFI_INPUT_KEY  Key;
  UINTN          EventIndex;

  gBS->Stall (200000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
  if (EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL,
                                   &TimerEvent))) return;
  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;
  while (TRUE) {
    gBS->SetTimer (TimerEvent, TimerRelative, 220ULL * 10000);
    if (EFI_ERROR (gBS->WaitForEvent (2, WaitList, &EventIndex)) ||
        EventIndex == 1) break;
    while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) { }
  }
  gBS->CloseEvent (TimerEvent);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

VOID
AtUiEnterMenu (IN CONST CHAR16 *Title)
{
  AtUiBeginScreen (Title, L"Entering");
  gBS->Stall (AT_ENTER_MENU_DELAY_S * 1000 * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

VOID
AtUiBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  UINTN  Width;
  UINTN  Height;
  UINTN  ContentTop;
  UINTN  Available;
  UINT16 TitleSize;
  Title = AtUiLocalize (Title);
  Subtitle = (Subtitle != NULL) ? AtUiLocalize (Subtitle) : NULL;
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  if (mAtGraphical) {
    Width = mAtGop->Mode->Info->HorizontalResolution;
    Height = mAtGop->Mode->Info->VerticalResolution;
    AtGfxFill (0, 0, Width, Height, &mAtBackground);
    AtDrawStatusBar ();
    AtGfxFill (0, mAtSafeTop, Width, CANOE_UI_HEADER_HEIGHT, &mAtSurface);
    AtGfxFill (0, mAtSafeTop + CANOE_UI_HEADER_HEIGHT - 4,
               Width, 4, &mAtPrimary);
    TitleSize = AtGfxFitText (CANOE_UI_TITLE_FONT, 34,
                             Width - 2 * CANOE_UI_SIDE_MARGIN, Title);
    AtGfxText (CANOE_UI_SIDE_MARGIN, mAtSafeTop + 38,
               TitleSize, Title, &mAtText);
    if (Subtitle != NULL) {
      AtGfxText (CANOE_UI_SIDE_MARGIN,
                 mAtSafeTop + CANOE_UI_HEADER_HEIGHT + 24,
                 AtGfxFitText (CANOE_UI_SUBTITLE_FONT, 22,
                               Width - 2 * CANOE_UI_SIDE_MARGIN, Subtitle),
                 Subtitle, &mAtMuted);
      ContentTop = mAtSafeTop + CANOE_UI_HEADER_HEIGHT + 82;
    } else {
      ContentTop = mAtSafeTop + CANOE_UI_HEADER_HEIGHT + 34;
    }
    mAtY = ContentTop;
    Available = (Height > ContentTop + CANOE_UI_FOOTER_HEIGHT + 20)
                  ? Height - ContentTop - CANOE_UI_FOOTER_HEIGHT - 20 : 0;
    mAtRowStep = (mAtVisibleRows != 0) ? Available / mAtVisibleRows : 0;
    mAtRowStep = MIN (CANOE_UI_CARD_MAX_HEIGHT + CANOE_UI_CARD_GAP,
                      MAX ((UINTN)44, mAtRowStep));
    mAtRowHeight = mAtRowStep - CANOE_UI_CARD_GAP;
    return;
  }
  gST->ConOut->SetAttribute (gST->ConOut, AT_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"%s\r\n", Title);
  gST->ConOut->SetAttribute (gST->ConOut, AT_ATTR_NORMAL);
  if (Subtitle != NULL) Print (L"%s\r\n", Subtitle);
  Print (L"\r\n");
}

VOID
AtUiEndScreen (IN CONST CHAR16 *Footer)
{
  UINTN  Width;
  UINTN  Height;
  Footer = (Footer != NULL) ? AtUiLocalize (Footer) : NULL;
  if (mAtGraphical) {
    if (Footer == NULL) return;
    Width = mAtGop->Mode->Info->HorizontalResolution;
    Height = mAtGop->Mode->Info->VerticalResolution;
    AtGfxFill (0, Height - CANOE_UI_FOOTER_HEIGHT, Width,
               CANOE_UI_FOOTER_HEIGHT, &mAtSurface);
    AtGfxFill (0, Height - CANOE_UI_FOOTER_HEIGHT - 4,
               Width, 4, &mAtPrimary);
    AtGfxText (CANOE_UI_SIDE_MARGIN, Height - 76,
               AtGfxFitText (CANOE_UI_FOOTER_FONT, 22,
                             Width - 2 * CANOE_UI_SIDE_MARGIN, Footer),
               Footer, &mAtMuted);
    return;
  }
  if (Footer != NULL) Print (L"\r\n%s\r\n", Footer);
}

VOID
AtUiDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker,
             IN CONST CHAR16 *Text)
{
  AtUiDrawRowIcon (Selected, AtIconForText (Text), Marker, Text);
}

VOID
AtUiSetVisibleRows (IN UINTN Rows)
{
  mAtVisibleRows = MIN (CANOE_UI_VISIBLE_MAX,
                        MAX (CANOE_UI_VISIBLE_MIN, Rows));
}

VOID
AtUiDrawRowIcon (IN BOOLEAN Selected, IN CANOE_UI_ICON Icon,
                 IN CONST CHAR16 *FallbackMarker, IN CONST CHAR16 *Text)
{
  UINTN  Width;
  Text = AtUiLocalize (Text);
  if (mAtGraphical) {
    UINTN CardWidth;
    UINTN IconSize;
    UINTN IconY;
    UINTN TextX;
    UINTN TextWidth;
    UINTN TextY;
    UINT16 TextSize;
    Width = mAtGop->Mode->Info->HorizontalResolution;
    CardWidth = Width - 2 * CANOE_UI_SIDE_MARGIN;
    IconSize = MIN (CANOE_UI_ICON_BOX,
                    (mAtRowHeight > 18) ? mAtRowHeight - 18 : 12);
    IconY = mAtY + (mAtRowHeight - IconSize) / 2;
    TextX = CANOE_UI_SIDE_MARGIN + 34 + CANOE_UI_ICON_BOX + CANOE_UI_TEXT_GAP;
    TextWidth = Width - CANOE_UI_SIDE_MARGIN - TextX - 28;
    TextSize = AtGfxFitText (CANOE_UI_BODY_FONT, CANOE_UI_BODY_FONT_MIN,
                             TextWidth, Text);
    TextSize = (UINT16)MIN ((UINTN)TextSize,
                            MAX ((UINTN)20, mAtRowHeight - 16));
    TextY = mAtY + (mAtRowHeight - TextSize) / 2;
    AtGfxFill (CANOE_UI_SIDE_MARGIN, mAtY, CardWidth, mAtRowHeight,
               Selected ? &mAtPrimary : &mAtSurface);
    if (!Selected) {
      AtGfxFill (CANOE_UI_SIDE_MARGIN, mAtY, 4,
                 mAtRowHeight, &mAtDisabled);
    }
    AtGfxIcon (CANOE_UI_SIDE_MARGIN + 28, IconY, IconSize, Icon,
               Selected ? &mAtBackground : &mAtMuted);
    AtGfxText (TextX, TextY, TextSize, Text,
               Selected ? &mAtBackground : &mAtText);
    mAtY += mAtRowStep;
    return;
  }
  gST->ConOut->SetAttribute (gST->ConOut,
                             Selected ? AT_ATTR_SELECTED : AT_ATTR_NORMAL);
  Print (L"%s %s %s\r\n", Selected ? L">" : L" ",
         FallbackMarker != NULL ? FallbackMarker : L" ", Text);
  gST->ConOut->SetAttribute (gST->ConOut, AT_ATTR_NORMAL);
}

VOID
AtUiWriteLine (IN CONST CHAR16 *Text)
{
  Text = AtUiLocalize (Text);
  if (mAtGraphical) {
    UINTN Width = mAtGop->Mode->Info->HorizontalResolution;
    UINT16 Size = AtGfxFitText (CANOE_UI_SUBTITLE_FONT, 22,
                                Width - 2 * CANOE_UI_SIDE_MARGIN - 48, Text);
    AtGfxFill (CANOE_UI_SIDE_MARGIN, mAtY,
               Width - 2 * CANOE_UI_SIDE_MARGIN, 82, &mAtSurface);
    AtGfxFill (CANOE_UI_SIDE_MARGIN, mAtY, 4, 82, &mAtPrimary);
    AtGfxText (CANOE_UI_SIDE_MARGIN + 28, mAtY + (82 - Size) / 2,
               Size, Text, &mAtText);
    mAtY += 96;
  } else {
    Print (L"%s\r\n", Text);
  }
}

VOID
AtUiDrawFocusScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Value,
                     IN CONST CHAR16 *Detail, IN UINTN State)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *ValueColor;
  UINTN Width;
  UINTN CardWidth;
  UINTN CardHeight;
  UINTN ValueWidth;
  UINTN DetailWidth;
  UINT16 ValueSize;
  UINT16 DetailSize;

  AtUiSetVisibleRows (CANOE_UI_VISIBLE_MIN);
  AtUiBeginScreen (Title, NULL);
  if (!mAtGraphical) {
    AtUiWriteLine (Value);
    if (Detail != NULL) AtUiWriteLine (Detail);
    return;
  }
  Width = mAtGop->Mode->Info->HorizontalResolution;
  CardWidth = Width - 2 * CANOE_UI_SIDE_MARGIN;
  CardHeight = (mAtGop->Mode->Info->VerticalResolution >
                mAtY + CANOE_UI_FOOTER_HEIGHT + 40)
                 ? mAtGop->Mode->Info->VerticalResolution - mAtY -
                   CANOE_UI_FOOTER_HEIGHT - 40 : 180;
  CardHeight = MIN ((UINTN)390, MAX ((UINTN)180, CardHeight));
  Color = (State == 1) ? &mAtSuccess :
          (State == 2) ? &mAtWarning :
          (State == 3) ? &mAtPrimary : &mAtSurface;
  ValueColor = (State == 0) ? &mAtText : &mAtBackground;
  AtGfxFill (CANOE_UI_SIDE_MARGIN, mAtY, CardWidth, CardHeight, Color);
  ValueSize = AtGfxFitText (112, 52, CardWidth - 80, AtUiLocalize (Value));
  ValueWidth = AtGfxMeasureText (ValueSize, AtUiLocalize (Value));
  AtGfxText (CANOE_UI_SIDE_MARGIN + (CardWidth - ValueWidth) / 2,
             mAtY + CardHeight / 2 - ValueSize / 2 - 28,
             ValueSize, AtUiLocalize (Value), ValueColor);
  if (Detail != NULL) {
    Detail = AtUiLocalize (Detail);
    DetailSize = AtGfxFitText (CANOE_UI_SUBTITLE_FONT, 22,
                               CardWidth - 80, Detail);
    DetailWidth = AtGfxMeasureText (DetailSize, Detail);
    AtGfxText (CANOE_UI_SIDE_MARGIN + (CardWidth - DetailWidth) / 2,
               mAtY + CardHeight - DetailSize - 46,
               DetailSize, Detail, ValueColor);
  }
  mAtY += CardHeight + CANOE_UI_CARD_GAP;
}

BOOLEAN
AtUiConfirmDanger (IN CONST CHAR16 *Title, IN CONST CHAR16 *Warning)
{
  STATIC CONST CHAR16 *Choices[] = { L"Cancel", L"Confirm action" };
  UINTN Selected = 0;

  AtUiBeginScreen (Title, L"Review warning");
  AtUiWriteLine (Warning != NULL ? Warning : L"");
  AtUiEndScreen (L"Press power to review");
  if (AtUiWaitForKey (0) != AtKeySelect) {
    AtUiDebounce ();
    return FALSE;
  }
  AtUiDebounce ();
  if (EFI_ERROR (AtUiRunMenu (Title, Choices, ARRAY_SIZE (Choices),
                              &Selected, L"Vol+/- move, power select"))) {
    return FALSE;
  }
  return (BOOLEAN)(Selected == 1);
}

UINTN
AtUiWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
{
  if (Count <= Rows) return 0;
  if (Cursor < Rows / 2) return 0;
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) return Count - Rows;
  return Cursor - Rows / 2;
}

VOID
AtUiMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN AT_KEY Key)
{
  if (Count == 0) { *Cursor = 0; return; }
  if (Key == AtKeyUp) *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  else if (Key == AtKeyDown) *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
}

VOID
AtUiShowMessage (IN CONST CHAR16 *Text)
{
  AtUiBeginScreen (L"Notice", NULL);
  AtUiWriteLine (Text);
  AtUiEndScreen (L"Press power to continue");
}

VOID
AtUiReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  CHAR16  Line[128];
  UnicodeSPrint (Line, sizeof (Line), L"%s: %r", AtUiLocalize (What), Status);
  AtUiBeginScreen (L"Error", NULL);
  AtUiWriteLine (Line);
  AtUiEndScreen (L"Press power to continue");
  AtUiWaitForKey (0);
  AtUiDebounce ();
}

EFI_STATUS
AtUiRunMenu (IN CONST CHAR16 *Title, IN CONST CHAR16 **Items, IN UINTN Count,
             OUT UINTN *Selected, IN CONST CHAR16 *Footer)
{
  UINTN   Cursor = 0;
  UINTN   Start;
  UINTN   Index;
  UINTN   Visible;
  AT_KEY  Key;

  if (Items == NULL || Selected == NULL || Count == 0) return EFI_INVALID_PARAMETER;
  gST->ConIn->Reset (gST->ConIn, FALSE);
  Visible = MIN ((UINTN)CANOE_UI_VISIBLE_MAX, Count);
  AtUiSetVisibleRows (Visible);
  while (TRUE) {
    AtUiBeginScreen (Title, NULL);
    Start = AtUiWindowStart (Cursor, Count, Visible);
    for (Index = Start; Index < Start + Visible && Index < Count; Index++) {
      AtUiDrawRowIcon ((BOOLEAN)(Index == Cursor), AtIconForText (Items[Index]),
                       L" ", Items[Index]);
    }
    AtUiEndScreen (Footer);
    Key = AtUiWaitForKey (0);
    if (Key == AtKeySelect) {
      AtUiDebounce ();
      *Selected = Cursor;
      return EFI_SUCCESS;
    }
    if (Key == AtKeyUp || Key == AtKeyDown) AtUiMoveCursor (&Cursor, Count, Key);
  }
}
