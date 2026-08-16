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

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
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

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorBackground = { 0x18, 0x12, 0x0d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorSurface    = { 0x2d, 0x25, 0x1d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorPrimary    = { 0xe8, 0x79, 0x24, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorText       = { 0xf4, 0xf4, 0xf4, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorMuted      = { 0xb0, 0xa8, 0x9f, 0x00 };

STATIC
CONST CHAR16 *
SfbUiChinese (IN CONST CHAR16 *Text)
{
  if (Text == NULL)                         return L"";
  if (StrCmp (Text, L"Boot Menu") == 0)     return L"启动菜单";
  if (StrCmp (Text, L"Android Tools") == 0) return L"安卓工具";
  if (StrCmp (Text, L"Android") == 0)       return L"启动安卓";
  if (StrCmp (Text, L"Enter Fastboot") == 0)return L"进入 Fastboot";
  if (StrCmp (Text, L"Enter EFI Program Selector") == 0) return L"选择 EFI 程序";
  if (StrCmp (Text, L"EFI Program Selector") == 0) return L"EFI 程序选择器";
  if (StrCmp (Text, L"Power Off") == 0)     return L"关机";
  if (StrCmp (Text, L"Restart") == 0)       return L"重新启动";
  if (StrCmp (Text, L"Back") == 0)          return L"返回";
  if (StrCmp (Text, L"Action failed") == 0) return L"操作失败";
  if (StrCmp (Text, L"Action complete") == 0) return L"操作完成";
  if (StrCmp (Text, L"Launching") == 0)     return L"正在启动";
  if (StrCmp (Text, L"Power") == 0)         return L"电源选项";
  if (StrCmp (Text, L"Boot control") == 0)  return L"启动控制";
  if (StrCmp (Text, L"Fastboot") == 0)      return L"Fastboot 模式";
  if (StrCmp (Text, L"Reboot Tools") == 0)  return L"重启工具";
  if (StrCmp (Text, L"BL Tools") == 0)      return L"引导锁工具";
  if (StrCmp (Text, L"ARB Tools") == 0)     return L"防回滚工具";
  return Text;
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

/* SimpleTextIn reports repeats but does not reliably expose a key-up event on
 * these handsets. After Power confirms an action, wait until the input stream
 * has stayed quiet for a complete debounce window. This prevents one slightly
 * long press from confirming a second item on the next screen. */
STATIC
VOID
SfbWaitForSelectRelease (VOID)
{
  EFI_EVENT      TimerEvent;
  EFI_EVENT      WaitList[2];
  EFI_INPUT_KEY  Key;
  EFI_STATUS     Status;
  UINTN          EventIndex;

  gBS->Stall (200 * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
  if (EFI_ERROR (Status)) {
    return;
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;
  while (TRUE) {
    gBS->SetTimer (TimerEvent, TimerRelative, 220 * 10000);
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

  if (mSfbGraphical) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color;

    Color = (Attribute == SFB_ATTR_ERROR) ? &mSfbColorPrimary :
            (Attribute == SFB_ATTR_MUTED) ? &mSfbColorMuted : &mSfbColorText;
    SfbGfxText (72, mSfbGfxY, 30, SfbUiChinese (Text), Color);
    mSfbGfxY += 58;
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
  UINTN   Width;

  SfbUiInitGraphics ();
  if (mSfbGraphical) {
    Width = mSfbGop->Mode->Info->HorizontalResolution;
    mSfbSafeTop = MAX (192,
                       mSfbGop->Mode->Info->VerticalResolution / 16);
    SfbGfxFill (0, 0, Width,
                mSfbGop->Mode->Info->VerticalResolution,
                &mSfbColorBackground);
    SfbGfxFill (0, mSfbSafeTop, Width, 168, &mSfbColorSurface);
    SfbGfxFill (0, mSfbSafeTop + 164, Width, 4, &mSfbColorPrimary);
    UnicodeSPrint (Header, sizeof (Header), L"CANOE  /  %s",
                   SfbUiChinese (Title));
    SfbGfxText (72, mSfbSafeTop + 36, 60, Header, &mSfbColorText);
    if (Subtitle != NULL) {
      SfbGfxText (72, mSfbSafeTop + 184, 32,
                  SfbUiChinese (Subtitle), &mSfbColorMuted);
      mSfbGfxY = mSfbSafeTop + 244;
    } else {
      mSfbGfxY = mSfbSafeTop + 204;
    }
    return;
  }

  SfbUiRefreshGeometry ();
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  UnicodeSPrint (Header, sizeof (Header), L"  CANOE  /  %s", Title);
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

    SfbGfxFill (0, Height - 112, Width, 112, &mSfbColorSurface);
    SfbGfxFill (0, Height - 116, Width, 4, &mSfbColorPrimary);
    SfbGfxText (72, Height - 84, 36,
                L"音量 +/-：移动      电源键：确认",
                &mSfbColorMuted);
    return;
  }

  SfbUiFullRow (SFB_ATTR_NORMAL, L"");
  SfbUiRule ();
  UnicodeSPrint (Hint, sizeof (Hint), L"  [VOL +/-] Navigate    [POWER] %s",
                 Footer != NULL ? Footer : L"Select");
  SfbUiFullRow (SFB_ATTR_MUTED, Hint);
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  CHAR16  Row[SFB_UI_LINE_CHARS];

  if (mSfbGraphical) {
    UINTN  Width = mSfbGop->Mode->Info->HorizontalResolution;
    UINTN  CardWidth = Width - 144;

    SfbGfxFill (72, mSfbGfxY, CardWidth, 104,
                Selected ? &mSfbColorPrimary : &mSfbColorSurface);
    SfbGfxText (98, mSfbGfxY + 34, 30, Marker,
                Selected ? &mSfbColorText : &mSfbColorMuted);
    SfbGfxText (220, mSfbGfxY + 26, 48, SfbUiChinese (Text),
                &mSfbColorText);
    mSfbGfxY += 120;
    return;
  }

  UnicodeSPrint (Row, sizeof (Row), L"  %s  %-6s  %s",
                 Selected ? L">" : L" ", Marker, Text);
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
  UnicodeSPrint (Detail, sizeof (Detail), L"  Status  %r", Status);
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
  SfbBeginScreen (L"Fastboot", L"USB service is ready");
  SfbUiFullRow (SFB_ATTR_SUCCESS, L"  ONLINE");
  SfbUiFullRow (SFB_ATTR_NORMAL,
                L"  Connect a host and use fastboot to manage this device.");
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
  SfbUiFullRow (SFB_ATTR_ACCENT, L"  INITIALIZING ...");

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
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
                 L"%u 个启动项  /  * 表示默认项", (UINT32)Menu->Count);
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

    if (Index == Menu->DefaultIndex) {
      Marker = L"*";
    } else {
      switch (Entry->Kind) {
      case SfbEntryEfiFile:   Marker = L"EFI";   break;
      case SfbEntrySubmenu:   Marker = L"MENU";  break;
      case SfbEntryFastboot:  Marker = L"USB";   break;
      case SfbEntrySelector:  Marker = L"FILES"; break;
      case SfbEntryBack:      Marker = L"BACK";  break;
      case SfbEntryPowerOff:
      case SfbEntryRestart:   Marker = L"POWER"; break;
      default:                Marker = L"";      break;
      }
    }

    /* Submenu rows get a trailing '>' so it is obvious they open another list
     * rather than launch an image. */
    if (Entry->Kind == SfbEntrySubmenu) {
      CHAR16  Text[SFB_DESC_CHARS + 4];

      UnicodeSPrint (Text, sizeof (Text), L"%s >", Entry->Desc);
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Entry->Desc);
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
