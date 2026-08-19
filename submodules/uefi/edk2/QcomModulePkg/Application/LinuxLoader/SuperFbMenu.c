/*
 * Console UI for the super-fastboot boot menu.
 *
 * Volume up/down move the cursor and power confirms. Graphical firmware may
 * additionally expose touch through Absolute Pointer; vertical swipes map to
 * the same cursor actions so every screen keeps one input contract.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"
#include "SuperFbFont.h"
#include "SuperFbImage.h"
#include "CanoeUiStyle.h"
#include "CanoeIcons.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/EFIChargerEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/CanoeUi.h>
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
STATIC EFI_HANDLE                    mSfbUiProtocolHandle = NULL;
STATIC UINTN                         mSfbGfxY = 0;
STATIC UINTN                         mSfbSafeTop = 0;
STATIC UINTN                         mSfbRowHeight = 104;
STATIC UINTN                         mSfbRowStep = 120;
STATIC UINTN                         mSfbVisibleRows = CANOE_UI_VISIBLE_MIN;
STATIC BOOLEAN                       mSfbClockCalibrationLoaded = FALSE;
STATIC UINTN                         mSfbClockOffsetSeconds = 0;
STATIC BOOLEAN                       mSfbSelectionAnimated = FALSE;
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL *mSfbTouch = NULL;
STATIC BOOLEAN                       mSfbTouchTracking = FALSE;
STATIC BOOLEAN                       mSfbTouchGestureConsumed = FALSE;
STATIC UINT64                        mSfbTouchStartY = 0;

#define SFB_SELECTION_FRAMES    9
#define SFB_SELECTION_FRAME_US  15000
#define SFB_TOUCH_SWIPE_DIVISOR 16

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorBackground = { 0x18, 0x12, 0x0d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorSurface    = { 0x2d, 0x25, 0x1d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorPrimary    = { 0xe8, 0x79, 0x24, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorText       = { 0xf4, 0xf4, 0xf4, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorMuted      = { 0xb0, 0xa8, 0x9f, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorDisabled   = { 0x68, 0x62, 0x5d, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorSuccess    = { 0x78, 0xd6, 0x55, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorWarning    = { 0x42, 0xa5, 0xff, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSfbColorShadow     = { 0x08, 0x07, 0x06, 0x00 };

#define SFB_LOCK_OFF     0
#define SFB_LOCK_SIMPLE  1
#define SFB_LOCK_PIN     2

STATIC CONST CANOE_UI_BASE  mSfbBases[CANOE_UI_BASE_COUNT] =
  CANOE_UI_BASE_INITIALIZERS;
STATIC CONST CANOE_UI_ACCENT  mSfbAccents[CANOE_UI_ACCENT_COUNT] =
  CANOE_UI_ACCENT_INITIALIZERS;

STATIC UINTN    mSfbBase = 0;
STATIC UINTN    mSfbAccent = 0;
STATIC UINTN    mSfbLockMode = SFB_LOCK_OFF;
STATIC UINTN    mSfbLanguage = 0; /* 0 = Chinese, 1 = English */
STATIC BOOLEAN  mSfbDescriptions = TRUE;
/* 0 = hidden, 1 = minimal card, 2 = custom PNG/GIF, 3 = ASCII art. */
STATIC UINTN    mSfbBootVisual = 1;
/* 0 = top, 1 = center, 2 = bottom. */
STATIC UINTN    mSfbBootPosition = 1;
/* 0=off, 1..6 preset entrances, 7=random. */
STATIC UINTN    mSfbArtAnimation = 7;
STATIC CHAR8    mSfbBootAssetLabel[192] = "";
STATIC CHAR8    mSfbBootAssetPath[768] = "";
STATIC CHAR8    mSfbPin[5] = "1234";
STATIC BOOLEAN  mSfbSettingsLoaded = FALSE;

STATIC
VOID
SfbApplyPalette (VOID)
{
  mSfbColorBackground = mSfbBases[mSfbBase].Background;
  mSfbColorSurface = mSfbBases[mSfbBase].Surface;
  mSfbColorText = mSfbBases[mSfbBase].Text;
  mSfbColorMuted = mSfbBases[mSfbBase].Muted;
  mSfbColorDisabled = mSfbBases[mSfbBase].Disabled;
  mSfbColorShadow = mSfbBases[mSfbBase].Shadow;
  mSfbColorPrimary = mSfbAccents[mSfbAccent].Primary;
  mSfbColorSuccess = mSfbAccents[mSfbAccent].Success;
  mSfbColorWarning = mSfbAccents[mSfbAccent].Warning;
  if (mSfbAccent == 0 && mSfbBase == 1) {
    mSfbColorPrimary.Blue = 0x24;
    mSfbColorPrimary.Green = 0x21;
    mSfbColorPrimary.Red = 0x20;
    mSfbColorSuccess.Blue = mSfbColorSuccess.Green = mSfbColorSuccess.Red = 0x44;
    mSfbColorWarning.Blue = mSfbColorWarning.Green = mSfbColorWarning.Red = 0x68;
  }
}

