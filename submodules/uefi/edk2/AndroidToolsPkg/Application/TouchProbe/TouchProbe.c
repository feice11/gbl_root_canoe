#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AbsolutePointer.h>
#include "AndroidToolsUi.h"

EFI_STATUS EFIAPI TouchProbeEntry(IN EFI_HANDLE ImageHandle,IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE *Handles=NULL;UINTN HandleCount=0,Index,ProtocolTotal=0;
  EFI_ABSOLUTE_POINTER_PROTOCOL *Pointer=NULL;CHAR16 Line[160];EFI_STATUS Status;
  (VOID)SystemTable;AtUiInitialize(ImageHandle);AtUiEnterMenu(L"Touch Probe");
  Status=gBS->LocateHandleBuffer(AllHandles,NULL,NULL,&HandleCount,&Handles);
  if(!EFI_ERROR(Status))for(Index=0;Index<HandleCount;Index++){
    EFI_GUID **Protocols=NULL;UINTN Count=0,Probe;
    if(!EFI_ERROR(gBS->ProtocolsPerHandle(Handles[Index],&Protocols,&Count))){
      ProtocolTotal+=Count;for(Probe=0;Probe<Count;Probe++)DEBUG((DEBUG_INFO,"TOUCHPROBE: handle=%u protocol=%g\n",(UINT32)Index,Protocols[Probe]));
      FreePool(Protocols);
    }
  }
  AtUiBeginScreen(L"Touch Probe",L"Read-only transport discovery");
  UnicodeSPrint(Line,sizeof(Line),L"Handles: %u    Protocol bindings: %u",(UINT32)HandleCount,(UINT32)ProtocolTotal);AtUiWriteLine(Line);
  Status=gBS->LocateProtocol(&gEfiAbsolutePointerProtocolGuid,NULL,(VOID **)&Pointer);
  if(!EFI_ERROR(Status)&&Pointer!=NULL&&Pointer->Mode!=NULL){
    UnicodeSPrint(Line,sizeof(Line),L"Absolute Pointer: present  X %lu..%lu",Pointer->Mode->AbsoluteMinX,Pointer->Mode->AbsoluteMaxX);AtUiWriteLine(Line);
    UnicodeSPrint(Line,sizeof(Line),L"Y %lu..%lu  attributes 0x%x",Pointer->Mode->AbsoluteMinY,Pointer->Mode->AbsoluteMaxY,Pointer->Mode->Attributes);AtUiWriteLine(Line);
  }else AtUiWriteLine(L"Absolute Pointer: not published by firmware");
  AtUiWriteLine(L"FT3683 register read: gated (no verified SPI protocol contract)");
  AtUiWriteLine(L"No MMIO, GPIO, reset, power, or SPI write was issued.");
  AtUiEndScreen(L"Press power to return");AtUiWaitForKey(0);
  if(Handles!=NULL)FreePool(Handles);return EFI_SUCCESS;
}
