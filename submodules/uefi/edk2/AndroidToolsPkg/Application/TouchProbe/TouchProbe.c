#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SpiConfiguration.h>
#include <Protocol/SpiHc.h>

#include "AndroidToolsUi.h"

STATIC
UINTN
TouchProbeProtocolCount (IN EFI_GUID *Protocol)
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;

  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL,
                                            &Count, &Handles)) &&
      Handles != NULL) {
    FreePool (Handles);
  }
  return Count;
}

STATIC
EFI_ABSOLUTE_POINTER_PROTOCOL *
TouchProbeFindPointer (OUT UINTN *PointerCount)
{
  EFI_HANDLE                     *Handles = NULL;
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Selected = NULL;
  UINTN                          Count = 0;
  UINTN                          Index;

  *PointerCount = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (
                       ByProtocol, &gEfiAbsolutePointerProtocolGuid, NULL,
                       &Count, &Handles))) {
    return NULL;
  }

  *PointerCount = Count;
  for (Index = 0; Index < Count; Index++) {
    EFI_ABSOLUTE_POINTER_PROTOCOL  *Pointer = NULL;

    if (!EFI_ERROR (gBS->HandleProtocol (
                          Handles[Index], &gEfiAbsolutePointerProtocolGuid,
                          (VOID **)&Pointer)) &&
        Pointer != NULL && Pointer->Mode != NULL &&
        Pointer->GetState != NULL && Pointer->WaitForInput != NULL) {
      DEBUG ((DEBUG_INFO,
              "TOUCHPROBE: abs[%u] X=%Lu..%Lu Y=%Lu..%Lu Z=%Lu..%Lu attr=0x%x\n",
              (UINT32)Index, Pointer->Mode->AbsoluteMinX,
              Pointer->Mode->AbsoluteMaxX, Pointer->Mode->AbsoluteMinY,
              Pointer->Mode->AbsoluteMaxY, Pointer->Mode->AbsoluteMinZ,
              Pointer->Mode->AbsoluteMaxZ, Pointer->Mode->Attributes));
      if (Selected == NULL &&
          Pointer->Mode->AbsoluteMaxY > Pointer->Mode->AbsoluteMinY) {
        Selected = Pointer;
      }
    }
  }

  FreePool (Handles);
  return Selected;
}

STATIC
VOID
TouchProbeLivePointer (IN EFI_SYSTEM_TABLE *SystemTable,
                       IN EFI_ABSOLUTE_POINTER_PROTOCOL *Pointer)
{
  EFI_EVENT                   WaitList[2];
  EFI_ABSOLUTE_POINTER_STATE  State;
  EFI_INPUT_KEY               Key;
  EFI_STATUS                  Status;
  UINTN                       EventIndex;
  UINTN                       Samples = 0;
  CHAR16                      Line[160];

  WaitList[0] = SystemTable->ConIn->WaitForKey;
  WaitList[1] = Pointer->WaitForInput;
  while (TRUE) {
    Status = gBS->WaitForEvent (2, WaitList, &EventIndex);
    if (EFI_ERROR (Status) || EventIndex == 0) {
      if (EventIndex == 0) {
        (VOID)SystemTable->ConIn->ReadKeyStroke (SystemTable->ConIn, &Key);
      }
      break;
    }

    Status = Pointer->GetState (Pointer, &State);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Samples++;
    AtUiBeginScreen (L"Touch Probe", L"Live Absolute Pointer state");
    UnicodeSPrint (Line, sizeof (Line), L"Samples: %u", (UINT32)Samples);
    AtUiWriteLine (Line);
    UnicodeSPrint (Line, sizeof (Line), L"X: %lu    Y: %lu    Z: %lu",
                   State.CurrentX, State.CurrentY, State.CurrentZ);
    AtUiWriteLine (Line);
    UnicodeSPrint (Line, sizeof (Line), L"Buttons: 0x%x    Touch: %s",
                   State.ActiveButtons,
                   (State.ActiveButtons & EFI_ABSP_TouchActive) != 0
                     ? L"DOWN" : L"UP");
    AtUiWriteLine (Line);
    AtUiWriteLine (L"Move a finger vertically and verify Y changes.");
    AtUiEndScreen (L"Press any hardware key to finish");
  }
}

EFI_STATUS
EFIAPI
TouchProbeEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE                     *Handles = NULL;
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Pointer;
  UINTN                          HandleCount = 0;
  UINTN                          PointerCount = 0;
  UINTN                          ProtocolTotal = 0;
  UINTN                          Index;
  CHAR16                         Line[160];
  EFI_STATUS                     Status;

  AtUiInitialize (ImageHandle);
  AtUiEnterMenu (L"Touch Probe");
  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL,
                                    &HandleCount, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      EFI_GUID  **Protocols = NULL;
      UINTN     Count = 0;
      UINTN     Probe;

      if (!EFI_ERROR (gBS->ProtocolsPerHandle (
                            Handles[Index], &Protocols, &Count))) {
        ProtocolTotal += Count;
        for (Probe = 0; Probe < Count; Probe++) {
          DEBUG ((DEBUG_INFO, "TOUCHPROBE: handle=%u protocol=%g\n",
                  (UINT32)Index, Protocols[Probe]));
        }
        FreePool (Protocols);
      }
    }
  }

  Pointer = TouchProbeFindPointer (&PointerCount);
  AtUiBeginScreen (L"Touch Probe", L"Read-only transport discovery");
  UnicodeSPrint (Line, sizeof (Line),
                 L"Handles: %u    Protocol bindings: %u",
                 (UINT32)HandleCount, (UINT32)ProtocolTotal);
  AtUiWriteLine (Line);
  UnicodeSPrint (Line, sizeof (Line),
                 L"Absolute Pointer instances: %u", (UINT32)PointerCount);
  AtUiWriteLine (Line);
  UnicodeSPrint (Line, sizeof (Line),
                 L"Standard SPI HC: %u    SPI Config: %u",
                 (UINT32)TouchProbeProtocolCount (&gEfiSpiHcProtocolGuid),
                 (UINT32)TouchProbeProtocolCount (
                           &gEfiSpiConfigurationProtocolGuid));
  AtUiWriteLine (Line);
  if (Pointer != NULL) {
    UnicodeSPrint (Line, sizeof (Line), L"Pointer Y: %lu..%lu  attr: 0x%x",
                   Pointer->Mode->AbsoluteMinY,
                   Pointer->Mode->AbsoluteMaxY,
                   Pointer->Mode->Attributes);
    AtUiWriteLine (Line);
    AtUiWriteLine (L"A live pointer test will open next.");
  } else {
    AtUiWriteLine (L"No usable Absolute Pointer was published by firmware.");
  }
  AtUiWriteLine (L"FT3683 read remains gated until its SPI ABI is verified.");
  AtUiWriteLine (L"No MMIO, GPIO, reset, power, or SPI write was issued.");
  AtUiEndScreen (Pointer != NULL ? L"Press power for live test"
                                 : L"Press power to return");
  AtUiWaitForKey (0);

  if (Pointer != NULL) {
    TouchProbeLivePointer (SystemTable, Pointer);
  }
  if (Handles != NULL) {
    FreePool (Handles);
  }
  return EFI_SUCCESS;
}
