#include "SuperFbMenu.h"
#include "SuperFbImage.h"
#include "CanoeUiStyle.h"
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/HiiImageDecoder.h>

#define SFB_ASSET_MAX_BYTES  (16 * 1024 * 1024)
#define SFB_ASSET_MAX_DIM    4096

STATIC VOID SfbAsciiToUnicode (IN CONST CHAR8 *In, OUT CHAR16 *Out, IN UINTN Max)
{
  UINTN I;
  for (I = 0; I + 1 < Max && In[I] != '\0'; I++) Out[I] = (CHAR16)(UINT8)In[I];
  Out[I] = L'\0';
}

STATIC EFI_STATUS SfbReadAsset (IN CONST CHAR8 *WantLabel,
  IN CONST CHAR8 *AsciiPath, OUT VOID **Data, OUT UINTN *DataBytes)
{
  EFI_HANDLE *Volumes = NULL;
  UINTN Count = 0, Index;
  CHAR16 Path[SFB_PATH_CHARS], Label[SFB_DESC_CHARS], Want[SFB_DESC_CHARS];
  EFI_STATUS Result = EFI_NOT_FOUND;
  SfbAsciiToUnicode (AsciiPath, Path, ARRAY_SIZE (Path));
  SfbAsciiToUnicode (WantLabel, Want, ARRAY_SIZE (Want));
  *Data = NULL; *DataBytes = 0;
  if (EFI_ERROR (SfbLocateVolumes (&Volumes, &Count)) || Volumes == NULL)
    return EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
    EFI_FILE_INFO *Info = NULL;
    UINTN InfoBytes = 0, ReadBytes;
    VOID *Buffer;
    if (EFI_ERROR (SfbOpenVolumeRoot (Volumes[Index], &Root)) || Root == NULL) continue;
    Label[0] = L'\0'; SfbGetVolumeLabel (Root, Label, ARRAY_SIZE (Label));
    if (Want[0] != L'\0' && StrCmp (Want, Label) != 0) { Root->Close (Root); continue; }
    if (EFI_ERROR (Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0))) {
      Root->Close (Root); continue;
    }
    Result = File->GetInfo (File, &gEfiFileInfoGuid, &InfoBytes, NULL);
    if (Result == EFI_BUFFER_TOO_SMALL) Info = AllocatePool (InfoBytes);
    if (Info == NULL || EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid,
        &InfoBytes, Info)) || Info->FileSize == 0 ||
        Info->FileSize > SFB_ASSET_MAX_BYTES) {
      Result = EFI_BAD_BUFFER_SIZE;
      if (Info != NULL) FreePool (Info);
      File->Close (File); Root->Close (Root); break;
    }
    ReadBytes = (UINTN)Info->FileSize; FreePool (Info);
    Buffer = AllocatePool (ReadBytes);
    if (Buffer == NULL) { Result = EFI_OUT_OF_RESOURCES; File->Close (File); Root->Close (Root); break; }
    Result = File->Read (File, &ReadBytes, Buffer);
    File->Close (File); Root->Close (Root);
    if (!EFI_ERROR (Result)) { *Data = Buffer; *DataBytes = ReadBytes; }
    else FreePool (Buffer);
    break;
  }
  FreePool (Volumes);
  return Result;
}

