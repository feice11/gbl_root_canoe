#include "SuperFbMenu.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>

#define SFB_SNAPSHOT_SLOT_BYTES  ((SIZE_1MB - SFB_STORE_SLOT_BYTES * SFB_STORE_SLOTS) / 2)

#pragma pack(1)
typedef struct {
  UINT8  Magic[4];
  UINT16 Version;
  UINT16 HeaderBytes;
  UINT16 Width;
  UINT16 Height;
  UINT8  Scale;
  UINT8  PixelFormat;
  UINT16 Reserved;
  UINT32 DataBytes;
  UINT32 DataCrc32;
  UINT32 Sequence;
} SFB_SCREENSHOT_HEADER;
#pragma pack()

STATIC UINT16
SfbRgb565 (IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixel)
{
  return (UINT16)(((Pixel->Red & 0xf8) << 8) |
                  ((Pixel->Green & 0xfc) << 3) | (Pixel->Blue >> 3));
}

STATIC UINTN
SfbEncodeScreenshot (IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels,
                     IN UINTN SourceWidth, IN UINTN SourceHeight,
                     IN UINTN Scale, OUT UINT8 *Out, IN UINTN Capacity)
{
  UINTN Width=(SourceWidth+Scale-1)/Scale,Height=(SourceHeight+Scale-1)/Scale;
  UINTN Total=Width*Height,Index=0,Used=0;
#define SAMPLE(_i) SfbRgb565(&Pixels[MIN(((_i)/Width)*Scale,SourceHeight-1)*SourceWidth+MIN(((_i)%Width)*Scale,SourceWidth-1)])
  while(Index<Total){
    UINT16 Value=SAMPLE(Index);UINTN Run=1,LiteralStart,Literal=0,I;
    while(Index+Run<Total&&Run<128&&SAMPLE(Index+Run)==Value)Run++;
    if(Run>=2){if(Used+3>Capacity)return 0;Out[Used++]=(UINT8)(0x80|(Run-1));Out[Used++]=(UINT8)Value;Out[Used++]=(UINT8)(Value>>8);Index+=Run;continue;}
    LiteralStart=Index;
    while(Index<Total&&Literal<128){
      Value=SAMPLE(Index);Run=1;while(Index+Run<Total&&Run<2&&SAMPLE(Index+Run)==Value)Run++;
      if(Run>=2&&Literal!=0)break;Index++;Literal++;
      if(Run>=2)break;
    }
    if(Used+1+Literal*2>Capacity)return 0;Out[Used++]=(UINT8)(Literal-1);
    for(I=0;I<Literal;I++){Value=SAMPLE(LiteralStart+I);Out[Used++]=(UINT8)Value;Out[Used++]=(UINT8)(Value>>8);}
  }
#undef SAMPLE
  return Used;
}

EFI_STATUS
SfbCaptureScreen (VOID)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels=NULL;
  SFB_SCREENSHOT_HEADER *Header;
  UINT8 *Record=NULL,*Payload;
  UINTN Width,Height,Scale,DataBytes=0,Capacity;
  EFI_STATUS Status;
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(EFI_ERROR(Status)||Gop==NULL||Gop->Mode==NULL)return EFI_UNSUPPORTED;
  Width=Gop->Mode->Info->HorizontalResolution;Height=Gop->Mode->Info->VerticalResolution;
  if(Width==0||Height==0||Width>4096||Height>4096)return EFI_BAD_BUFFER_SIZE;
  Pixels=AllocatePool(Width*Height*sizeof(*Pixels));
  Record=AllocateZeroPool(SFB_SNAPSHOT_SLOT_BYTES);
  if(Pixels==NULL||Record==NULL){Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=Gop->Blt(Gop,Pixels,EfiBltVideoToBltBuffer,0,0,0,0,Width,Height,Width*sizeof(*Pixels));
  if(EFI_ERROR(Status))goto Done;
  Header=(SFB_SCREENSHOT_HEADER *)Record;Payload=Record+sizeof(*Header);
  Capacity=SFB_SNAPSHOT_SLOT_BYTES-sizeof(*Header);
  for(Scale=1;Scale<=4;Scale*=2){DataBytes=SfbEncodeScreenshot(Pixels,Width,Height,Scale,Payload,Capacity);if(DataBytes!=0)break;}
  if(DataBytes==0){Status=EFI_BAD_BUFFER_SIZE;goto Done;}
  CopyMem(Header->Magic,"SFSS",4);Header->Version=1;Header->HeaderBytes=sizeof(*Header);
  Header->Width=(UINT16)((Width+Scale-1)/Scale);Header->Height=(UINT16)((Height+Scale-1)/Scale);
  Header->Scale=(UINT8)Scale;Header->PixelFormat=1;Header->DataBytes=(UINT32)DataBytes;
  Status=gBS->CalculateCrc32(Payload,DataBytes,&Header->DataCrc32);
  if(EFI_ERROR(Status))goto Done;
  Status=SfbStoreWriteSnapshot(Record,sizeof(*Header)+DataBytes);
Done:
  if(Pixels!=NULL)FreePool(Pixels);if(Record!=NULL)FreePool(Record);
  return Status;
}
