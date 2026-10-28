/* SEGA_SYS.H - BOOT ROM service routine macros. Header doc stripped
 * because the original SGL 302j header had nested slash-star sequences
 * and version-string literals that the modern SH-elf gcc parser rejects.
 */

#ifndef SEGA_SYS_H
#define SEGA_SYS_H

/***********************************************************************
 *      Include files
 ***********************************************************************/
#include "SEGA_XPT.H"

/***********************************************************************
 *      Processing macros
 ***********************************************************************/
#define SYS_SETUINT(_Num_, _Hdr_) \
        ((**(volatile void(**)(void) *)(0x6000300 + _Num_ * 4)) = (void(*)())_Hdr_)
#define SYS_GETUINT(_Num_) \
        (*(void(**)(void))(0x6000300 + _Num_ * 4))
#define SYS_SETSINT(_Num_, _Hdr_) \
        ((void (*)(void)) (*(Uint32 *)(0x6000310 + (_Num_ - 0x40) * 4) = (Uint32)_Hdr_))
#define SYS_GETSINT(_Num_) \
        ((void (*)(void)) (*(Uint32 *)(0x6000310 + (_Num_ - 0x40) * 4)))
#define SYS_TASSEM(num) \
        ((Sint32)(*((volatile Sint32 *)(0x6000340 + (num) * 4))))
#define SYS_CLRSEM(num) \
        ((void)(*((volatile Sint32 *)(0x6000340 + (num) * 4)) = (Sint32)1))
#define SYS_SETSCUIM(msk) \
        ((void)(*((volatile Sint32 *)0x6000358) = (Sint32)(msk)))
#define SYS_CHGSCUIM(enbmsk, dismsk) \
        ((void)(*((volatile Sint32 *)0x600035C) = (Sint32)(enbmsk) << 16 | (Sint32)(dismsk)))
#define SYS_GETSCUIM \
        (*((volatile Sint32 *)0x6000358))
#define SYS_CHGSYSCK(ck) \
        ((void)(*((volatile Sint8 *)0x6000380) = (Sint8)(ck)))
#define SYS_GETSYSCK \
        (*((volatile Sint8 *)0x6000380))
#define SYS_CHGUIPR(PriTab) \
        ((void)(*((volatile Sint32 *)0x600037C) = (Sint32)(PriTab)))
#define SYS_EXECDMP() \
        ((void)(*((volatile Sint8 *)0x6000374) = (Sint8)1))
#define SYS_PCLRMEM \
        (*((volatile Uint8 *)0x6000384))
#define SYS_CHKMPEG(fl) \
        ((Sint32)(*((volatile Sint32 *)0x6000378) = (Sint32)(fl)))

#endif /* SEGA_SYS_H */