STATIC
VOID
SfbImportLegacyTheme (IN UINTN Theme)
{
  STATIC CONST UINT8  AccentMap[4] = { 0, 7, 6, 4 };

  mSfbBase = 0;
  mSfbAccent = AccentMap[MIN (Theme, ARRAY_SIZE (AccentMap) - 1)];
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
      AsciiStrnCmp (Record, "SFC7|", 5) == 0 &&
      Record[5] >= '0' && Record[5] < '0' + CANOE_UI_BASE_COUNT &&
      Record[6] == '|' &&
      Record[7] >= '0' && Record[7] < '0' + CANOE_UI_ACCENT_COUNT &&
      Record[8] == '|' && Record[9] >= '0' && Record[9] <= '2' &&
      Record[10] == '|' && (Record[11] == '0' || Record[11] == '1') &&
      Record[12] == '|' && (Record[13] == '0' || Record[13] == '1') &&
      Record[14] == '|' && Record[15] >= '0' && Record[15] <= '3' &&
      Record[16] == '|' && Record[17] >= '0' && Record[17] <= '2' &&
      Record[18] == '|' && Record[19] >= '0' && Record[19] <= '7' &&
      Record[20] == '|') {
    CONST CHAR8 *Cursor;
    UINTN Out;
    mSfbBase = Record[5] - '0';
    mSfbAccent = Record[7] - '0';
    mSfbLockMode = Record[9] - '0';
    mSfbLanguage = Record[11] - '0';
    mSfbDescriptions = (BOOLEAN)(Record[13] == '1');
    mSfbBootVisual = Record[15] - '0';
    mSfbBootPosition = Record[17] - '0';
    mSfbArtAnimation = Record[19] - '0';
    Cursor = Record + 21;
    for (Index = 0; Index < 4; Index++) {
      if (Cursor[Index] < '0' || Cursor[Index] > '9') {
        mSfbLockMode = SFB_LOCK_OFF;
        break;
      }
      mSfbPin[Index] = Cursor[Index];
    }
    mSfbPin[4] = '\0';
    Cursor += 4;
    if (*Cursor == '|') Cursor++;
    Out = 0;
    while (*Cursor != '\0' && *Cursor != '|' &&
           Out + 1 < sizeof (mSfbBootAssetLabel)) {
      mSfbBootAssetLabel[Out++] = *Cursor++;
    }
    mSfbBootAssetLabel[Out] = '\0';
    if (*Cursor == '|') Cursor++;
    AsciiStrnCpyS (mSfbBootAssetPath, sizeof (mSfbBootAssetPath), Cursor,
                   sizeof (mSfbBootAssetPath) - 1);
  } else if (AsciiStrnCmp (Record, "SFC6|", 5) == 0 &&
      Record[5] >= '0' && Record[5] < '0' + CANOE_UI_BASE_COUNT &&
      Record[6] == '|' && Record[7] >= '0' &&
      Record[7] < '0' + CANOE_UI_ACCENT_COUNT && Record[8] == '|' &&
      Record[9] >= '0' && Record[9] <= '2' && Record[10] == '|' &&
      (Record[11] == '0' || Record[11] == '1') && Record[12] == '|' &&
      (Record[13] == '0' || Record[13] == '1') && Record[14] == '|' &&
      Record[15] >= '0' && Record[15] <= '2' && Record[16] == '|' &&
      Record[17] >= '0' && Record[17] <= '2' && Record[18] == '|' &&
      Record[19] >= '0' && Record[19] <= '7' && Record[20] == '|') {
    CONST CHAR8 *Cursor;
    UINTN Out;
    mSfbBase=Record[5]-'0';mSfbAccent=Record[7]-'0';mSfbLockMode=Record[9]-'0';
    mSfbLanguage=Record[11]-'0';mSfbDescriptions=(BOOLEAN)(Record[13]=='1');
    mSfbBootVisual=Record[15]-'0';mSfbBootPosition=Record[17]-'0';
    mSfbArtAnimation=Record[19]-'0';
    if (mSfbArtAnimation != 0) mSfbBootVisual = 3;
    Cursor=Record+21;
    for(Index=0;Index<4;Index++){if(Cursor[Index]<'0'||Cursor[Index]>'9'){mSfbLockMode=SFB_LOCK_OFF;break;}mSfbPin[Index]=Cursor[Index];}
    mSfbPin[4]='\0';Cursor+=4;if(*Cursor=='|')Cursor++;Out=0;
    while(*Cursor!='\0'&&*Cursor!='|'&&Out+1<sizeof(mSfbBootAssetLabel))mSfbBootAssetLabel[Out++]=*Cursor++;
    mSfbBootAssetLabel[Out]='\0';if(*Cursor=='|')Cursor++;
    AsciiStrnCpyS(mSfbBootAssetPath,sizeof(mSfbBootAssetPath),Cursor,sizeof(mSfbBootAssetPath)-1);
  } else if (AsciiStrnCmp (Record, "SFC5|", 5) == 0 &&
      Record[5] >= '0' && Record[5] < '0' + CANOE_UI_BASE_COUNT &&
      Record[6] == '|' && Record[7] >= '0' &&
      Record[7] < '0' + CANOE_UI_ACCENT_COUNT && Record[8] == '|' &&
      Record[9] >= '0' && Record[9] <= '2' && Record[10] == '|' &&
      (Record[11] == '0' || Record[11] == '1') && Record[12] == '|' &&
      (Record[13] == '0' || Record[13] == '1') && Record[14] == '|' &&
      Record[15] >= '0' && Record[15] <= '2' && Record[16] == '|' &&
      Record[17] >= '0' && Record[17] <= '2' && Record[18] == '|') {
    CONST CHAR8 *Cursor;
    UINTN Out;
    mSfbBase=Record[5]-'0';mSfbAccent=Record[7]-'0';mSfbLockMode=Record[9]-'0';
    mSfbLanguage=Record[11]-'0';mSfbDescriptions=(BOOLEAN)(Record[13]=='1');
    mSfbBootVisual=Record[15]-'0';mSfbBootPosition=Record[17]-'0';Cursor=Record+19;
    if (mSfbArtAnimation != 0) mSfbBootVisual = 3;
    for(Index=0;Index<4;Index++){if(Cursor[Index]<'0'||Cursor[Index]>'9'){mSfbLockMode=SFB_LOCK_OFF;break;}mSfbPin[Index]=Cursor[Index];}
    mSfbPin[4]='\0';Cursor+=4;if(*Cursor=='|')Cursor++;Out=0;
    while(*Cursor!='\0'&&*Cursor!='|'&&Out+1<sizeof(mSfbBootAssetLabel))mSfbBootAssetLabel[Out++]=*Cursor++;
    mSfbBootAssetLabel[Out]='\0';if(*Cursor=='|')Cursor++;
    AsciiStrnCpyS(mSfbBootAssetPath,sizeof(mSfbBootAssetPath),Cursor,sizeof(mSfbBootAssetPath)-1);
  } else if (AsciiStrnCmp (Record, "SFC4|", 5) == 0 &&
      Record[5] >= '0' && Record[5] < '0' + 4 &&
      Record[6] == '|' && Record[7] >= '0' && Record[7] <= '2' &&
      Record[8] == '|' && (Record[9] == '0' || Record[9] == '1') &&
      Record[10] == '|' && (Record[11] == '0' || Record[11] == '1') &&
      Record[12] == '|' && Record[13] >= '0' && Record[13] <= '2' &&
      Record[14] == '|' && Record[15] >= '0' && Record[15] <= '2' &&
      Record[16] == '|') {
    CONST CHAR8 *Cursor;
    UINTN Out;
    SfbImportLegacyTheme (Record[5] - '0');
    mSfbLockMode = Record[7] - '0';
    mSfbLanguage = Record[9] - '0';
    mSfbDescriptions = (BOOLEAN)(Record[11] == '1');
    mSfbBootVisual = Record[13] - '0';
    mSfbBootPosition = Record[15] - '0';
    Cursor = Record + 17;
    for (Index = 0; Index < 4; Index++) {
      if (Cursor[Index] < '0' || Cursor[Index] > '9') {
        mSfbLockMode = SFB_LOCK_OFF;
        break;
      }
      mSfbPin[Index] = Cursor[Index];
    }
    mSfbPin[4] = '\0';
    Cursor += 4;
    if (*Cursor == '|') Cursor++;
    Out = 0;
    while (*Cursor != '\0' && *Cursor != '|' &&
           Out + 1 < sizeof (mSfbBootAssetLabel)) {
      mSfbBootAssetLabel[Out++] = *Cursor++;
    }
    mSfbBootAssetLabel[Out] = '\0';
    if (*Cursor == '|') Cursor++;
    AsciiStrnCpyS (mSfbBootAssetPath, sizeof (mSfbBootAssetPath), Cursor,
                   sizeof (mSfbBootAssetPath) - 1);
  } else if (AsciiStrnCmp (Record, "SFC3|", 5) == 0 &&
      Record[5] >= '0' && Record[5] < '0' + 4 &&
      Record[6] == '|' &&
      Record[7] >= '0' && Record[7] <= '2' &&
      Record[8] == '|' &&
      (Record[9] == '0' || Record[9] == '1') &&
      Record[10] == '|' &&
      (Record[11] == '0' || Record[11] == '1') &&
      Record[12] == '|') {
    SfbImportLegacyTheme (Record[5] - '0');
    mSfbLockMode = Record[7] - '0';
    mSfbLanguage = Record[9] - '0';
    mSfbDescriptions = (BOOLEAN)(Record[11] == '1');
    for (Index = 0; Index < 4; Index++) {
      if (Record[13 + Index] < '0' || Record[13 + Index] > '9') {
        mSfbLockMode = SFB_LOCK_OFF;
        break;
      }
      mSfbPin[Index] = Record[13 + Index];
    }
    mSfbPin[4] = '\0';
  } else if (AsciiStrnCmp (Record, "SFC2|", 5) == 0 &&
             Record[5] >= '0' && Record[5] < '0' + 4 &&
             Record[6] == '|' && Record[7] >= '0' && Record[7] <= '2' &&
             Record[8] == '|' && (Record[9] == '0' || Record[9] == '1') &&
             Record[10] == '|' && Record[12] == '|') {
    SfbImportLegacyTheme (Record[5] - '0');
    mSfbLockMode = Record[7] - '0';
    mSfbLanguage = Record[9] - '0';
    for (Index = 0; Index < 4; Index++) {
      if (Record[13 + Index] < '0' || Record[13 + Index] > '9') {
        mSfbLockMode = SFB_LOCK_OFF;
        break;
      }
      mSfbPin[Index] = Record[13 + Index];
    }
    mSfbPin[4] = '\0';
  } else if (AsciiStrnCmp (Record, "SFC1|", 5) == 0 &&
             Record[5] >= '0' && Record[5] < '0' + 4 &&
             Record[6] == '|' &&
             Record[7] >= '0' && Record[7] <= '2' &&
             Record[8] == '|') {
    /* Backward-compatible import of the first settings format. */
    SfbImportLegacyTheme (Record[5] - '0');
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
  CHAR8  Record[SFB_STORE_SLOT_BYTES];

  AsciiSPrint (Record, sizeof (Record), "SFC7|%u|%u|%u|%u|%u|%u|%u|%u|%a|%a|%a",
               (UINT32)mSfbBase, (UINT32)mSfbAccent, (UINT32)mSfbLockMode,
               (UINT32)mSfbLanguage, mSfbDescriptions ? 1U : 0U,
               (UINT32)mSfbBootVisual, (UINT32)mSfbBootPosition,
               (UINT32)mSfbArtAnimation,
               mSfbPin, mSfbBootAssetLabel, mSfbBootAssetPath);
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
  return mSfbBase;
}

UINTN
SfbUiAccent (VOID)
{
  SfbLoadSettings ();
  return mSfbAccent;
}

BOOLEAN
SfbUiDescriptionsEnabled (VOID)
{
  SfbLoadSettings ();
  return mSfbDescriptions;
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
EFI_GRAPHICS_OUTPUT_BLT_PIXEL
SfbBlendColor (IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *From,
               IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *To,
               IN UINTN Amount)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Result;
  UINTN Inverse;

  if (Amount > 256) Amount = 256;
  Inverse = 256 - Amount;
  Result.Blue = (UINT8)((From->Blue * Inverse + To->Blue * Amount + 128) / 256);
  Result.Green = (UINT8)((From->Green * Inverse + To->Green * Amount + 128) / 256);
  Result.Red = (UINT8)((From->Red * Inverse + To->Red * Amount + 128) / 256);
  Result.Reserved = 0;
  return Result;
}

STATIC
CONST SFB_FONT_GLYPH *
SfbFindGlyph (IN CHAR16 Codepoint)
{
  UINTN Index;

  /* SuperFbFont.h follows the source-string generation order, not Unicode
   * codepoint order.  A binary search therefore works for the leading ASCII
   * run but drops most Chinese glyphs.  The table is intentionally small, so
   * a reliable linear lookup is cheaper than carrying another index in BDS. */
  for (Index = 0; Index < ARRAY_SIZE (mSfbFontGlyphs); Index++) {
    if (mSfbFontGlyphs[Index].Codepoint == Codepoint) {
      return &mSfbFontGlyphs[Index];
    }
  }
  return NULL;
}

