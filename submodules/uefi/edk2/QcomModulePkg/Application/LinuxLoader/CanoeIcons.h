/** Compact 24-unit outline icons adapted from Lucide Icons.
 * Copyright (c) 2020 Lucide Contributors, ISC license.
 * {-1,-1} starts a new subpath; source curves are flattened into segments.
 */
#ifndef __CANOE_ICONS_H__
#define __CANOE_ICONS_H__
typedef struct { INT8 X; INT8 Y; } CANOE_ICON_POINT;
typedef struct { CONST CANOE_ICON_POINT *Points; UINTN Count; } CANOE_ICON_PATH;
#define P(x,y) {(x),(y)}
#define B P(-1,-1)
STATIC CONST CANOE_ICON_POINT mIconBoot[]={P(5,3),P(19,12),P(5,21),P(5,3)};
STATIC CONST CANOE_ICON_POINT mIconUsb[]={P(12,3),P(12,17),P(9,20),P(6,17),B,P(12,8),P(17,5),B,P(17,5),P(15,5),B,P(17,5),P(17,7),B,P(12,12),P(7,9),B,P(7,9),P(5,9),P(5,11),P(7,11),P(7,9),B,P(12,17),P(12,21)};
STATIC CONST CANOE_ICON_POINT mIconFile[]={P(3,7),P(9,7),P(11,4),P(21,4),P(21,19),P(3,19),P(3,7)};
STATIC CONST CANOE_ICON_POINT mIconSettings[]={P(4,6),P(20,6),B,P(4,12),P(20,12),B,P(4,18),P(20,18),B,P(8,4),P(8,8),B,P(16,10),P(16,14),B,P(10,16),P(10,20)};
STATIC CONST CANOE_ICON_POINT mIconPower[]={P(12,2),P(12,12),B,P(7,4),P(4,7),P(3,11),P(4,16),P(7,19),P(12,21),P(17,19),P(20,16),P(21,11),P(20,7),P(17,4)};
STATIC CONST CANOE_ICON_POINT mIconRestart[]={P(20,7),P(20,3),P(16,3),B,P(20,3),P(17,6),P(14,4),P(10,3),P(6,5),P(3,9),P(3,14),P(6,18),P(10,21),P(15,20),P(19,17),P(21,13)};
STATIC CONST CANOE_ICON_POINT mIconBack[]={P(15,18),P(9,12),P(15,6),B,P(9,12),P(21,12)};
STATIC CONST CANOE_ICON_POINT mIconLock[]={P(5,10),P(19,10),P(19,21),P(5,21),P(5,10),B,P(8,10),P(8,7),P(9,4),P(12,3),P(15,4),P(16,7),P(16,10),B,P(12,14),P(12,17)};
STATIC CONST CANOE_ICON_POINT mIconPalette[]={P(12,3),P(7,4),P(4,7),P(3,12),P(5,17),P(9,20),P(13,21),P(15,20),P(15,17),P(17,15),P(20,15),P(21,12),P(20,8),P(17,5),P(12,3),B,P(8,9),P(8,9),B,P(12,7),P(12,7),B,P(16,9),P(16,9),B,P(7,13),P(7,13)};
STATIC CONST CANOE_ICON_POINT mIconLanguage[]={P(4,5),P(14,5),B,P(9,3),P(9,5),B,P(6,8),P(12,14),B,P(12,8),P(6,14),B,P(15,21),P(19,11),P(23,21),B,P(16,18),P(22,18)};
STATIC CONST CANOE_ICON_POINT mIconPin[]={P(9,3),P(15,3),P(16,8),P(19,11),P(5,11),P(8,8),P(9,3),B,P(12,11),P(12,21)};
STATIC CONST CANOE_ICON_POINT mIconTool[]={P(14,6),P(18,2),P(22,6),P(18,10),B,P(14,6),P(4,20),P(2,18),P(16,4),B,P(13,13),P(20,20)};
STATIC CONST CANOE_ICON_POINT mIconGame[]={P(8,7),P(16,7),P(20,10),P(22,17),P(20,20),P(17,20),P(14,16),P(10,16),P(7,20),P(4,20),P(2,17),P(4,10),P(8,7),B,P(7,11),P(7,15),B,P(5,13),P(9,13),B,P(17,12),P(17,12),B,P(19,15),P(19,15)};
STATIC CONST CANOE_ICON_POINT mIconInfo[]={P(12,3),P(7,4),P(4,7),P(3,12),P(4,17),P(7,20),P(12,21),P(17,20),P(20,17),P(21,12),P(20,7),P(17,4),P(12,3),B,P(12,10),P(12,17),B,P(12,7),P(12,7)};
STATIC CONST CANOE_ICON_POINT mIconWarning[]={P(12,3),P(22,20),P(2,20),P(12,3),B,P(12,9),P(12,14),B,P(12,17),P(12,17)};
#define I(n) {n,ARRAY_SIZE(n)}
STATIC CONST CANOE_ICON_PATH mCanoeIconPaths[]={{NULL,0},I(mIconBoot),I(mIconUsb),I(mIconFile),I(mIconSettings),I(mIconPower),I(mIconRestart),I(mIconBack),I(mIconLock),I(mIconPalette),I(mIconLanguage),I(mIconPin),I(mIconTool),I(mIconGame),I(mIconInfo),I(mIconWarning)};
#undef I
#undef B
#undef P
#endif
