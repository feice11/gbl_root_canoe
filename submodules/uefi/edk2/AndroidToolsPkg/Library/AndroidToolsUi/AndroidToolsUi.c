/** @file
 * Shared graphical UI for every AndroidToolsPkg application.
 *
 * BootMenu passes CUI1|<language>|<theme> through EFI LoadOptions.  This keeps
 * every tool visually and linguistically in sync without writing a second
 * settings store.  Volume up/down move, power confirms; menu confirmation is
 * protected by the same quiet-window debounce used by BootMenu.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
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

typedef struct {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Background;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Surface;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Primary;
} AT_PALETTE;

STATIC CONST AT_PALETTE  mAtPalettes[AT_THEME_COUNT] = {
  { { 0x18, 0x12, 0x0d, 0 }, { 0x2d, 0x25, 0x1d, 0 }, { 0xe8, 0x79, 0x24, 0 } },
  { { 0x19, 0x10, 0x14, 0 }, { 0x32, 0x21, 0x2a, 0 }, { 0xff, 0x59, 0x9b, 0 } },
  { { 0x13, 0x16, 0x0d, 0 }, { 0x26, 0x2c, 0x1d, 0 }, { 0x7b, 0xc7, 0x27, 0 } },
  { { 0x10, 0x12, 0x18, 0 }, { 0x20, 0x26, 0x32, 0 }, { 0x3d, 0x8a, 0xff, 0 } }
};

STATIC BOOLEAN                       mAtChinese = TRUE;
STATIC UINTN                         mAtTheme = 0;
STATIC BOOLEAN                       mAtGraphical = FALSE;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mAtGop = NULL;
STATIC UINTN                         mAtSafeTop = 192;
STATIC UINTN                         mAtY = 0;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtBackground;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtSurface;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtPrimary;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtText = { 0xf4, 0xf4, 0xf4, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mAtMuted = { 0xb0, 0xa8, 0x9f, 0 };

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

VOID
AtUiInitialize (IN EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage = NULL;
  CONST CHAR16               *Options;
  EFI_STATUS                 Status;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle,
                                       &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&LoadedImage)) &&
      LoadedImage != NULL && LoadedImage->LoadOptions != NULL &&
      LoadedImage->LoadOptionsSize >= 9 * sizeof (CHAR16)) {
    Options = (CONST CHAR16 *)LoadedImage->LoadOptions;
    if (StrnCmp (Options, L"CUI1|", 5) == 0 &&
        (Options[5] == L'0' || Options[5] == L'1') &&
        Options[6] == L'|' && Options[7] >= L'0' &&
        Options[7] < L'0' + AT_THEME_COUNT) {
      mAtChinese = (BOOLEAN)(Options[5] == L'0');
      mAtTheme = Options[7] - L'0';
    }
  }
  mAtBackground = mAtPalettes[mAtTheme].Background;
  mAtSurface = mAtPalettes[mAtTheme].Surface;
  mAtPrimary = mAtPalettes[mAtTheme].Primary;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&mAtGop);
  mAtGraphical = (BOOLEAN)(!EFI_ERROR (Status) && mAtGop != NULL &&
                            mAtGop->Mode != NULL && mAtGop->Mode->Info != NULL);
  if (mAtGraphical) {
    mAtSafeTop = MAX (192, mAtGop->Mode->Info->VerticalResolution / 16);
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
  Title = AtUiLocalize (Title);
  Subtitle = (Subtitle != NULL) ? AtUiLocalize (Subtitle) : NULL;
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  if (mAtGraphical) {
    Width = mAtGop->Mode->Info->HorizontalResolution;
    AtGfxFill (0, 0, Width, mAtGop->Mode->Info->VerticalResolution,
               &mAtBackground);
    AtGfxFill (0, mAtSafeTop - 4, Width, 4, &mAtPrimary);
    AtGfxText (72, mAtSafeTop + 30, 44, Title, &mAtText);
    if (Subtitle != NULL) AtGfxText (72, mAtSafeTop + 92, 26, Subtitle, &mAtMuted);
    AtGfxFill (72, mAtSafeTop + 142, Width - 144, 2, &mAtPrimary);
    mAtY = mAtSafeTop + 174;
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
    AtGfxFill (0, Height - 112, Width, 112, &mAtSurface);
    AtGfxFill (0, Height - 112, Width, 3, &mAtPrimary);
    AtGfxText (72, Height - 75, 26, Footer, &mAtMuted);
    return;
  }
  if (Footer != NULL) Print (L"\r\n%s\r\n", Footer);
}

VOID
AtUiDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker,
             IN CONST CHAR16 *Text)
{
  UINTN  Width;
  Text = AtUiLocalize (Text);
  if (mAtGraphical) {
    Width = mAtGop->Mode->Info->HorizontalResolution;
    AtGfxFill (72, mAtY, Width - 144, 104,
               Selected ? &mAtPrimary : &mAtSurface);
    if (Marker != NULL && Marker[0] != L'\0')
      AtGfxText (96, mAtY + 38, 20, Marker, Selected ? &mAtBackground : &mAtMuted);
    AtGfxText (220, mAtY + 30, 34, Text,
               Selected ? &mAtBackground : &mAtText);
    mAtY += 116;
    return;
  }
  gST->ConOut->SetAttribute (gST->ConOut,
                             Selected ? AT_ATTR_SELECTED : AT_ATTR_NORMAL);
  Print (L"%s %s %s\r\n", Selected ? L">" : L" ",
         Marker != NULL ? Marker : L" ", Text);
  gST->ConOut->SetAttribute (gST->ConOut, AT_ATTR_NORMAL);
}

VOID
AtUiWriteLine (IN CONST CHAR16 *Text)
{
  Text = AtUiLocalize (Text);
  if (mAtGraphical) {
    AtGfxText (72, mAtY, 30, Text, &mAtText);
    mAtY += 58;
  } else {
    Print (L"%s\r\n", Text);
  }
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
  Visible = (Count < AT_VISIBLE_ROWS) ? Count : AT_VISIBLE_ROWS;
  while (TRUE) {
    AtUiBeginScreen (Title, NULL);
    Start = AtUiWindowStart (Cursor, Count, Visible);
    for (Index = Start; Index < Start + Visible && Index < Count; Index++) {
      AtUiDrawRow ((BOOLEAN)(Index == Cursor), L" ", Items[Index]);
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