STATIC
UINTN
SfbGfxMeasureText (IN UINT16 Size, IN CONST CHAR16 *Text)
{
  CONST SFB_FONT_GLYPH  *Glyph;
  UINTN                 Index;
  UINTN                 Width = 0;

  if (Text == NULL || Size == 0) return 0;
  for (Index = 0; Text[Index] != L'\0'; Index++) {
    Glyph = SfbFindGlyph (Text[Index]);
    if (Glyph == NULL) Glyph = SfbFindGlyph (L'?');
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
VOID __attribute__ ((unused))
SfbGfxGradientText (IN UINTN Y, IN UINT16 Preferred, IN UINT16 Minimum,
                    IN CONST CHAR16 *Text)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Mid = { 0xdf, 0x68, 0xa8, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL End = { 0xff, 0xd0, 0x35, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;
  CHAR16 GlyphText[2];
  UINTN Width;
  UINTN TextWidth;
  UINTN Cursor;
  UINTN Length;
  UINTN Index;
  UINTN T;
  UINT16 Size;

  if (!mSfbGraphical || Text == NULL) return;
  Width = mSfbGop->Mode->Info->HorizontalResolution;
  Size = SfbGfxFitText (Preferred, Minimum,
                        Width - 2 * CANOE_UI_STATUS_INSET, Text);
  TextWidth = SfbGfxMeasureText (Size, Text);
  Cursor = (Width > TextWidth) ? (Width - TextWidth) / 2 : 0;
  Length = StrLen (Text);
  GlyphText[1] = L'\0';
  for (Index = 0; Index < Length; Index++) {
    T = (Length > 1) ? Index * 512 / (Length - 1) : 0;
#define SFB_LERP(_a, _b, _t) \
  ((UINT8)(((UINTN)(_a) * (256 - (_t)) + (UINTN)(_b) * (_t) + 128) / 256))
    if (T <= 256) {
      Color.Blue = SFB_LERP (mSfbColorPrimary.Blue, Mid.Blue, T);
      Color.Green = SFB_LERP (mSfbColorPrimary.Green, Mid.Green, T);
      Color.Red = SFB_LERP (mSfbColorPrimary.Red, Mid.Red, T);
    } else {
      T -= 256;
      Color.Blue = SFB_LERP (Mid.Blue, End.Blue, T);
      Color.Green = SFB_LERP (Mid.Green, End.Green, T);
      Color.Red = SFB_LERP (Mid.Red, End.Red, T);
    }
#undef SFB_LERP
    Color.Reserved = 0;
    GlyphText[0] = Text[Index];
    SfbGfxText (Cursor, Y, Size, GlyphText, &Color);
    Cursor += SfbGfxMeasureText (Size, GlyphText);
  }
}

STATIC
VOID
SfbGfxIcon (IN UINTN X, IN UINTN Y, IN UINTN Size, IN CANOE_UI_ICON Icon,
            IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color);

STATIC
VOID
SfbGfxIcon (IN UINTN X, IN UINTN Y, IN UINTN Size, IN CANOE_UI_ICON Icon,
            IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  CONST CANOE_ICON_PATH *Path;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer = NULL;
  UINT8 *Mask = NULL;
  UINTN High, Radius, Index, Sx, Sy, Count;
  INTN LastX = -1, LastY = -1;
  if (!mSfbGraphical || Icon == CanoeIconNone || Icon > CanoeIconWarning ||
      Size < 12 || Color == NULL) return;
  Path = &mCanoeIconPaths[Icon]; High = Size * 4;
  Mask = AllocateZeroPool (High * High);
  Buffer = AllocatePool (Size * Size * sizeof (*Buffer));
  if (Mask == NULL || Buffer == NULL) goto Done;
  Radius = MAX ((UINTN)3, High / 24);
  for (Index = 0; Index < Path->Count; Index++) {
    INTN X1,Y1,X0,Y0,Dx,Dy,Step,Steps;
    if (Path->Points[Index].X < 0) { LastX=LastY=-1; continue; }
    X1=Path->Points[Index].X*(INTN)(High-1)/24;
    Y1=Path->Points[Index].Y*(INTN)(High-1)/24;
    if (LastX < 0) { LastX=X1; LastY=Y1; }
    X0=LastX;Y0=LastY;Dx=X1-X0;Dy=Y1-Y0;
    Steps=MAX(Dx<0?-Dx:Dx,Dy<0?-Dy:Dy);
    for (Step=0;Step<=MAX(Steps,1);Step++) {
      INTN Cx=X0+Dx*Step/MAX(Steps,1),Cy=Y0+Dy*Step/MAX(Steps,1),Ox,Oy;
      for(Oy=-(INTN)Radius;Oy<=(INTN)Radius;Oy++)for(Ox=-(INTN)Radius;Ox<=(INTN)Radius;Ox++){
        INTN Px=Cx+Ox,Py=Cy+Oy;
        if(Px>=0&&Py>=0&&Px<(INTN)High&&Py<(INTN)High&&Ox*Ox+Oy*Oy<=(INTN)(Radius*Radius))Mask[Py*High+Px]=1;
      }
    }
    LastX=X1;LastY=Y1;
  }
  if(EFI_ERROR(mSfbGop->Blt(mSfbGop,Buffer,EfiBltVideoToBltBuffer,X,Y,0,0,Size,Size,Size*sizeof(*Buffer))))goto Done;
  for(Sy=0;Sy<Size;Sy++)for(Sx=0;Sx<Size;Sx++){
    Count=0;for(Index=0;Index<4;Index++){
      Count+=Mask[(Sy*4+Index)*High+Sx*4];Count+=Mask[(Sy*4+Index)*High+Sx*4+1];
      Count+=Mask[(Sy*4+Index)*High+Sx*4+2];Count+=Mask[(Sy*4+Index)*High+Sx*4+3];
    }
    if(Count){UINTN A=Count*255/16;EFI_GRAPHICS_OUTPUT_BLT_PIXEL *O=&Buffer[Sy*Size+Sx];
      O->Blue=(UINT8)((Color->Blue*A+O->Blue*(255-A)+127)/255);
      O->Green=(UINT8)((Color->Green*A+O->Green*(255-A)+127)/255);
      O->Red=(UINT8)((Color->Red*A+O->Red*(255-A)+127)/255);}
  }
  (VOID)mSfbGop->Blt(mSfbGop,Buffer,EfiBltBufferToVideo,0,0,X,Y,Size,Size,Size*sizeof(*Buffer));
Done:
  if(Mask!=NULL)FreePool(Mask);if(Buffer!=NULL)FreePool(Buffer);
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
  if (StrCmp (Marker, L"INFO") == 0) return CanoeIconInfo;
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
  UINTN                          SxFixed;
  UINTN                          SyFixed;
  UINTN                          Fx;
  UINTN                          Fy;
  UINTN                          SxNext;
  UINTN                          SyNext;
  UINTN                          A00;
  UINTN                          A10;
  UINTN                          A01;
  UINTN                          A11;
  UINTN                          Advance;
  UINTN                          Alpha;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel;

  if (!mSfbGraphical || Text == NULL || Size == 0) {
    return EFI_UNSUPPORTED;
  }

  /* Measure with the same scaled advance used by the raster loop. */
  for (Index = 0; Text[Index] != L'\0'; Index++) {
    Glyph = SfbFindGlyph (Text[Index]);
    if (Glyph == NULL) Glyph = SfbFindGlyph (L'?');
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
    Glyph = SfbFindGlyph (Text[Index]);
    if (Glyph == NULL) Glyph = SfbFindGlyph (L'?');
    if (Glyph == NULL) continue;
    Advance = ((UINTN)Glyph->Advance * Size + SFB_FONT_HEIGHT - 1) /
              SFB_FONT_HEIGHT;
    for (Dy = 0; Dy < Height; Dy++) {
      SyFixed = (Height > 1)
                  ? Dy * (SFB_FONT_HEIGHT - 1) * 256 / (Height - 1) : 0;
      Sy = SyFixed >> 8;
      Fy = SyFixed & 0xff;
      SyNext = MIN (Sy + 1, (UINTN)SFB_FONT_HEIGHT - 1);
      for (Dx = 0; Dx < Advance && Cursor + Dx < Width; Dx++) {
        SxFixed = (Advance > 1 && Glyph->Advance > 1)
                    ? Dx * (Glyph->Advance - 1) * 256 / (Advance - 1) : 0;
        Sx = SxFixed >> 8;
        Fx = SxFixed & 0xff;
        SxNext = MIN (Sx + 1, (UINTN)Glyph->Advance - 1);
        Alpha = Glyph->Bitmap[Sy * SFB_FONT_STRIDE + Sx / 2];
        A00 = (((Sx & 1) == 0) ? (Alpha >> 4) : (Alpha & 0x0f)) * 17;
        Alpha = Glyph->Bitmap[Sy * SFB_FONT_STRIDE + SxNext / 2];
        A10 = (((SxNext & 1) == 0) ? (Alpha >> 4) : (Alpha & 0x0f)) * 17;
        Alpha = Glyph->Bitmap[SyNext * SFB_FONT_STRIDE + Sx / 2];
        A01 = (((Sx & 1) == 0) ? (Alpha >> 4) : (Alpha & 0x0f)) * 17;
        Alpha = Glyph->Bitmap[SyNext * SFB_FONT_STRIDE + SxNext / 2];
        A11 = (((SxNext & 1) == 0) ? (Alpha >> 4) : (Alpha & 0x0f)) * 17;
        Alpha = (((A00 * (256 - Fx) + A10 * Fx) * (256 - Fy)) +
                 ((A01 * (256 - Fx) + A11 * Fx) * Fy) + 32768) >> 16;
        if (Alpha != 0) {
          Pixel = &Buffer[Dy * Width + Cursor + Dx];
          Pixel->Blue = (UINT8)((Color->Blue * Alpha +
                                 Pixel->Blue * (255 - Alpha) + 127) / 255);
          Pixel->Green = (UINT8)((Color->Green * Alpha +
                                  Pixel->Green * (255 - Alpha) + 127) / 255);
          Pixel->Red = (UINT8)((Color->Red * Alpha +
                                Pixel->Red * (255 - Alpha) + 127) / 255);
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

STATIC UINTN EFIAPI
SfbProtocolMeasureText (IN CANOE_UI_PROTOCOL *This, IN UINT16 Size,
                        IN CONST CHAR16 *Text)
{
  (VOID)This;
  return SfbGfxMeasureText (Size, Text);
}

STATIC EFI_STATUS EFIAPI
SfbProtocolDrawText (IN CANOE_UI_PROTOCOL *This, IN UINTN X, IN UINTN Y,
                     IN UINT16 Size, IN CONST CHAR16 *Text,
                     IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  (VOID)This;
  return SfbGfxText (X, Y, Size, Text, Color);
}

STATIC EFI_STATUS EFIAPI
SfbProtocolDrawIcon (IN CANOE_UI_PROTOCOL *This, IN UINTN X, IN UINTN Y,
                     IN UINTN Size, IN UINTN Icon,
                     IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  (VOID)This;
  if (Icon > CanoeIconWarning || Color == NULL) return EFI_INVALID_PARAMETER;
  SfbGfxIcon (X, Y, Size, (CANOE_UI_ICON)Icon, Color);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
SfbProtocolCaptureScreen (IN CANOE_UI_PROTOCOL *This)
{
  EFI_STATUS Status;
  (VOID)This;
  Status = SfbCaptureScreen ();
  if (mSfbGraphical) {
    CONST CHAR16 *Message = EFI_ERROR (Status)
      ? (mSfbLanguage == 0 ? L"截图保存失败" : L"Screenshot failed")
      : (mSfbLanguage == 0 ? L"截图已暂存" : L"Screenshot captured");
    UINTN Width=mSfbGop->Mode->Info->HorizontalResolution;
    UINTN TextWidth=SfbGfxMeasureText(30,Message);
    SfbGfxFill((Width-TextWidth-56)/2,mSfbSafeTop+18,TextWidth+56,62,
               EFI_ERROR(Status)?&mSfbColorWarning:&mSfbColorSurface);
    SfbGfxText((Width-TextWidth)/2,mSfbSafeTop+34,30,Message,&mSfbColorText);
    gBS->Stall(300000);
  }
  return Status;
}

STATIC CANOE_UI_PROTOCOL mSfbUiProtocol = {
  CANOE_UI_PROTOCOL_REVISION,
  SfbProtocolMeasureText,
  SfbProtocolDrawText,
  SfbProtocolDrawIcon,
  SfbProtocolCaptureScreen
};

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
  UINTN     PercentWidth;

  if (!mSfbGraphical || mSfbSafeTop < 48) {
    return;
  }

  Width = mSfbGop->Mode->Info->HorizontalResolution;
  StatusY = (mSfbSafeTop - CANOE_UI_STATUS_FONT) / 2;
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
  SfbGfxText (CANOE_UI_STATUS_INSET, StatusY, CANOE_UI_STATUS_FONT,
              TimeText, &mSfbColorText);

  SfbReadPowerStatus (&BatteryAvailable, &BatteryPercent, &Charging);
  UnicodeSPrint (PercentText, sizeof (PercentText),
                 BatteryAvailable ? L"%u%%" : L"--%%",
                 (UINT32)BatteryPercent);

  PercentWidth = SfbGfxMeasureText (CANOE_UI_STATUS_FONT, PercentText);
  BatteryX = Width - CANOE_UI_STATUS_INSET - PercentWidth - 94;
  BatteryY = StatusY + 4;
  /* Battery outline and terminal. */
  SfbGfxFill (BatteryX, BatteryY, 64, 3, &mSfbColorMuted);
  SfbGfxFill (BatteryX, BatteryY + 31, 64, 3, &mSfbColorMuted);
  SfbGfxFill (BatteryX, BatteryY, 3, 34, &mSfbColorMuted);
  SfbGfxFill (BatteryX + 61, BatteryY, 3, 34, &mSfbColorMuted);
  SfbGfxFill (BatteryX + 64, BatteryY + 10, 6, 14, &mSfbColorMuted);
  if (BatteryAvailable && BatteryPercent != 0) {
    FillWidth = 54 * BatteryPercent / 100;
    FillWidth = MAX (FillWidth, 2);
    SfbGfxFill (BatteryX + 5, BatteryY + 5, FillWidth, 24,
                Charging ? &mSfbColorPrimary : &mSfbColorText);
  }
  if (Charging) {
    /* Compact lightning mark immediately before the battery. */
    SfbGfxFill (BatteryX - 30, BatteryY + 2, 13, 8, &mSfbColorPrimary);
    SfbGfxFill (BatteryX - 24, BatteryY + 8, 13, 8, &mSfbColorPrimary);
    SfbGfxFill (BatteryX - 18, BatteryY + 14, 7, 11, &mSfbColorPrimary);
  }
  SfbGfxText (Width - CANOE_UI_STATUS_INSET - PercentWidth, StatusY,
              CANOE_UI_STATUS_FONT, PercentText, &mSfbColorText);
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

VOID
SfbUiDebounce (VOID)
{
  SfbWaitForSelectRelease ();
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
  if (mSfbGraphical && mSfbUiProtocolHandle == NULL) {
    (VOID)gBS->InstallProtocolInterface (&mSfbUiProtocolHandle,
                                         &gCanoeUiProtocolGuid,
                                         EFI_NATIVE_INTERFACE,
                                         &mSfbUiProtocol);
  }
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

/* Locate touch lazily because some platforms publish the absolute pointer only
 * after the menu's driver connection pass. Absence is harmless: keypad input
 * remains the primary handset fallback. */
STATIC
EFI_EVENT
SfbTouchWaitEvent (VOID)
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       HandleCount = 0;
  UINTN       Index;

  if (mSfbTouch != NULL) {
    return mSfbTouch->WaitForInput;
  }

  if (EFI_ERROR (gBS->LocateHandleBuffer (
                       ByProtocol, &gEfiAbsolutePointerProtocolGuid, NULL,
                       &HandleCount, &Handles))) {
    return NULL;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_ABSOLUTE_POINTER_PROTOCOL  *Candidate = NULL;

    if (!EFI_ERROR (gBS->HandleProtocol (
                          Handles[Index], &gEfiAbsolutePointerProtocolGuid,
                          (VOID **)&Candidate)) &&
        Candidate != NULL && Candidate->Mode != NULL &&
        Candidate->GetState != NULL && Candidate->WaitForInput != NULL &&
        Candidate->Mode->AbsoluteMaxY > Candidate->Mode->AbsoluteMinY) {
      mSfbTouch = Candidate;
      mSfbTouchTracking = FALSE;
      mSfbTouchGestureConsumed = FALSE;
      DEBUG ((EFI_D_INFO,
              "SFB: absolute pointer %u enabled, Y=%Lu..%Lu attr=0x%x\n",
              (UINT32)Index, Candidate->Mode->AbsoluteMinY,
              Candidate->Mode->AbsoluteMaxY, Candidate->Mode->Attributes));
      break;
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  return mSfbTouch == NULL ? NULL : mSfbTouch->WaitForInput;
}

STATIC EFI_STATUS EFIAPI
SfbProtocolCaptureScreen (IN CANOE_UI_PROTOCOL *This);

STATIC BOOLEAN
SfbTryScreenshotChord (IN UINT16 FirstScanCode)
{
  EFI_EVENT TimerEvent,WaitList[2];
  UINTN EventIndex;
  EFI_INPUT_KEY Key;
  if(EFI_ERROR(gBS->CreateEvent(EVT_TIMER,TPL_CALLBACK,NULL,NULL,&TimerEvent)))return FALSE;
  WaitList[0]=gST->ConIn->WaitForKey;WaitList[1]=TimerEvent;
  gBS->SetTimer(TimerEvent,TimerRelative,150ULL*10000);
  if(EFI_ERROR(gBS->WaitForEvent(2,WaitList,&EventIndex))||EventIndex==1){gBS->CloseEvent(TimerEvent);return FALSE;}
  gBS->CloseEvent(TimerEvent);
  if(EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)))return FALSE;
  if((FirstScanCode==SCAN_UP&&Key.ScanCode==SCAN_DOWN)||
     (FirstScanCode==SCAN_DOWN&&Key.ScanCode==SCAN_UP)){
    (VOID)SfbProtocolCaptureScreen(NULL);return TRUE;
  }
  return FALSE;
}

/* Translate one completed swipe threshold into the existing menu-key model.
 * The gesture fires as soon as it crosses the threshold, then remains consumed
 * until release so a single swipe cannot race through multiple rows. */
STATIC
SFB_KEY
SfbReadTouchGesture (VOID)
{
  EFI_ABSOLUTE_POINTER_STATE  State;
  EFI_STATUS                  Status;
  UINT64                      Delta;
  UINT64                      Range;
  UINT64                      Threshold;
  BOOLEAN                     Active;

  if (mSfbTouch == NULL) {
    return SfbKeyTimeout;
  }

  Status = mSfbTouch->GetState (mSfbTouch, &State);
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_READY) {
      DEBUG ((EFI_D_WARN, "SFB: absolute pointer failed: %r\n", Status));
      mSfbTouch = NULL;
      mSfbTouchTracking = FALSE;
      mSfbTouchGestureConsumed = FALSE;
    }
    return SfbKeyTimeout;
  }

  Active = (BOOLEAN)((State.ActiveButtons & EFI_ABSP_TouchActive) != 0);
  if (!Active) {
    mSfbTouchTracking = FALSE;
    mSfbTouchGestureConsumed = FALSE;
    return SfbKeyTimeout;
  }

  if (!mSfbTouchTracking) {
    mSfbTouchTracking = TRUE;
    mSfbTouchGestureConsumed = FALSE;
    mSfbTouchStartY = State.CurrentY;
    return SfbKeyTimeout;
  }
  if (mSfbTouchGestureConsumed) {
    return SfbKeyTimeout;
  }

  Range = mSfbTouch->Mode->AbsoluteMaxY -
          mSfbTouch->Mode->AbsoluteMinY;
  Threshold = MAX (Range / SFB_TOUCH_SWIPE_DIVISOR, 1);
  Delta = State.CurrentY >= mSfbTouchStartY
            ? State.CurrentY - mSfbTouchStartY
            : mSfbTouchStartY - State.CurrentY;
  if (Delta < Threshold) {
    return SfbKeyTimeout;
  }

  mSfbTouchGestureConsumed = TRUE;
  if (State.CurrentY < mSfbTouchStartY) {
    DEBUG ((EFI_D_VERBOSE, "SFB: touch swipe up\n"));
    return SfbKeyDown;
  }

  DEBUG ((EFI_D_VERBOSE, "SFB: touch swipe down\n"));
  return SfbKeyUp;
}

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      TouchPollEvent = NULL;
  EFI_EVENT      TouchEvent;
  EFI_EVENT      WaitList[4];
  UINTN          WaitCount;
  UINTN          EventIndex;
  UINTN          TimerEventIndex = MAX_UINTN;
  UINTN          TouchEventIndex = MAX_UINTN;
  UINTN          TouchPollEventIndex = MAX_UINTN;
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
  TouchEvent = SfbTouchWaitEvent ();
  if (TouchEvent != NULL) {
    TouchEventIndex = WaitCount;
    WaitList[WaitCount++] = TouchEvent;
  }
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL,
                             &TouchPollEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (TouchPollEvent, TimerPeriodic,
                            TouchEvent != NULL ? 20ULL * 10000
                                               : 250ULL * 10000);
    if (EFI_ERROR (Status)) {
      gBS->CloseEvent (TouchPollEvent);
      TouchPollEvent = NULL;
    } else {
      TouchPollEventIndex = WaitCount;
      WaitList[WaitCount++] = TouchPollEvent;
    }
  }
  if (TimerEvent != NULL) {
    TimerEventIndex = WaitCount;
    WaitList[WaitCount++] = TimerEvent;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == TimerEventIndex) {
      break;
    }

    if (EventIndex == TouchEventIndex) {
      Result = SfbReadTouchGesture ();
      if (Result != SfbKeyTimeout) {
        break;
      }
      continue;
    }

    if (EventIndex == TouchPollEventIndex) {
      if (mSfbTouch == NULL) {
        (VOID)SfbTouchWaitEvent ();
        if (mSfbTouch != NULL) {
          (VOID)gBS->SetTimer (TouchPollEvent, TimerPeriodic,
                               20ULL * 10000);
        }
      }
      Result = SfbReadTouchGesture ();
      if (Result != SfbKeyTimeout) {
        break;
      }
      continue;
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
      if (SfbTryScreenshotChord (SCAN_UP)) continue;
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      if (SfbTryScreenshotChord (SCAN_DOWN)) continue;
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
  if (TouchPollEvent != NULL) {
    gBS->CloseEvent (TouchPollEvent);
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
    mSfbSelectionAnimated = FALSE;
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
    SfbGfxCenteredText (
      Height - 76, CANOE_UI_FOOTER_FONT, 22,
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

STATIC
VOID
SfbGfxWrappedText (IN UINTN X, IN UINTN Y, IN UINTN Available,
                   IN UINT16 Size, IN CONST CHAR16 *Text,
                   IN UINTN MaxLines,
                   IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color)
{
  CHAR16  Line[96];
  UINTN   Start = 0;
  UINTN   Length;
  UINTN   Count;
  UINTN   Break;
  UINTN   LineIndex;

  if (Text == NULL) return;
  Length = StrLen (Text);
  for (LineIndex = 0; LineIndex < MaxLines && Start < Length; LineIndex++) {
    Count = 1;
    while (Start + Count <= Length && Count < ARRAY_SIZE (Line) &&
           SfbGfxMeasureText (Size, Text + Start) > Available) {
      /* The complete tail does not fit; find the longest fitting prefix. */
      StrnCpyS (Line, ARRAY_SIZE (Line), Text + Start, Count);
      if (SfbGfxMeasureText (Size, Line) > Available) break;
      Count++;
    }
    if (Start + Count > Length ||
        SfbGfxMeasureText (Size, Text + Start) <= Available) {
      Count = MIN (Length - Start, ARRAY_SIZE (Line) - 1);
    } else if (Count > 1) {
      Count--;
    }
    Break = Count;
    if (Start + Count < Length) {
      while (Break > 1 && Text[Start + Break] != L' ') Break--;
      if (Break > 1) Count = Break;
    }
    StrnCpyS (Line, ARRAY_SIZE (Line), Text + Start, Count);
    SfbGfxText (X, Y + LineIndex * (Size + 12), Size, Line, Color);
    Start += Count;
    while (Start < Length && Text[Start] == L' ') Start++;
  }
}

#define SFB_ART_MAX_BYTES  (16 * 1024)
#define SFB_ART_MAX_LINES  24
#define SFB_ART_MAX_COLS   96

STATIC UINTN
SfbLoadArtText (OUT CHAR16 Lines[SFB_ART_MAX_LINES][SFB_ART_MAX_COLS + 1])
{
  STATIC CONST CHAR8 Fallback[] =
    "   ____    _    _   _  ___  _____\n"
    "  / ___|  / \\  | \\ | |/ _ \\| ____|\n"
    " | |     / _ \\ |  \\| | | | |  _|\n"
    " | |___ / ___ \\| |\\  | |_| | |___\n"
    "  \\____/_/   \\_\\_| \\_|\\___/|_____|";
  CHAR8 *Raw = NULL;
  UINTN Bytes = 0, Count = 0, Column = 0, Index, VolumeCount = 0;
  EFI_HANDLE *Volumes = NULL;
  CONST CHAR8 *Source = Fallback;

  ZeroMem (Lines, SFB_ART_MAX_LINES * (SFB_ART_MAX_COLS + 1) * sizeof (CHAR16));
  if (!EFI_ERROR (SfbLocateVolumes (&Volumes, &VolumeCount)) && Volumes != NULL) {
    for (Index = 0; Index < VolumeCount; Index++) {
      EFI_FILE_PROTOCOL *Root = NULL;
      CONST CHAR16 *Path = SfbIsExt4Volume (Volumes[Index])
                             ? L"\\efisp\\ARTTEXT.TXT" : L"\\ARTTEXT.TXT";
      Raw = AllocateZeroPool (SFB_ART_MAX_BYTES + 1);
      if (Raw == NULL) break;
      if (!EFI_ERROR (SfbOpenVolumeRoot (Volumes[Index], &Root)) && Root != NULL &&
          !EFI_ERROR (SfbReadFileBytes (Root, Path, Raw,
                                        SFB_ART_MAX_BYTES, &Bytes)) && Bytes != 0) {
        Root->Close (Root); Source = Raw; break;
      }
      if (Root != NULL) Root->Close (Root);
      FreePool (Raw); Raw = NULL;
    }
    FreePool (Volumes);
  }
  for (Index = 0; Source[Index] != '\0' && Count < SFB_ART_MAX_LINES; Index++) {
    UINT8 Ch = (UINT8)Source[Index];
    if (Ch == '\r') continue;
    if (Ch == '\n') { Lines[Count][Column] = L'\0'; Count++; Column = 0; continue; }
    if (Ch == '\t') {
      UINTN Spaces = 4 - (Column & 3);
      while (Spaces-- != 0 && Column < SFB_ART_MAX_COLS) Lines[Count][Column++] = L' ';
    } else if (Ch >= 0x20 && Ch <= 0x7e && Column < SFB_ART_MAX_COLS) {
      Lines[Count][Column++] = (CHAR16)Ch;
    }
  }
  if (Count < SFB_ART_MAX_LINES && Column != 0) Count++;
  if (Raw != NULL) FreePool (Raw);
  return Count;
}

STATIC
VOID
SfbDrawArtText (IN BOOLEAN Animate)
{
  CHAR16 Lines[SFB_ART_MAX_LINES][SFB_ART_MAX_COLS + 1];
  CHAR16 FrameLine[SFB_ART_MAX_COLS + 1];
  UINTN Count, Frame, Row, Col, Width, Height, MaxWidth = 0, Size = 48;
  UINTN BoxX, BoxY, BoxW, BoxH, Preset;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Saved = NULL;
  if (!mSfbGraphical) return;
  Count = SfbLoadArtText (Lines); if (Count == 0) return;
  Width=mSfbGop->Mode->Info->HorizontalResolution;
  Height=mSfbGop->Mode->Info->VerticalResolution;
  while (Size > 18) {
    MaxWidth=0;for(Row=0;Row<Count;Row++)MaxWidth=MAX(MaxWidth,SfbGfxMeasureText((UINT16)Size,Lines[Row]));
    if(MaxWidth<=Width-2*CANOE_UI_SIDE_MARGIN && Count*(Size+8)<=Height-CANOE_UI_SAFE_TOP_MIN-CANOE_UI_FOOTER_HEIGHT)break;
    Size-=2;
  }
  BoxW=MIN(MaxWidth+32,Width);BoxH=Count*(Size+8)+24;
  BoxX=(Width-BoxW)/2;BoxY=(Height-BoxH)/2;
  Saved=AllocatePool(BoxW*BoxH*sizeof(*Saved));
  if(Saved==NULL||EFI_ERROR(mSfbGop->Blt(mSfbGop,Saved,EfiBltVideoToBltBuffer,BoxX,BoxY,0,0,BoxW,BoxH,BoxW*sizeof(*Saved))))goto Done;
  Preset=mSfbArtAnimation==7?(GetPerformanceCounter()%6)+1:mSfbArtAnimation;
  if (Preset == 0) Animate = FALSE;
  for(Frame=Animate?0:15;Frame<16;Frame++){
    (VOID)mSfbGop->Blt(mSfbGop,Saved,EfiBltBufferToVideo,0,0,BoxX,BoxY,BoxW,BoxH,BoxW*sizeof(*Saved));
    for(Row=0;Row<Count;Row++){
      UINTN Len=StrLen(Lines[Row]),Visible=0;
      INTN LineX;
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL DrawColor=mSfbColorPrimary;
      for(Col=0;Col<Len;Col++){
        BOOLEAN Show;
        UINTN Hash=(Col*37+Row*61+Col*Row*7)&15;
        if(Preset==0)Show=TRUE;
        else if(Preset==1)Show=(Row*SFB_ART_MAX_COLS+Col)*16<=(Frame+1)*(Count*SFB_ART_MAX_COLS);
        else if(Preset==2)Show=Row*16<=(Frame+1)*Count;
        else if(Preset==3)Show=Hash<=Frame;
        else if(Preset==4)Show=Frame>=((Col+Row*2)&7);
        else Show=(Frame>=MIN((Col+Row)&7,(UINTN)6));
        FrameLine[Col]=Show?Lines[Row][Col]:L' ';if(Show)Visible++;
      }
      FrameLine[Len]=L'\0';if(Visible==0)continue;
      if(Preset==5&&Frame<15&&((Row+Frame)&3)==0)DrawColor=mSfbColorText;
      LineX=(Width-SfbGfxMeasureText((UINT16)Size,FrameLine))/2;
      if(Preset==6&&Frame<12)LineX+=(INTN)(((Row*13+Frame*7)%7)-3)*3;
      SfbGfxText((UINTN)(LineX+3),BoxY+12+Row*(Size+8)+3,(UINT16)Size,FrameLine,&mSfbColorShadow);
      SfbGfxText((UINTN)LineX,BoxY+12+Row*(Size+8),(UINT16)Size,FrameLine,&DrawColor);
    }
    if(Animate&&Frame<15)gBS->Stall(50000);
  }
Done:
  if(Saved!=NULL)FreePool(Saved);
}

STATIC
VOID
SfbDrawDescriptionCard (IN CONST CHAR16 *Title, IN CONST CHAR16 *Description)
{
  UINTN  Width;
  UINTN  Height;
  UINTN  BoxX;
  UINTN  BoxY;
  UINTN  BoxWidth;
  UINTN  BoxHeight = 238;
  UINT16 TitleSize;

  if (!mSfbGraphical || !mSfbDescriptions || Description == NULL) return;
  Width = mSfbGop->Mode->Info->HorizontalResolution;
  Height = mSfbGop->Mode->Info->VerticalResolution;
  BoxX = CANOE_UI_STATUS_INSET;
  BoxWidth = Width - 2 * CANOE_UI_STATUS_INSET;
  BoxY = Height - CANOE_UI_FOOTER_HEIGHT - BoxHeight - 34;
  SfbGfxFill (BoxX + 12, BoxY + 14, BoxWidth, BoxHeight,
              &mSfbColorShadow);
  SfbGfxFill (BoxX, BoxY, BoxWidth, BoxHeight, &mSfbColorSurface);
  SfbGfxFill (BoxX, BoxY, 6, BoxHeight, &mSfbColorPrimary);
  SfbGfxFill (BoxX, BoxY, BoxWidth, 3, &mSfbColorPrimary);
  TitleSize = SfbGfxFitText (34, 26, BoxWidth - 70, Title);
  SfbGfxText (BoxX + 34, BoxY + 26, TitleSize, Title, &mSfbColorText);
  SfbGfxWrappedText (BoxX + 34, BoxY + 88, BoxWidth - 70, 28,
                     Description, 3, &mSfbColorMuted);
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
    BOOLEAN IsDefault = (BOOLEAN)(FallbackMarker != NULL &&
                                   StrCmp (FallbackMarker, L"*") == 0);
    UINTN  Width = mSfbGop->Mode->Info->HorizontalResolution;
    UINTN  CardWidth = Width - 2 * CANOE_UI_SIDE_MARGIN;
    UINTN  IconSize = MIN (CANOE_UI_ICON_BOX,
                           (mSfbRowHeight > 18) ? mSfbRowHeight - 18 : 12);
    UINTN  IconY = mSfbGfxY + (mSfbRowHeight - IconSize) / 2;
    UINTN  TextX = CANOE_UI_SIDE_MARGIN + 34 + CANOE_UI_ICON_BOX +
                   CANOE_UI_TEXT_GAP;
    UINTN  BadgeWidth = IsDefault ? 148 : 0;
    UINTN  TextWidth = Width - CANOE_UI_SIDE_MARGIN - TextX - 28 - BadgeWidth;
    UINT16 TextSize = SfbGfxFitText (CANOE_UI_BODY_FONT,
                                    CANOE_UI_BODY_FONT_MIN,
                                    TextWidth, Text);
    UINTN  Frame;
    TextSize = (UINT16)MIN ((UINTN)TextSize,
                            MAX ((UINTN)20, mSfbRowHeight - 16));
    UINTN  TextY = mSfbGfxY + (mSfbRowHeight - TextSize) / 2;

    if (Selected) {
      if (!mSfbSelectionAnimated) {
        for (Frame = 0; Frame < SFB_SELECTION_FRAMES; Frame++) {
          UINTN Linear = (Frame * 256) / (SFB_SELECTION_FRAMES - 1);
          UINTN Ease = 256 - (((256 - Linear) * (256 - Linear)) / 256);
          EFI_GRAPHICS_OUTPUT_BLT_PIXEL CardColor =
            SfbBlendColor (&mSfbColorSurface, &mSfbColorPrimary, Ease);
          EFI_GRAPHICS_OUTPUT_BLT_PIXEL ContentColor =
            SfbBlendColor (&mSfbColorText, &mSfbColorBackground, Ease);

          SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY,
                      CardWidth, mSfbRowHeight, &CardColor);
          SfbGfxIcon (CANOE_UI_SIDE_MARGIN + 28, IconY, IconSize, Icon,
                      &ContentColor);
          SfbGfxText (TextX, TextY, TextSize, Text, &ContentColor);
          if (Frame + 1 < SFB_SELECTION_FRAMES) {
            gBS->Stall (SFB_SELECTION_FRAME_US);
          }
        }
        mSfbSelectionAnimated = TRUE;
      } else {
        SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY,
                    CardWidth, mSfbRowHeight, &mSfbColorPrimary);
      }
    } else {
      SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY, CardWidth, mSfbRowHeight,
                  &mSfbColorSurface);
    }
    if (!Selected) {
      SfbGfxFill (CANOE_UI_SIDE_MARGIN, mSfbGfxY, 4,
                  mSfbRowHeight, &mSfbColorDisabled);
    }
    SfbGfxIcon (CANOE_UI_SIDE_MARGIN + 28, IconY, IconSize, Icon,
                Selected ? &mSfbColorBackground : &mSfbColorMuted);
    SfbGfxText (TextX, TextY, TextSize, Text,
                Selected ? &mSfbColorBackground : &mSfbColorText);
    if (IsDefault) {
      CONST CHAR16 *Badge = mSfbLanguage == 0 ? L"默认" : L"DEFAULT";
      UINT16 BadgeSize = SfbGfxFitText (26, 20, BadgeWidth - 28, Badge);
      UINTN BadgeTextWidth = SfbGfxMeasureText (BadgeSize, Badge);
      UINTN BadgeX = CANOE_UI_SIDE_MARGIN + CardWidth - BadgeWidth + 12;
      UINTN BadgeY = mSfbGfxY + (mSfbRowHeight - 54) / 2;
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BadgeColor =
        Selected ? &mSfbColorBackground : &mSfbColorPrimary;
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BadgeTextColor =
        Selected ? &mSfbColorPrimary : &mSfbColorBackground;
      SfbGfxFill (BadgeX, BadgeY, BadgeWidth - 24, 54, BadgeColor);
      SfbGfxText (BadgeX + (BadgeWidth - 24 - BadgeTextWidth) / 2,
                  BadgeY + (54 - BadgeSize) / 2,
                  BadgeSize, Badge, BadgeTextColor);
    }
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
  SfbLoadSettings ();
  SfbSetVisibleRows (CANOE_UI_VISIBLE_MIN);
  SfbBeginScreen (L"Fastboot",
                  mSfbLanguage == 0 ? L"正在初始化 USB 服务"
                                    : L"Initializing the USB service");
  SfbDrawRowIcon (TRUE, CanoeIconUsb, L"USB",
                  mSfbLanguage == 0 ? L"正在切换至 Superfastboot"
                                    : L"Switching to Superfastboot");
  SfbEndScreen (mSfbLanguage == 0 ? L"请稍候" : L"Please wait");
}

VOID
SfbDrawFastbootScreen (IN BOOLEAN Connected, IN UINTN Cursor)
{
  SfbLoadSettings ();
  SfbSetVisibleRows (CANOE_UI_VISIBLE_MIN);
  SfbBeginScreen (
    L"Superfastboot",
    Connected
      ? (mSfbLanguage == 0 ? L"USB 已连接，可执行 fastboot 命令"
                           : L"USB connected; fastboot commands are available")
      : (mSfbLanguage == 0 ? L"正在等待电脑连接"
                           : L"Waiting for a host connection"));
  SfbDrawRowIcon ((BOOLEAN)(Cursor == 0), CanoeIconPower, L"POWER",
                  mSfbLanguage == 0 ? L"关闭设备" : L"Power Off");
  SfbDrawRowIcon ((BOOLEAN)(Cursor == 1), CanoeIconRestart, L"POWER",
                  mSfbLanguage == 0 ? L"重新启动" : L"Restart");
  SfbEndScreen (mSfbLanguage == 0
                  ? L"音量 +/-：移动      电源键：确认"
                  : L"VOL +/-: Move      POWER: Select");
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbUpdateBootingStage (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen,
                       IN UINTN Stage)
{
  CHAR16 Text[SFB_UI_LINE_CHARS];
  STATIC BOOLEAN CustomAssetActive = FALSE;

  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  SfbLoadSettings ();
  if (mSfbBootVisual == 0) return;
  if (ClearScreen && Stage == 0) {
    SfbBeginScreen (L"Launching", L"Starting the selected EFI application");
  }
  SfbUiInitGraphics ();
  if (mSfbGraphical) {
    UINTN Width = mSfbGop->Mode->Info->HorizontalResolution;
    UINTN Height = mSfbGop->Mode->Info->VerticalResolution;
    UINTN CardWidth = MIN (Width - 2 * CANOE_UI_SIDE_MARGIN, (UINTN)1040);
    UINTN CardHeight = 230;
    UINTN CardX = (Width - CardWidth) / 2;
    UINTN CardY;
    UINTN Progress = MIN (Stage, (UINTN)3);
    if (Stage == 0) {
      CustomAssetActive = FALSE;
    }
    if (mSfbBootVisual == 3) {
      SfbDrawArtText ((BOOLEAN)(Stage == 0));
      return;
    }
    if (mSfbBootVisual == 2 && (Stage == 0 || CustomAssetActive) &&
        !EFI_ERROR (SfbDrawLaunchAsset (mSfbBootAssetLabel,
                                        mSfbBootAssetPath,
                                        mSfbBootPosition, Stage, mSfbGop,
                                        &mSfbColorBackground))) {
      CustomAssetActive = TRUE;
      return;
    }
    if (mSfbBootPosition == 0) CardY = mSfbSafeTop + CANOE_UI_HEADER_HEIGHT + 70;
    else if (mSfbBootPosition == 2) CardY = Height - CANOE_UI_FOOTER_HEIGHT - CardHeight - 80;
    else CardY = (Height - CardHeight) / 2;
    SfbGfxFill (CardX + 12, CardY + 14, CardWidth, CardHeight,
                &mSfbColorBackground);
    SfbGfxFill (CardX, CardY, CardWidth, CardHeight, &mSfbColorSurface);
    SfbGfxFill (CardX, CardY, 10, CardHeight, &mSfbColorPrimary);
    UnicodeSPrint (Text, sizeof (Text),
                   mSfbLanguage == 0 ? L"正在启动 %s" : L"Booting %s",
                   (Name != NULL && Name[0] != L'\0') ? Name : L"application");
    SfbGfxCenteredText (CardY + 52, 54, 32, Text, &mSfbColorText);
    SfbGfxFill (CardX + 64, CardY + CardHeight - 54,
                CardWidth - 128, 8, &mSfbColorDisabled);
    if (Progress != 0) {
      SfbGfxFill (CardX + 64, CardY + CardHeight - 54,
                  (CardWidth - 128) * Progress / 3, 8, &mSfbColorPrimary);
    }
    return;
  }
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_SUCCESS);
  Print (L"  Booting %s ...\r\n",
         (Name != NULL && Name[0] != L'\0') ? Name : L"application");
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen)
{
  SfbUpdateBootingStage (Name, ClearScreen, 0);
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
SfbBaseName (VOID)
{
  if (mSfbLanguage != 0) {
    switch (mSfbBase) {
    case 1: return L"Porcelain";
    case 2: return L"Smoke";
    case 3: return L"OLED Black";
    default: return L"Graphite";
    }
  }
  switch (mSfbBase) {
  case 1: return L"瓷白";
  case 2: return L"雾灰";
  case 3: return L"纯黑";
  default: return L"石墨";
  }
}

STATIC
CONST CHAR16 *
SfbAccentName (VOID)
{
  if (mSfbLanguage != 0) {
    switch (mSfbAccent) {
    case 1: return L"Cyan";
    case 2: return L"Coral";
    case 3: return L"Mint";
    case 4: return L"Amber";
    case 5: return L"Rose";
    case 6: return L"Lime";
    case 7: return L"Violet";
    default: return L"Monochrome";
    }
  }
  switch (mSfbAccent) {
  case 1: return L"青蓝";
  case 2: return L"珊瑚";
  case 3: return L"薄荷";
  case 4: return L"琥珀";
  case 5: return L"玫红";
  case 6: return L"青柠";
  case 7: return L"紫罗兰";
  default: return L"单色";
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
CONST CHAR16 *
SfbSettingsDescription (IN UINTN Cursor);

BOOLEAN
SfbSelectBootAsset (OUT CHAR8 *Label, IN UINTN LabelBytes,
                    OUT CHAR8 *Path, IN UINTN PathBytes);

STATIC
VOID
SfbRunSettings (VOID)
{
  UINTN    Cursor = 0;
  SFB_KEY  Key;

  while (TRUE) {
    CHAR16  Base[48];
    CHAR16  Accent[48];
    CHAR16  Language[48];
    CHAR16  Lock[48];
    CHAR16  Descriptions[48];
    CHAR16  BootVisual[64];
    CHAR16  BootPosition[48];
    CHAR16  ArtAnimation[64];
    STATIC CONST CHAR16 *ArtNamesZh[] = { L"静态",L"打字显现",L"扫描揭示",L"溶解聚合",L"波浪组装",L"流光落定",L"轻故障归位",L"随机" };
    STATIC CONST CHAR16 *ArtNamesEn[] = { L"Static",L"Type on",L"Scan reveal",L"Dissolve",L"Wave assemble",L"Shimmer",L"Glitch settle",L"Random" };
    UINTN   NextRow = 6;
    UINTN   PositionRow = (mSfbBootVisual == 1 || mSfbBootVisual == 2)
                            ? NextRow++ : MAX_UINTN;
    UINTN   ArtRow = (mSfbBootVisual == 3) ? NextRow++ : MAX_UINTN;
    UINTN   PreviewRow = NextRow++;
    UINTN   AssetRow = (mSfbBootVisual == 2) ? NextRow++ : MAX_UINTN;
    UINTN   PinRow = NextRow;
    UINTN   BackRow = PinRow + ((mSfbLockMode == SFB_LOCK_PIN) ? 1 : 0);
    UINTN   Count = BackRow + 1;
    UINTN   Start;
    UINTN   Last;

    UnicodeSPrint (Base, sizeof (Base),
                   mSfbLanguage == 0 ? L"明暗基底    %s" : L"Neutral base    %s",
                   SfbBaseName ());
    UnicodeSPrint (Accent, sizeof (Accent),
                   mSfbLanguage == 0 ? L"强调颜色    %s" : L"Accent color    %s",
                   SfbAccentName ());
    UnicodeSPrint (Language, sizeof (Language),
                   mSfbLanguage == 0 ? L"语言    中文" : L"Language    English");
    UnicodeSPrint (Lock, sizeof (Lock),
                   mSfbLanguage == 0 ? L"锁定方式    %s" : L"Lock mode    %s",
                   SfbLockName ());
    UnicodeSPrint (Descriptions, sizeof (Descriptions),
                   mSfbLanguage == 0 ? L"菜单说明    %s" : L"Menu descriptions    %s",
                   mSfbDescriptions
                     ? (mSfbLanguage == 0 ? L"开启" : L"On")
                     : (mSfbLanguage == 0 ? L"关闭" : L"Off"));
    UnicodeSPrint (BootVisual, sizeof (BootVisual),
                   mSfbLanguage == 0 ? L"启动画面    %s" : L"Launch visual    %s",
                   mSfbBootVisual == 0 ? (mSfbLanguage == 0 ? L"关闭" : L"Off") :
                   mSfbBootVisual == 1 ? (mSfbLanguage == 0 ? L"极简" : L"Minimal") :
                   mSfbBootVisual == 2 ? (mSfbLanguage == 0 ? L"自定义" : L"Custom") :
                                         (mSfbLanguage == 0 ? L"ASCII 艺术字" : L"ASCII art"));
    UnicodeSPrint (BootPosition, sizeof (BootPosition),
                   mSfbLanguage == 0 ? L"画面位置    %s" : L"Visual position    %s",
                   mSfbBootPosition == 0 ? (mSfbLanguage == 0 ? L"顶部" : L"Top") :
                   mSfbBootPosition == 1 ? (mSfbLanguage == 0 ? L"居中" : L"Center") :
                                           (mSfbLanguage == 0 ? L"底部" : L"Bottom"));
    UnicodeSPrint (ArtAnimation, sizeof (ArtAnimation),
                   mSfbLanguage == 0 ? L"艺术字动画    %s" : L"ASCII art    %s",
                   mSfbLanguage == 0 ? ArtNamesZh[mSfbArtAnimation]
                                     : ArtNamesEn[mSfbArtAnimation]);
    SfbSetVisibleRows (MIN (Count, (UINTN)SFB_VISIBLE_ROWS));
    SfbBeginScreen (L"Settings",
                    mSfbLanguage == 0 ? L"选择一项进行更改"
                                      : L"Select an item to change");
    Start = SfbWindowStart (Cursor, Count, SFB_VISIBLE_ROWS);
    Last = MIN (Count, Start + SFB_VISIBLE_ROWS);
#define SFB_SETTING_ROW(_index,_marker,_text) do { if ((_index)>=Start&&(_index)<Last) SfbDrawRow((BOOLEAN)(Cursor==(_index)),(_marker),(_text)); } while(0)
    SFB_SETTING_ROW (0, L"COLOR", Base);
    SFB_SETTING_ROW (1, L"COLOR", Accent);
    SFB_SETTING_ROW (2, L"LANG", Language);
    SFB_SETTING_ROW (3, L"LOCK", Lock);
    SFB_SETTING_ROW (4, L"INFO", Descriptions);
    SFB_SETTING_ROW (5, L"COLOR", BootVisual);
    if (PositionRow != MAX_UINTN) SFB_SETTING_ROW (PositionRow, L"INFO", BootPosition);
    if (ArtRow != MAX_UINTN && ArtRow >= Start && ArtRow < Last) SfbDrawRowIcon ((BOOLEAN)(Cursor == ArtRow), CanoeIconLanguage, L"TEXT", ArtAnimation);
    if (PreviewRow >= Start && PreviewRow < Last) SfbDrawRowIcon ((BOOLEAN)(Cursor == PreviewRow), CanoeIconBoot, L"PLAY", mSfbLanguage == 0 ? L"预览开机动画" : L"Preview boot animation");
    if (AssetRow != MAX_UINTN && AssetRow >= Start && AssetRow < Last) {
      SfbDrawRow ((BOOLEAN)(Cursor == AssetRow), L"FILES",
                  mSfbBootAssetPath[0] == '\0'
                    ? (mSfbLanguage == 0 ? L"选择 PNG/GIF" : L"Choose PNG/GIF")
                    : (mSfbLanguage == 0 ? L"更换启动素材" : L"Change launch asset"));
    }
    if (mSfbLockMode == SFB_LOCK_PIN && PinRow >= Start && PinRow < Last) {
      SfbDrawRow ((BOOLEAN)(Cursor == PinRow), L"PIN",
                  mSfbLanguage == 0 ? L"更改 PIN 密码" : L"Change PIN");
    }
    SFB_SETTING_ROW (BackRow, L"BACK", SfbLocalize (L"Back"));
#undef SFB_SETTING_ROW
    SfbEndScreen (L"Select");

    Key = SfbWaitForKey (mSfbDescriptions ? 2000 : 0);
    if (Key == SfbKeyTimeout && mSfbDescriptions) {
      CONST CHAR16 *TipTitle;
      if (Cursor == 0) TipTitle = Base;
      else if (Cursor == 1) TipTitle = Accent;
      else if (Cursor == 2) TipTitle = Language;
      else if (Cursor == 3) TipTitle = Lock;
      else if (Cursor == 4) TipTitle = Descriptions;
      else if (Cursor == 5) TipTitle = BootVisual;
      else if (Cursor == PositionRow) TipTitle = BootPosition;
      else if (Cursor == ArtRow) TipTitle = ArtAnimation;
      else if (Cursor == PreviewRow) TipTitle = mSfbLanguage == 0 ? L"预览开机动画" : L"Preview boot animation";
      else if (Cursor == AssetRow) TipTitle = mSfbLanguage == 0 ? L"选择启动素材" : L"Choose launch asset";
      else if (mSfbLockMode == SFB_LOCK_PIN && Cursor == PinRow) {
        TipTitle = mSfbLanguage == 0 ? L"更改 PIN 密码" : L"Change PIN";
      } else TipTitle = SfbLocalize (L"Back");
      SfbDrawDescriptionCard (TipTitle, SfbSettingsDescription (Cursor));
      Key = SfbWaitForKey (0);
    }
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Count, Key);
      continue;
    }
    if (Cursor == 0) {
      mSfbBase = (mSfbBase + 1) % CANOE_UI_BASE_COUNT;
      SfbApplyPalette ();
      SfbSaveSettings ();
    } else if (Cursor == 1) {
      mSfbAccent = (mSfbAccent + 1) % CANOE_UI_ACCENT_COUNT;
      SfbApplyPalette ();
      SfbSaveSettings ();
      Cursor = 1;
    } else if (Cursor == 2) {
      mSfbLanguage = (mSfbLanguage + 1) % 2;
      SfbSaveSettings ();
      Cursor = 2;
    } else if (Cursor == 3) {
      UINTN  NewMode = (mSfbLockMode + 1) % 3;

      if (NewMode == SFB_LOCK_PIN) {
        CHAR8  NewPin[5];

        ZeroMem (NewPin, sizeof (NewPin));
        SfbEditPin (FALSE, NewPin);
        CopyMem (mSfbPin, NewPin, sizeof (mSfbPin));
      }
      mSfbLockMode = NewMode;
      SfbSaveSettings ();
      Cursor = 3;
    } else if (Cursor == 4) {
      mSfbDescriptions = (BOOLEAN)!mSfbDescriptions;
      SfbSaveSettings ();
      Cursor = 4;
    } else if (Cursor == 5) {
      mSfbBootVisual = (mSfbBootVisual + 1) % 4;
      SfbSaveSettings ();
      Cursor = 5;
    } else if (Cursor == PositionRow) {
      mSfbBootPosition = (mSfbBootPosition + 1) % 3;
      SfbSaveSettings ();
      Cursor = PositionRow;
    } else if (Cursor == ArtRow) {
      mSfbArtAnimation = (mSfbArtAnimation + 1) % 8;
      SfbSaveSettings ();
      Cursor = ArtRow;
    } else if (Cursor == PreviewRow) {
      SfbUpdateBootingStage (mSfbLanguage == 0 ? L"预览" : L"Preview",
                             TRUE, 0);
      SfbWaitForKey (0);
      Cursor = PreviewRow;
    } else if (Cursor == AssetRow) {
      if (SfbSelectBootAsset (mSfbBootAssetLabel,
                              sizeof (mSfbBootAssetLabel),
                              mSfbBootAssetPath,
                              sizeof (mSfbBootAssetPath))) {
        SfbSaveSettings ();
      }
      Cursor = AssetRow;
    } else if (mSfbLockMode == SFB_LOCK_PIN && Cursor == PinRow) {
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
SfbDescribeEntry (IN CONST SFB_BOOT_ENTRY *Entry,
                  OUT CHAR16 *Description, IN UINTN DescriptionChars)
{
  if (Entry == NULL || Description == NULL || DescriptionChars == 0) return;
  Description[0] = L'\0';
  switch (Entry->Kind) {
  case SfbEntryEfiFile:
    UnicodeSPrint (
      Description, DescriptionChars * sizeof (CHAR16),
      mSfbLanguage == 0
        ? (Entry->NoDefault
             ? L"从 %s 启动 %s（%s）；此次选择不会修改默认启动项。"
             : L"从 %s 启动 %s（%s）；成功选择后会将它保存为默认启动项。")
        : (Entry->NoDefault
             ? L"Launch %s from %s (%s) without changing the saved default."
             : L"Launch %s from %s (%s) and save it as the default entry."),
      mSfbLanguage == 0 ? Entry->VolLabel : Entry->Desc,
      mSfbLanguage == 0 ? Entry->Desc : Entry->VolLabel,
      Entry->Path);
    break;
  case SfbEntrySubmenu:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"打开包含更多 EFI 工具和操作的子菜单。"
                               : L"Open a submenu containing more EFI tools and actions.");
    break;
  case SfbEntryFastboot:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"启动真正的 Superfastboot USB 服务，可由电脑执行 fastboot 命令。"
                               : L"Start the Superfastboot USB service for host fastboot commands.");
    break;
  case SfbEntrySelector:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"浏览 FAT32 或 EXT4 启动卷并临时运行 EFI 程序。"
                               : L"Browse FAT32 or EXT4 boot volumes and launch an EFI program temporarily.");
    break;
  case SfbEntrySettings:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"调整配色、语言、锁定方式和菜单说明。"
                               : L"Change the color theme, language, lock mode and menu descriptions.");
    break;
  case SfbEntryPowerOff:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"安全关闭设备电源。" : L"Power the device off safely.");
    break;
  case SfbEntryRestart:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"退出 UEFI 并重新启动设备。" : L"Exit UEFI and restart the device.");
    break;
  case SfbEntryBack:
    StrCpyS (Description, DescriptionChars,
             mSfbLanguage == 0 ? L"返回上一级菜单。" : L"Return to the previous menu.");
    break;
  default:
    break;
  }
}

STATIC
CONST CHAR16 *
SfbSettingsDescription (IN UINTN Cursor)
{
  UINTN  NextRow = 6;
  UINTN  PositionRow = (mSfbBootVisual == 1 || mSfbBootVisual == 2)
                         ? NextRow++ : MAX_UINTN;
  UINTN  ArtRow = (mSfbBootVisual == 3) ? NextRow++ : MAX_UINTN;
  UINTN  PreviewRow = NextRow++;
  UINTN  AssetRow = (mSfbBootVisual == 2) ? NextRow++ : MAX_UINTN;
  UINTN  PinRow = NextRow;

  if (mSfbLanguage == 0) {
    switch (Cursor) {
    case 0: return L"独立选择石墨、瓷白、雾灰或纯黑界面基底。";
    case 1: return L"在当前基底上自由混搭单色或七种强调颜色。";
    case 2: return L"在中文和英文界面之间切换。";
    case 3: return L"选择关闭、简易按键锁或四位 PIN 密码锁。";
    case 4: return L"控制菜单项停留两秒后是否显示功能说明。";
    case 5: return L"选择关闭、极简卡片、自定义 PNG/GIF 或 ASCII 艺术字；四者不会叠加。";
    default: break;
    }
    if (Cursor == PositionRow) return L"将极简或自定义启动画面放在顶部、中央或底部。";
    if (Cursor == ArtRow) return L"选择艺术字的有限开场动画；内容来自 persist/efisp/ARTTEXT.TXT。";
    if (Cursor == PreviewRow) return L"不启动 EFI 应用，直接预览当前选择的开机动画类型。";
    if (Cursor == AssetRow) return L"从已挂载卷选择 PNG 或 GIF 启动素材。";
    if (mSfbLockMode == SFB_LOCK_PIN && Cursor == PinRow) return L"更改四位 PIN 密码。";
    return L"返回启动菜单。";
  }
  switch (Cursor) {
  case 0: return L"Choose a graphite, porcelain, smoke, or OLED-black neutral base.";
  case 1: return L"Mix monochrome or any of seven accent colors with the current base.";
  case 2: return L"Switch the interface language between Chinese and English.";
  case 3: return L"Choose no lock, the simple key lock, or a four-digit PIN.";
  case 4: return L"Show or hide item descriptions after a two-second pause.";
  case 5: return L"Choose off, minimal, custom PNG/GIF, or ASCII art; the modes never overlay.";
  default: break;
  }
  if (Cursor == PositionRow) return L"Place a minimal or custom launch visual at the top, center, or bottom.";
  if (Cursor == ArtRow) return L"Choose the ASCII art entrance; content comes from persist/efisp/ARTTEXT.TXT.";
  if (Cursor == PreviewRow) return L"Preview the currently selected launch visual without starting an EFI app.";
  if (Cursor == AssetRow) return L"Choose a PNG or GIF launch asset from a mounted volume.";
  if (mSfbLockMode == SFB_LOCK_PIN && Cursor == PinRow) return L"Change the four-digit PIN.";
  return L"Return to the boot menu.";
}

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
                   ? L"%u 个启动项  /  徽标表示默认项"
                   : L"%u boot entries  /  badge marks default",
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
    case SfbEntryEfiFile:   Icon = CanoeIconBoot; break;
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
    Key = SfbWaitForKey (mSfbDescriptions ? 2000 : 0);
    if (Key == SfbKeyTimeout && mSfbDescriptions && Menu->Count != 0) {
      CHAR16 Description[SFB_UI_LINE_CHARS];
      SfbDescribeEntry (&Menu->Entry[Cursor], Description,
                        ARRAY_SIZE (Description));
      SfbDrawDescriptionCard (
        SfbUiEntryText (Menu->Entry[Cursor].Kind, Menu->Entry[Cursor].Desc),
        Description);
      Key = SfbWaitForKey (0);
    }

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
    Key = SfbWaitForKey (mSfbDescriptions ? 2000 : 0);
    if (Key == SfbKeyTimeout && mSfbDescriptions && Menu.Count != 0) {
      CHAR16 Description[SFB_UI_LINE_CHARS];
      SfbDescribeEntry (&Menu.Entry[Cursor], Description,
                        ARRAY_SIZE (Description));
      SfbDrawDescriptionCard (
        SfbUiEntryText (Menu.Entry[Cursor].Kind, Menu.Entry[Cursor].Desc),
        Description);
      Key = SfbWaitForKey (0);
    }

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
      /* Shipped tools do not alter volumes or boot-entry files. Keep the
       * already resolved menu so returning from a tool is instant. Unknown
       * third-party applications still receive the conservative rescan. */
      Rebuild = (BOOLEAN)(
        StrStr (Menu.Entry[Chosen].Path, L"\\tools\\RebootTools.efi") == NULL &&
        StrStr (Menu.Entry[Chosen].Path, L"\\tools\\ArbTools.efi") == NULL &&
        StrStr (Menu.Entry[Chosen].Path, L"\\tools\\BLTools.efi") == NULL &&
        StrStr (Menu.Entry[Chosen].Path, L"\\tools\\MiniGames.efi") == NULL);
      break;
    }
  }
}
