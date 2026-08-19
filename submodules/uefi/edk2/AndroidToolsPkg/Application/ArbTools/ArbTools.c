/** @file
 *  ArbTools - a standalone UEFI tool launched from the super-fastboot boot
 *  menu. Reads each AVB rollback-index slot from the DeviceInfo blob (via the
 *  Verified Boot protocol, ported from the r32 tree) and offers a reset that
 *  zeroes every slot and writes it back.
 *
 *  Copyright (c) 2026, contributors to the canoe ABL tree.
 *  SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "AtDevInfo.h"
#include "AndroidToolsUi.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)  (sizeof (a) / sizeof ((a)[0]))
#endif

/* ---- ported r32 DeviceInfo / rollback helpers --------------------------- */

EFI_STATUS
AtDevInfoRead (
  OUT DeviceInfo *DevInfo
  )
{
  QCOM_VERIFIEDBOOT_PROTOCOL *VbIntf;
  EFI_STATUS                 Status;

  if (DevInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (&gEfiQcomVerifiedBootProtocolGuid, NULL,
                                (VOID **)&VbIntf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AT: VB protocol not found: %r\n", Status));
    return Status;
  }

  return VbIntf->VBRwDeviceState (VbIntf, READ_CONFIG, (UINT8 *)DevInfo,
                                  (UINT32)sizeof (DeviceInfo));
}

EFI_STATUS
AtDevInfoWrite (
  IN CONST DeviceInfo *DevInfo
  )
{
  QCOM_VERIFIEDBOOT_PROTOCOL *VbIntf;
  EFI_STATUS                 Status;

  if (DevInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (&gEfiQcomVerifiedBootProtocolGuid, NULL,
                                (VOID **)&VbIntf);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return VbIntf->VBRwDeviceState (VbIntf, WRITE_CONFIG, (UINT8 *)DevInfo,
                                  (UINT32)sizeof (DeviceInfo));
}

EFI_STATUS
AtReadRollbackIndex (
  IN  CONST DeviceInfo *DevInfo,
  IN  UINT32            Loc,
  OUT UINT64            *RollbackIndex
  )
{
  if (DevInfo == NULL || RollbackIndex == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Loc >= MAX_VB_PARTITIONS) {
    return EFI_INVALID_PARAMETER;
  }
  *RollbackIndex = DevInfo->rollback_index[Loc];
  return EFI_SUCCESS;
}

VOID
AtClearRollbackIndex (
  IN OUT DeviceInfo *DevInfo
  )
{
  if (DevInfo == NULL) {
    return;
  }
  SetMem (DevInfo->rollback_index, sizeof (DevInfo->rollback_index), 0);
}

/* ---- screens ------------------------------------------------------------ */

/**
  Show every rollback-index slot whose value is non-zero, in a scrollable list.
  Power returns. Slots that are still zero are skipped, so only the ARB indexes
  that are actually in use are shown.
**/
STATIC
VOID
AtShowArbValues (
  VOID
  )
{
  DeviceInfo  Info;
  EFI_STATUS  Status;
  CHAR16    **Lines = NULL;
  UINTN       Used;
  UINTN       Index;
  UINT64      Val;

  Status = AtDevInfoRead (&Info);
  if (EFI_ERROR (Status)) {
    AtUiReportStatus (L"Read DeviceInfo", Status);
    return;
  }

  /* Refuse to touch a blob whose magic does not match: the persist partition
   * would be uninitialized or corrupt, so the "existing" fields are garbage
   * and writing them back could clobber unrelated areas (unlock state, keys). */
  if (CompareMem (Info.magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE) != 0) {
    AtUiShowMessage (L"DeviceInfo not initialized");
    AtUiWaitForKey (0);
    AtUiDebounce ();
    return;
  }

  Lines = AllocateZeroPool (MAX_VB_PARTITIONS * sizeof (CHAR16 *));
  if (Lines == NULL) {
    AtUiReportStatus (L"Alloc", EFI_OUT_OF_RESOURCES);
    goto Out;
  }

  /* Walk every possible slot, keep only the non-zero ones. */
  Used = 0;
  for (Index = 0; Index < MAX_VB_PARTITIONS; Index++) {
    AtReadRollbackIndex (&Info, (UINT32)Index, &Val);
    if (Val == 0) {
      continue;
    }
    Lines[Used] = AllocatePool (40 * sizeof (CHAR16));
    if (Lines[Used] == NULL) {
      AtUiReportStatus (L"Alloc", EFI_OUT_OF_RESOURCES);
      goto Out;
    }
    UnicodeSPrint (Lines[Used], 40 * sizeof (CHAR16),
                   AtUiIsChinese () ? L"槽位 %2u：0x%016lx"
                                     : L"Slot %2u: 0x%016lx",
                   (UINT32)Index, Val);
    Used++;
  }

  if (Used == 0) {
    AtUiShowMessage (L"All rollback slots are 0");
    AtUiWaitForKey (0);
    AtUiDebounce ();
    goto Out;
  }

  /* Selection is ignored - power just returns to the main menu. */
  Index = 0;
  AtUiRunMenu (L"ARB Rollback Index (non-zero)", (CONST CHAR16 **)Lines, Used,
               &Index, L"Vol+/- scroll, power to return");

Out:
  for (Index = 0; Index < MAX_VB_PARTITIONS; Index++) {
    if (Lines != NULL && Lines[Index] != NULL) {
      FreePool (Lines[Index]);
    }
  }
  if (Lines != NULL) {
    FreePool (Lines);
  }
}

/** Show the risk first, then a final Cancel/Confirm menu defaulting to Cancel. */
STATIC
BOOLEAN
AtConfirmReset (
  VOID
  )
{
  return AtUiConfirmDanger (
           L"Reset ARB Index",
           AtUiIsChinese ()
             ? L"警告：此操作会写入 TEE，可能造成密钥丢失。"
             : L"WARNING: this writes to the TEE and may lose keys.");
}

/**
  Zero every rollback-index slot after requiring five confirmations.
**/
STATIC
VOID
AtResetArbValues (
  VOID
  )
{
  DeviceInfo  Info;
  EFI_STATUS  Status;

  if (!AtConfirmReset ()) {
    AtUiShowMessage (L"Reset cancelled");
    gBS->Stall (1000000);
    return;
  }

  AtUiShowMessage (L"Resetting ARB index...");

  Status = AtDevInfoRead (&Info);
  if (EFI_ERROR (Status)) {
    AtUiReportStatus (L"Read DeviceInfo", Status);
    return;
  }

  /* Only write back when the existing blob is valid, so the read-modify-write
   * preserves the real on-device fields. A bad magic means the persist data is
   * uninitialized/corrupt; zeroing rollback_index and persisting the rest would
   * clobber unrelated areas, so abort instead. */
  if (CompareMem (Info.magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE) != 0) {
    AtUiReportStatus (L"DeviceInfo magic", EFI_VOLUME_CORRUPTED);
    return;
  }

  AtClearRollbackIndex (&Info);

  Status = AtDevInfoWrite (&Info);
  if (EFI_ERROR (Status)) {
    AtUiReportStatus (L"Write DeviceInfo", Status);
    return;
  }

  AtUiShowMessage (L"ARB index reset complete");
  AtUiWaitForKey (0);
  AtUiDebounce ();
}

/* ---- entry point -------------------------------------------------------- */

EFI_STATUS
EFIAPI
ArbToolsEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  STATIC CONST CHAR16 *Items[] = {
    L"Get ARB Value",
    L"Reset ARB Value",
    L"Back",
  };
  STATIC CONST CHAR16 *DescriptionsEn[] = {
    L"Read and display all non-zero anti-rollback indexes.",
    L"Reset anti-rollback indexes after a two-stage safety confirmation.",
    L"Return to the Android Tools menu.",
  };
  STATIC CONST CHAR16 *DescriptionsZh[] = {
    L"读取并显示所有非零的防回滚索引。",
    L"经过二段式安全确认后重置防回滚索引。",
    L"返回 Android Tools 菜单。",
  };
  UINTN      Sel;
  EFI_STATUS Status;

  AtUiInitialize (ImageHandle);

  /*
   * The power press that selected us in the super-fastboot menu is often still
   * held when we start; drain it (after a release delay) so it cannot confirm
   * the first entry the instant the menu appears.
   */
  AtUiEnterMenu (L"ARB Tools");

  while (TRUE) {
    Status = AtUiRunMenuWithDescriptions (
               L"ARB Tools", Items,
               AtUiIsChinese () ? DescriptionsZh : DescriptionsEn,
               ARRAY_SIZE (Items), &Sel, L"Vol+/- move, power select");
    if (EFI_ERROR (Status)) {
      continue;
    }

    switch (Sel) {
    case 0:
      AtShowArbValues ();
      break;
    case 1:
      AtResetArbValues ();
      break;
    case 2:
      /* Exit the app so control returns to the boot menu. */
      return EFI_SUCCESS;
    default:
      break;
    }
  }

  return EFI_SUCCESS;
}