EFI_STATUS SfbDrawLaunchAsset (IN CONST CHAR8 *VolumeLabel, IN CONST CHAR8 *Path,
  IN UINTN Position, IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Background)
{
  VOID *Raw = NULL;
  UINTN RawBytes = 0, HandleCount = 0, Index, ScreenW, ScreenH;
  UINTN MaxW, MaxH, DrawW, DrawH, X, Y;
  EFI_HANDLE *Handles = NULL;
  EFI_IMAGE_OUTPUT *Image = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Scaled = NULL;
  EFI_STATUS Status;
  if (Path == NULL || Path[0] == '\0' || Gop == NULL) return EFI_NOT_FOUND;
  Status = SfbReadAsset (VolumeLabel, Path, &Raw, &RawBytes);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiHiiImageDecoderProtocolGuid,
                                    NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) { FreePool (Raw); return EFI_UNSUPPORTED; }
  Status = EFI_UNSUPPORTED;
  for (Index = 0; Index < HandleCount; Index++) {
    EFI_HII_IMAGE_DECODER_PROTOCOL *Decoder = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
        &gEfiHiiImageDecoderProtocolGuid, (VOID **)&Decoder)) && Decoder != NULL) {
      Image = NULL;
      Status = Decoder->DecodeImage (Decoder, Raw, RawBytes, &Image, TRUE);
      if (!EFI_ERROR (Status) && Image != NULL) break;
    }
  }
  FreePool (Handles); FreePool (Raw);
  if (EFI_ERROR (Status) || Image == NULL || Image->Image.Bitmap == NULL ||
      Image->Width == 0 || Image->Height == 0 ||
      Image->Width > SFB_ASSET_MAX_DIM || Image->Height > SFB_ASSET_MAX_DIM) {
    if (Image != NULL) FreePool (Image);
    return EFI_UNSUPPORTED;
  }
  ScreenW = Gop->Mode->Info->HorizontalResolution;
  ScreenH = Gop->Mode->Info->VerticalResolution;
  MaxW = ScreenW - 2 * CANOE_UI_SIDE_MARGIN;
  MaxH = (ScreenH - 2 * CANOE_UI_SAFE_TOP_MIN) * 2 / 5;
  DrawW = Image->Width; DrawH = Image->Height;
  if (DrawW > MaxW || DrawH > MaxH) {
    if (MaxW * DrawH < MaxH * DrawW) { DrawH = DrawH * MaxW / DrawW; DrawW = MaxW; }
    else { DrawW = DrawW * MaxH / DrawH; DrawH = MaxH; }
  }
  Scaled = AllocatePool (DrawW * DrawH * sizeof (*Scaled));
  if (Scaled == NULL) { FreePool (Image->Image.Bitmap); FreePool (Image); return EFI_OUT_OF_RESOURCES; }
  for (Y = 0; Y < DrawH; Y++) {
    UINTN Syf = (DrawH > 1) ? Y * (Image->Height - 1) * 256 / (DrawH - 1) : 0;
    UINTN Sy = Syf >> 8, Fy = Syf & 255;
    UINTN Sy1 = MIN (Sy + 1, (UINTN)Image->Height - 1);
    for (X = 0; X < DrawW; X++) {
      UINTN Sxf = (DrawW > 1) ? X * (Image->Width - 1) * 256 / (DrawW - 1) : 0;
      UINTN Sx = Sxf >> 8, Fx = Sxf & 255;
      UINTN Sx1 = MIN (Sx + 1, (UINTN)Image->Width - 1);
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P00 = &Image->Image.Bitmap[Sy * Image->Width + Sx];
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P10 = &Image->Image.Bitmap[Sy * Image->Width + Sx1];
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P01 = &Image->Image.Bitmap[Sy1 * Image->Width + Sx];
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P11 = &Image->Image.Bitmap[Sy1 * Image->Width + Sx1];
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *D = &Scaled[Y * DrawW + X];
#define SFB_BILERP(_c) ((UINT8)((((UINTN)P00->_c * (256-Fx) + (UINTN)P10->_c * Fx) * (256-Fy) + ((UINTN)P01->_c * (256-Fx) + (UINTN)P11->_c * Fx) * Fy + 32768) >> 16))
      D->Blue = SFB_BILERP (Blue); D->Green = SFB_BILERP (Green);
      D->Red = SFB_BILERP (Red); D->Reserved = SFB_BILERP (Reserved);
#undef SFB_BILERP
      if (Background != NULL && D->Reserved != 0 && D->Reserved != 255) {
        UINTN A = D->Reserved;
        D->Blue = (UINT8)((D->Blue * A + Background->Blue * (255-A) + 127) / 255);
        D->Green = (UINT8)((D->Green * A + Background->Green * (255-A) + 127) / 255);
        D->Red = (UINT8)((D->Red * A + Background->Red * (255-A) + 127) / 255);
      }
    }
  }
  X = (ScreenW - DrawW) / 2;
  if (Position == 0) Y = CANOE_UI_SAFE_TOP_MIN + 80;
  else if (Position == 2) Y = ScreenH - CANOE_UI_FOOTER_HEIGHT - DrawH - 80;
  else Y = (ScreenH - DrawH) / 2;
  Status = Gop->Blt (Gop, Scaled, EfiBltBufferToVideo, 0, 0, X, Y,
                     DrawW, DrawH, DrawW * sizeof (*Scaled));
  FreePool (Scaled); FreePool (Image->Image.Bitmap); FreePool (Image);
  return Status;
}
