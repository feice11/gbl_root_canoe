#ifndef __SUPER_FB_IMAGE_H__
#define __SUPER_FB_IMAGE_H__
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
EFI_STATUS SfbDrawLaunchAsset (IN CONST CHAR8 *VolumeLabel, IN CONST CHAR8 *Path,
  IN UINTN Position, IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Background);
#endif
