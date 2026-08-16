/** @file
 * MiniGames - Guess Number and a three-round reaction challenge.
 * Both games are designed for the handset's three available UEFI keys.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "AndroidToolsUi.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)  (sizeof (a) / sizeof ((a)[0]))
#endif

STATIC
UINT32
GameSeed (VOID)
{
  EFI_TIME  Time;
  if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    return Time.Nanosecond ^ ((UINT32)Time.Second << 17) ^
           ((UINT32)Time.Minute << 9) ^ ((UINT32)Time.Hour << 3);
  }
  return 0x51f15e1d;
}

STATIC
UINT32
GameNext (IN OUT UINT32 *State)
{
  *State = *State * 1664525U + 1013904223U;
  return *State;
}

STATIC
VOID
PlayGuessNumber (IN OUT UINT32 *RandomState)
{
  UINTN   Target = (GameNext (RandomState) % 20) + 1;
  UINTN   Guess = 10;
  UINTN   Attempts = 0;
  AT_KEY  Key;
  CHAR16  Current[16];
  CHAR16  Detail[80];
  CHAR16  Result[64];
  CONST CHAR16 *Hint = NULL;

  while (TRUE) {
    UnicodeSPrint (Current, sizeof (Current), L"%u", (UINT32)Guess);
    UnicodeSPrint (Detail, sizeof (Detail),
                   AtUiIsChinese () ? L"%s    已尝试 %u 次"
                                     : L"%s    %u attempts",
                   Hint != NULL ? AtUiLocalize (Hint) :
                     (AtUiIsChinese () ? L"选择 1-20" : L"Choose 1-20"),
                   (UINT32)Attempts);
    AtUiDrawFocusScreen (L"Guess Number", Current, Detail, 3);
    AtUiEndScreen (L"Vol+/- change, power submit");
    Key = AtUiWaitForKey (0);
    if (Key == AtKeyUp) {
      Guess = (Guess >= 20) ? 1 : Guess + 1;
    } else if (Key == AtKeyDown) {
      Guess = (Guess <= 1) ? 20 : Guess - 1;
    } else if (Key == AtKeySelect) {
      AtUiDebounce ();
      Attempts++;
      if (Guess == Target) {
        UnicodeSPrint (Result, sizeof (Result),
                       AtUiIsChinese () ? L"猜中了！共尝试 %u 次"
                                         : L"You got it in %u attempts!",
                       (UINT32)Attempts);
        AtUiDrawFocusScreen (L"Guess Number", L"OK", Result, 1);
        AtUiEndScreen (L"Press power to continue");
        AtUiWaitForKey (0);
        AtUiDebounce ();
        return;
      }
      Hint = (Guess > Target) ? L"Too high" : L"Too low";
    }
  }
}

STATIC
BOOLEAN
ReactionRound (IN UINTN Round, IN OUT UINT32 *RandomState, OUT UINTN *Elapsed)
{
  UINT32  Delay = 1400 + GameNext (RandomState) % 2600;
  UINTN   Ticks;
  AT_KEY  Key;
  CHAR16  RoundText[48];

  UnicodeSPrint (RoundText, sizeof (RoundText),
                 AtUiIsChinese () ? L"第 %u / 3 轮" : L"Round %u / 3",
                 (UINT32)Round);
  AtUiDrawFocusScreen (L"Reaction Challenge", L"...", RoundText, 2);
  AtUiEndScreen (AtUiIsChinese () ? L"提前按键会判定抢跑" : L"Pressing early is a false start");
  Key = AtUiWaitForKey (Delay);
  if (Key != AtKeyTimeout) {
    AtUiDebounce ();
    AtUiDrawFocusScreen (L"Reaction Challenge", L"!", L"False start!", 2);
    AtUiEndScreen (L"Press power to continue");
    AtUiWaitForKey (0);
    AtUiDebounce ();
    return FALSE;
  }

  AtUiDrawFocusScreen (L"Reaction Challenge", L"GO!", RoundText, 1);
  AtUiEndScreen (AtUiIsChinese () ? L"现在按下电源键" : L"Press power now");
  for (Ticks = 1; Ticks <= 500; Ticks++) {
    Key = AtUiWaitForKey (10);
    if (Key == AtKeySelect) {
      *Elapsed = Ticks * 10;
      AtUiDebounce ();
      return TRUE;
    }
  }
  *Elapsed = 5000;
  return TRUE;
}

STATIC
VOID
PlayReaction (IN OUT UINT32 *RandomState)
{
  UINTN   Round = 1;
  UINTN   Elapsed;
  UINTN   Total = 0;
  UINTN   Best = 5000;
  CHAR16  Result[80];

  while (Round <= 3) {
    if (!ReactionRound (Round, RandomState, &Elapsed)) {
      continue;
    }
    Total += Elapsed;
    Best = MIN (Best, Elapsed);
    UnicodeSPrint (Result, sizeof (Result),
                   AtUiIsChinese () ? L"本轮反应：%u 毫秒"
                                     : L"Reaction: %u ms",
                   (UINT32)Elapsed);
    AtUiDrawFocusScreen (L"Reaction Challenge", L"OK", Result, 1);
    AtUiEndScreen (L"Press power to continue");
    AtUiWaitForKey (0);
    AtUiDebounce ();
    Round++;
  }
  UnicodeSPrint (Result, sizeof (Result),
                 AtUiIsChinese () ? L"平均 %u 毫秒，最佳 %u 毫秒"
                                   : L"Average %u ms, best %u ms",
                 (UINT32)(Total / 3), (UINT32)Best);
  AtUiDrawFocusScreen (L"Reaction Challenge",
                       AtUiIsChinese () ? L"完成" : L"DONE", Result, 3);
  AtUiEndScreen (L"Press power to continue");
  AtUiWaitForKey (0);
  AtUiDebounce ();
}

EFI_STATUS
EFIAPI
MiniGamesEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  STATIC CONST CHAR16 *Items[] = {
    L"Guess Number",
    L"Reaction Challenge",
    L"Back",
  };
  UINTN       Selected;
  EFI_STATUS  Status;
  UINT32      RandomState = GameSeed ();

  AtUiInitialize (ImageHandle);
  AtUiEnterMenu (L"Mini Games");
  while (TRUE) {
    Status = AtUiRunMenu (L"Mini Games", Items, ARRAY_SIZE (Items), &Selected,
                          L"Vol+/- move, power select");
    if (EFI_ERROR (Status)) continue;
    if (Selected == 0) PlayGuessNumber (&RandomState);
    else if (Selected == 1) PlayReaction (&RandomState);
    else return EFI_SUCCESS;
  }
}
