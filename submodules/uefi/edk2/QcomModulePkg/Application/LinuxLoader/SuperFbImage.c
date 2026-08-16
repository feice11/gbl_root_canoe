#include "SuperFbMenu.h"
#include "SuperFbImage.h"
#include "CanoeUiStyle.h"
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/HiiImageDecoder.h>

#define SFB_ASSET_MAX_BYTES  (16 * 1024 * 1024)
#define SFB_ASSET_MAX_DIM    4096

typedef struct { CONST UINT8 *P; UINTN N, Pos; UINT32 Bits; UINTN Have; } SFB_BITS;
typedef struct { UINT16 Code[288]; UINT8 Len[288]; UINTN Count; } SFB_HUFF;

STATIC BOOLEAN SfbBits (SFB_BITS *B, UINTN N, UINT32 *V)
{
  while (B->Have < N) {
    if (B->Pos >= B->N) return FALSE;
    B->Bits |= (UINT32)B->P[B->Pos++] << B->Have; B->Have += 8;
  }
  *V = B->Bits & ((1U << N) - 1); B->Bits >>= N; B->Have -= N; return TRUE;
}
STATIC UINT16 SfbReverse (UINT16 V, UINTN N)
{
  UINT16 R = 0; while (N-- != 0) { R = (UINT16)((R << 1) | (V & 1)); V >>= 1; } return R;
}
STATIC BOOLEAN SfbHuffBuild (SFB_HUFF *H, CONST UINT8 *Lens, UINTN Count)
{
  UINT16 Counts[16], Next[16]; UINTN I; UINT16 Code = 0;
  ZeroMem (Counts, sizeof (Counts)); ZeroMem (Next, sizeof (Next)); H->Count = Count;
  if (Count > ARRAY_SIZE (H->Code)) return FALSE;
  for (I=0; I<Count; I++) { if (Lens[I] > 15) return FALSE; H->Len[I]=Lens[I]; if(Lens[I]) Counts[Lens[I]]++; }
  for (I=1; I<=15; I++) { Code=(UINT16)((Code+Counts[I-1])<<1); Next[I]=Code; }
  for (I=0; I<Count; I++) H->Code[I]=Lens[I]?SfbReverse(Next[Lens[I]]++,Lens[I]):0;
  return TRUE;
}
STATIC BOOLEAN SfbHuffGet (SFB_BITS *B, SFB_HUFF *H, UINT32 *Sym)
{
  UINT32 Code=0, Bit; UINTN L,I;
  for(L=1;L<=15;L++){ if(!SfbBits(B,1,&Bit))return FALSE; Code|=Bit<<(L-1);
    for(I=0;I<H->Count;I++) if(H->Len[I]==L&&H->Code[I]==Code){*Sym=(UINT32)I;return TRUE;} }
  return FALSE;
}
STATIC EFI_STATUS SfbInflate (CONST UINT8 *In, UINTN InBytes, UINT8 *Out, UINTN OutBytes)
{
  STATIC CONST UINT16 LB[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
  STATIC CONST UINT8 LE[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
  STATIC CONST UINT16 DB[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
  STATIC CONST UINT8 DE[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
  STATIC CONST UINT8 Order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
  SFB_BITS B; UINTN Op=0; UINT32 Final=0;
  if(InBytes<6 || (In[0]&15)!=8) return EFI_UNSUPPORTED;
  B.P=In+2; B.N=InBytes-6; B.Pos=0; B.Bits=0; B.Have=0;
  while(!Final){ UINT32 Type; SFB_HUFF Lit,Dist; UINT8 LL[288],DL[32]; UINTN I;
    if(!SfbBits(&B,1,&Final)||!SfbBits(&B,2,&Type))return EFI_VOLUME_CORRUPTED;
    ZeroMem(LL,sizeof(LL));ZeroMem(DL,sizeof(DL));
    if(Type==0){UINT32 Len,Nlen; B.Bits=0;B.Have=0; if(B.Pos+4>B.N)return EFI_VOLUME_CORRUPTED;
      Len=B.P[B.Pos]|(B.P[B.Pos+1]<<8);Nlen=B.P[B.Pos+2]|(B.P[B.Pos+3]<<8);B.Pos+=4;
      if((Len^(Nlen&0xffff))!=0xffff||B.Pos+Len>B.N||Op+Len>OutBytes)return EFI_VOLUME_CORRUPTED;
      CopyMem(Out+Op,B.P+B.Pos,Len);B.Pos+=Len;Op+=Len;continue; }
    if(Type==1){for(I=0;I<=143;I++)LL[I]=8;for(;I<=255;I++)LL[I]=9;for(;I<=279;I++)LL[I]=7;for(;I<288;I++)LL[I]=8;for(I=0;I<32;I++)DL[I]=5;}
    else if(Type==2){UINT32 HL,HD,HC,V,Sym;UINT8 CL[19];SFB_HUFF CH;UINTN Total,Pos=0;
      if(!SfbBits(&B,5,&HL)||!SfbBits(&B,5,&HD)||!SfbBits(&B,4,&HC))return EFI_VOLUME_CORRUPTED;HL+=257;HD+=1;HC+=4;ZeroMem(CL,sizeof(CL));
      for(I=0;I<HC;I++){if(!SfbBits(&B,3,&V))return EFI_VOLUME_CORRUPTED;CL[Order[I]]=(UINT8)V;}if(!SfbHuffBuild(&CH,CL,19))return EFI_VOLUME_CORRUPTED;
      Total=HL+HD;while(Pos<Total){UINTN Repeat=1;UINT8 Val;if(!SfbHuffGet(&B,&CH,&Sym))return EFI_VOLUME_CORRUPTED;
        if(Sym<16)Val=(UINT8)Sym;else if(Sym==16){if(Pos==0||!SfbBits(&B,2,&V))return EFI_VOLUME_CORRUPTED;Repeat=V+3;Val=(Pos<=HL)?LL[MIN(Pos-1,HL-1)]:DL[Pos-HL-1];}
        else if(Sym==17){if(!SfbBits(&B,3,&V))return EFI_VOLUME_CORRUPTED;Repeat=V+3;Val=0;}else{if(!SfbBits(&B,7,&V))return EFI_VOLUME_CORRUPTED;Repeat=V+11;Val=0;}
        while(Repeat--&&Pos<Total){if(Pos<HL)LL[Pos]=(UINT8)Val;else DL[Pos-HL]=(UINT8)Val;Pos++;}}
    } else return EFI_UNSUPPORTED;
    if(!SfbHuffBuild(&Lit,LL,Type==2?288:288)||!SfbHuffBuild(&Dist,DL,Type==2?32:32))return EFI_VOLUME_CORRUPTED;
    while(TRUE){UINT32 Sym,V,DS;UINTN Len,Distance;
      if(!SfbHuffGet(&B,&Lit,&Sym))return EFI_VOLUME_CORRUPTED;
      if(Sym<256){if(Op>=OutBytes)return EFI_BAD_BUFFER_SIZE;Out[Op++]=(UINT8)Sym;continue;}if(Sym==256)break;if(Sym<257||Sym>285)return EFI_VOLUME_CORRUPTED;
      Len=LB[Sym-257];if(LE[Sym-257]&&(!SfbBits(&B,LE[Sym-257],&V)))return EFI_VOLUME_CORRUPTED;else if(LE[Sym-257])Len+=V;
      if(!SfbHuffGet(&B,&Dist,&DS)||DS>=30)return EFI_VOLUME_CORRUPTED;Distance=DB[DS];if(DE[DS]&&(!SfbBits(&B,DE[DS],&V)))return EFI_VOLUME_CORRUPTED;else if(DE[DS])Distance+=V;
      if(Distance>Op||Op+Len>OutBytes)return EFI_VOLUME_CORRUPTED;while(Len--){Out[Op]=Out[Op-Distance];Op++;}
    }
  }
  return Op==OutBytes?EFI_SUCCESS:EFI_VOLUME_CORRUPTED;
}

STATIC UINT32 SfbBe32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC EFI_STATUS SfbDecodePng(CONST UINT8 *Raw,UINTN N,EFI_IMAGE_OUTPUT **Out)
{
  UINTN Pos=8,W=0,H=0,IdatN=0,Row,I,Y,X,Bpp=0;UINT8 Type=0,Depth=0,*Idat=NULL,*Scan=NULL,*Pixels=NULL;UINT8 Palette[256][4];UINTN PaletteN=0;
  if(N<33||CompareMem(Raw,"\x89PNG\r\n\x1a\n",8)!=0)return EFI_UNSUPPORTED;for(I=0;I<256;I++){Palette[I][0]=Palette[I][1]=Palette[I][2]=0;Palette[I][3]=255;}
  while(Pos+12<=N){UINT32 L=SfbBe32(Raw+Pos);CONST UINT8 *T=Raw+Pos+4,*D=Raw+Pos+8;if(Pos+12+L>N)return EFI_VOLUME_CORRUPTED;
    if(CompareMem(T,"IHDR",4)==0&&L==13){W=SfbBe32(D);H=SfbBe32(D+4);Depth=D[8];Type=D[9];if(D[12]!=0)return EFI_UNSUPPORTED;}
    else if(CompareMem(T,"PLTE",4)==0&&L/3<=256){PaletteN=L/3;for(I=0;I<PaletteN;I++){Palette[I][0]=D[I*3];Palette[I][1]=D[I*3+1];Palette[I][2]=D[I*3+2];}}
    else if(CompareMem(T,"tRNS",4)==0){for(I=0;I<L&&I<256;I++)Palette[I][3]=D[I];}
    else if(CompareMem(T,"IDAT",4)==0){UINT8 *New=AllocatePool(IdatN+L);if(New==NULL){if(Idat)FreePool(Idat);return EFI_OUT_OF_RESOURCES;}if(IdatN)CopyMem(New,Idat,IdatN);CopyMem(New+IdatN,D,L);if(Idat)FreePool(Idat);Idat=New;IdatN+=L;}
    Pos+=12+L;if(CompareMem(T,"IEND",4)==0)break;
  }
  if(W==0||H==0||W>SFB_ASSET_MAX_DIM||H>SFB_ASSET_MAX_DIM||Depth!=8||Idat==NULL){if(Idat)FreePool(Idat);return EFI_UNSUPPORTED;}if(Type==2)Bpp=3;else if(Type==6)Bpp=4;else if(Type==3&&PaletteN)Bpp=1;else{FreePool(Idat);return EFI_UNSUPPORTED;}
  Row=W*Bpp;Scan=AllocatePool((Row+1)*H);Pixels=AllocatePool(Row*H);if(!Scan||!Pixels){if(Idat)FreePool(Idat);if(Scan)FreePool(Scan);if(Pixels)FreePool(Pixels);return EFI_OUT_OF_RESOURCES;}
  if(EFI_ERROR(SfbInflate(Idat,IdatN,Scan,(Row+1)*H))){FreePool(Idat);FreePool(Scan);FreePool(Pixels);return EFI_VOLUME_CORRUPTED;}FreePool(Idat);
  for(Y=0;Y<H;Y++){UINT8 F=Scan[Y*(Row+1)];UINT8 *S=Scan+Y*(Row+1)+1,*P=Pixels+Y*Row,*Prev=Y?Pixels+(Y-1)*Row:NULL;for(X=0;X<Row;X++){UINT8 A=X>=Bpp?P[X-Bpp]:0,B=Prev?Prev[X]:0,C=(Prev&&X>=Bpp)?Prev[X-Bpp]:0;UINT8 V=S[X];if(F==1)V+=A;else if(F==2)V+=B;else if(F==3)V+=(UINT8)(((UINTN)A+B)/2);else if(F==4){INTN Pa=(INTN)B-(INTN)C,Pb=(INTN)A-(INTN)C,Pc=Pa+Pb;Pa=Pa<0?-Pa:Pa;Pb=Pb<0?-Pb:Pb;Pc=Pc<0?-Pc:Pc;V+=(Pa<=Pb&&Pa<=Pc)?A:(Pb<=Pc?B:C);}else if(F!=0){FreePool(Scan);FreePool(Pixels);return EFI_UNSUPPORTED;}P[X]=V;}}
  FreePool(Scan);*Out=AllocateZeroPool(sizeof(EFI_IMAGE_OUTPUT));if(*Out==NULL){FreePool(Pixels);return EFI_OUT_OF_RESOURCES;}(*Out)->Width=(UINT16)W;(*Out)->Height=(UINT16)H;(*Out)->Image.Bitmap=AllocatePool(W*H*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));if((*Out)->Image.Bitmap==NULL){FreePool(*Out);*Out=NULL;FreePool(Pixels);return EFI_OUT_OF_RESOURCES;}
  for(I=0;I<W*H;I++){EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P=&(*Out)->Image.Bitmap[I];if(Type==3){UINT8 K=Pixels[I];if(K>=PaletteN)K=0;P->Red=Palette[K][0];P->Green=Palette[K][1];P->Blue=Palette[K][2];P->Reserved=Palette[K][3];}else{P->Red=Pixels[I*Bpp];P->Green=Pixels[I*Bpp+1];P->Blue=Pixels[I*Bpp+2];P->Reserved=Bpp==4?Pixels[I*Bpp+3]:255;}}
  FreePool(Pixels);return EFI_SUCCESS;
}

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
  Status = SfbDecodePng ((CONST UINT8 *)Raw, RawBytes, &Image);
  if (!EFI_ERROR (Status) && Image != NULL) {
    FreePool (Raw);
    Handles = NULL;
    HandleCount = 0;
    goto Decoded;
  }
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
Decoded:
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
