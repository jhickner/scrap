#ifndef GLYPHS_H
#define GLYPHS_H

#define CP_SPACE 0x20u
#define CP_H 0x2500u
#define CP_V 0x2502u
#define CP_TL 0x250Cu
#define CP_TR 0x2510u
#define CP_BL 0x2514u
#define CP_BR 0x2518u
#define CP_TEE_L 0x251Cu
#define CP_TEE_R 0x2524u
#define CP_TEE_D 0x252Cu
#define CP_TEE_U 0x2534u
#define CP_CROSS 0x253Cu
#define CP_H_DOT 0x254Cu
#define CP_V_DOT 0x254Eu
#define CP_H_THICK 0x2501u
#define CP_V_THICK 0x2503u
#define CP_TL_THICK 0x250Fu
#define CP_TR_THICK 0x2513u
#define CP_BL_THICK 0x2517u
#define CP_BR_THICK 0x251Bu
#define CP_TEE_L_THICK 0x2523u
#define CP_TEE_R_THICK 0x252Bu
#define CP_TEE_D_THICK 0x2533u
#define CP_TEE_U_THICK 0x253Bu
#define CP_CROSS_THICK 0x254Bu
#define CP_RTL 0x256Du
#define CP_RTR 0x256Eu
#define CP_RBL 0x2570u
#define CP_RBR 0x256Fu
#define CP_ARR_DOWN 0x25BCu
#define CP_ARR_UP 0x25B2u
#define CP_ARR_DOWN_OPEN 0x25BDu
#define CP_ARR_UP_OPEN 0x25B3u
#define CP_ARR_RIGHT 0x25B6u
#define CP_ARR_LEFT 0x25C4u
#define CP_ARR_RIGHT_OPEN 0x25B7u
#define CP_ARR_LEFT_OPEN 0x25C1u
#define CP_DOT 0x25CFu
#define CP_CROSSX 0x00D7u
#define CP_DIAMOND_FILL 0x25C6u
#define CP_DIAMOND_OPEN 0x25C7u
#define CP_ELLIPSIS 0x2026u
#define CP_GUILL_L 0x00ABu
#define CP_GUILL_R 0x00BBu

#define BIT_U 1u
#define BIT_D 2u
#define BIT_L 4u
#define BIT_R 8u

#define STY_DOT 1u
#define STY_THICK 2u
#define STY_SOLID 4u

typedef enum { CLS_EMPTY, CLS_BORDER, CLS_TEXT, CLS_EDGE, CLS_EDGE_LABEL } Cls;

#endif
