#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "debug.h"
#include "box64context.h"
#include "dynarec.h"
#include "emu/x64emu_private.h"
#include "emu/x64run_private.h"
#include "x64run.h"
#include "x64emu.h"
#include "box64stack.h"
#include "callback.h"
#include "emu/x64run_private.h"
#include "x64trace.h"
#include "dynarec_native.h"
#include "../dynablock_private.h"
#include "custommem.h"

#include "la64_printer.h"
#include "dynarec_la64_private.h"
#include "dynarec_la64_functions.h"
#include "dynarec_la64_helper.h"

static uintptr_t geted_32(dynarec_la64_t* dyn, uintptr_t addr, int ninst, uint8_t nextop, uint8_t* ed, uint8_t hint, uint8_t scratch, int64_t* fixaddress, int* l, int i12);

/* setup r2 to address pointed by ED, also fixaddress is an optionnal delta in the range [-absmax, +absmax], with delta&mask==0 to be added to ed for LDR/STR */
uintptr_t geted(dynarec_la64_t* dyn, uintptr_t addr, int ninst, uint8_t nextop, uint8_t* ed, uint8_t hint, uint8_t scratch, int64_t* fixaddress, rex_t rex, int* l, int i12, int delta)
{
    MAYUSE(dyn);
    MAYUSE(ninst);
    MAYUSE(delta);

    if (rex.is32bits)
        return geted_32(dyn, addr, ninst, nextop, ed, hint, scratch, fixaddress, l, i12);

    int lock = l ? ((l == LOCK_LOCK) ? 1 : 2) : 0;
    if (lock == 2)
        *l = 0;
    uint8_t ret = x2;
    *fixaddress = 0;
    if (hint > 0) ret = hint;
    int maxval = 2047;
    if (i12 > 1)
        maxval -= i12;
    MAYUSE(scratch);
    if (!(nextop & 0xC0)) {
        if ((nextop & 7) == 4) {
            uint8_t sib = F8;
            int sib_reg = ((sib >> 3) & 7) + (rex.x << 3);
            int sib_reg2 = (sib & 0x7) + (rex.b << 3);
            if ((sib & 0x7) == 5) {
                int64_t tmp = F32S;
                if (sib_reg != 4) {
                    if (tmp && ((tmp < -2048) || (tmp > maxval) || !i12)) {
                        MOV64x(scratch, tmp);
                        ADDSL(ret, scratch, TO_LA64(sib_reg), sib >> 6, ret);
                    } else {
                        if (sib >> 6) {
                            SLLI_D(ret, TO_LA64(sib_reg), (sib >> 6));
                        } else {
                            ret = TO_LA64(sib_reg);
                        }
                        *fixaddress = tmp;
                    }
                } else {
                    switch (lock) {
                        case 1: addLockAddress(tmp); break;
                        case 2:
                            if (isLockAddress(tmp)) *l = 1;
                            break;
                    }
                    MOV64x(ret, tmp);
                }
            } else {
                if (sib_reg != 4) {
                    ADDSL(ret, TO_LA64(sib_reg2), TO_LA64(sib_reg), sib >> 6, scratch);
                } else {
                    ret = TO_LA64(sib_reg2);
                }
            }
        } else if ((nextop & 7) == 5) {
            int64_t tmp = F32S64;
            int64_t adj = dyn->last_ip ? ((addr + delta) - dyn->last_ip) : 0;
            if (i12 && adj && (tmp + adj >= -2048) && (tmp + adj <= maxval)) {
                ret = xRIP;
                *fixaddress = tmp + adj;
            } else if (i12 && (tmp >= -2048) && (tmp <= maxval)) {
                GETIP(addr + delta);
                ret = xRIP;
                *fixaddress = tmp;
            } else if (adj && (tmp + adj >= -2048) && (tmp + adj <= maxval)) {
                ADDI_D(ret, xRIP, tmp + adj);
            } else if ((tmp >= -2048) && (tmp <= maxval)) {
                GETIP(addr + delta);
                ADDI_D(ret, xRIP, tmp);
            } else if (tmp + addr + delta < 0x100000000LL) {
                MOV64x(ret, tmp + addr + delta);
            } else {
                if (adj) {
                    MOV64x(ret, tmp + adj);
                } else {
                    MOV64x(ret, tmp);
                    GETIP(addr + delta);
                }
                ADD_D(ret, ret, xRIP);
            }
            switch (lock) {
                case 1: addLockAddress(addr + delta + tmp); break;
                case 2:
                    if (isLockAddress(addr + delta + tmp)) *l = 1;
                    break;
            }
        } else {
            ret = TO_LA64((nextop & 7) + (rex.b << 3));
        }
    } else {
        int64_t i64;
        uint8_t sib = 0;
        int sib_reg = 0;
        if ((nextop & 7) == 4) {
            sib = F8;
            sib_reg = ((sib >> 3) & 7) + (rex.x << 3);
        }
        int sib_reg2 = (sib & 0x07) + (rex.b << 3);
        if (nextop & 0x80)
            i64 = F32S;
        else
            i64 = F8S;
        if (i64 == 0 || ((i64 >= -2048) && (i64 <= 2047) && i12)) {
            *fixaddress = i64;
            if ((nextop & 7) == 4) {
                if (sib_reg != 4) {
                    ADDSL(ret, TO_LA64(sib_reg2), TO_LA64(sib_reg), sib >> 6, scratch);
                } else {
                    ret = TO_LA64(sib_reg2);
                }
            } else {
                ret = TO_LA64((nextop & 0x07) + (rex.b << 3));
            }
        } else {
            if (i64 >= -2048 && i64 <= 2047) {
                if ((nextop & 7) == 4) {
                    if (sib_reg != 4) {
                        ADDSL(scratch, TO_LA64(sib_reg2), TO_LA64(sib_reg), sib >> 6, scratch);
                    } else {
                        scratch = TO_LA64(sib_reg2);
                    }
                } else {
                    scratch = TO_LA64((nextop & 0x07) + (rex.b << 3));
                }
                ADDI_D(ret, scratch, i64);
            } else {
                MOV64x(scratch, i64);
                if ((nextop & 7) == 4) {
                    if (sib_reg != 4) {
                        ADD_D(scratch, scratch, TO_LA64(sib_reg2));
                        ADDSL(ret, scratch, TO_LA64(sib_reg), sib >> 6, ret);
                    } else {
                        PASS3(int tmp = TO_LA64(sib_reg2));
                        ADD_D(ret, tmp, scratch);
                    }
                } else {
                    PASS3(int tmp = TO_LA64((nextop & 0x07) + (rex.b << 3)));
                    ADD_D(ret, tmp, scratch);
                }
            }
        }
    }
    *ed = ret;
    return addr;
}

static uintptr_t geted_32(dynarec_la64_t* dyn, uintptr_t addr, int ninst, uint8_t nextop, uint8_t* ed, uint8_t hint, uint8_t scratch, int64_t* fixaddress, int* l, int i12)
{
    MAYUSE(dyn);
    MAYUSE(ninst);

    int lock = l ? ((l == LOCK_LOCK) ? 1 : 2) : 0;
    if (lock == 2)
        *l = 0;
    uint8_t ret = x2;
    *fixaddress = 0;
    if (hint > 0) ret = hint;
    int maxval = 2047;
    if (i12 > 1)
        maxval -= i12;
    MAYUSE(scratch);
    if (!(nextop & 0xC0)) {
        if ((nextop & 7) == 4) {
            uint8_t sib = F8;
            int sib_reg = (sib >> 3) & 0x7;
            int sib_reg2 = sib & 0x7;
            if (sib_reg2 == 5) {
                int64_t tmp = F32S;
                if (sib_reg != 4) {
                    if (tmp && ((tmp < -2048) || (tmp > maxval) || !i12)) {
                        MOV32w(scratch, tmp);
                        if ((sib >> 6)) {
                            SLLI_D(ret, TO_LA64(sib_reg), sib >> 6);
                            ADD_W(ret, ret, scratch);
                        } else {
                            ADD_W(ret, TO_LA64(sib_reg), scratch);
                        }
                    } else {
                        if (sib >> 6) {
                            SLLI_D(ret, TO_LA64(sib_reg), (sib >> 6));
                        } else {
                            ret = TO_LA64(sib_reg);
                        }
                        *fixaddress = tmp;
                    }
                } else {
                    switch (lock) {
                        case 1: addLockAddress((int32_t)tmp); break;
                        case 2:
                            if (isLockAddress((int32_t)tmp)) *l = 1;
                            break;
                    }
                    MOV32w(ret, tmp);
                }
            } else {
                if (sib_reg != 4) {
                    if ((sib >> 6)) {
                        SLLI_D(ret, TO_LA64(sib_reg), (sib >> 6));
                        ADD_W(ret, ret, TO_LA64(sib_reg2));
                    } else {
                        ADD_W(ret, TO_LA64(sib_reg2), TO_LA64(sib_reg));
                    }
                } else {
                    ret = TO_LA64(sib_reg2);
                }
            }
        } else if ((nextop & 7) == 5) {
            uint32_t tmp = F32;
            MOV32w(ret, tmp);
            switch (lock) {
                case 1: addLockAddress(tmp); break;
                case 2:
                    if (isLockAddress(tmp)) *l = 1;
                    break;
            }
        } else {
            ret = TO_LA64((nextop & 7));
            if (ret == hint) {
                AND(hint, ret, xMASK); // to clear upper part
            }
        }
    } else {
        int64_t i32;
        uint8_t sib = 0;
        int sib_reg = 0;
        if ((nextop & 7) == 4) {
            sib = F8;
            sib_reg = (sib >> 3) & 7;
        }
        int sib_reg2 = sib & 0x07;
        if (nextop & 0x80)
            i32 = F32S;
        else
            i32 = F8S;
        if (i32 == 0 || ((i32 >= -2048) && (i32 <= 2047) && i12)) {
            *fixaddress = i32;
            if ((nextop & 7) == 4) {
                if (sib_reg != 4) {
                    if (sib >> 6) {
                        SLLI_D(ret, TO_LA64(sib_reg), (sib >> 6));
                        ADD_W(ret, ret, TO_LA64(sib_reg2));
                    } else {
                        ADD_W(ret, TO_LA64(sib_reg2), TO_LA64(sib_reg));
                    }
                } else {
                    ret = TO_LA64(sib_reg2);
                }
            } else {
                ret = TO_LA64((nextop & 0x07));
            }
        } else {
            if (i32 >= -2048 && i32 <= 2047) {
                if ((nextop & 7) == 4) {
                    if (sib_reg != 4) {
                        if (sib >> 6) {
                            SLLI_D(scratch, TO_LA64(sib_reg), sib >> 6);
                            ADD_W(scratch, scratch, TO_LA64(sib_reg2));
                        } else {
                            ADD_W(scratch, TO_LA64(sib_reg2), TO_LA64(sib_reg));
                        }
                    } else {
                        scratch = TO_LA64(sib_reg2);
                    }
                } else {
                    scratch = TO_LA64((nextop & 0x07));
                }
                ADDI_W(ret, scratch, i32);
            } else {
                MOV32w(scratch, i32);
                if ((nextop & 7) == 4) {
                    if (sib_reg != 4) {
                        ADD_W(scratch, scratch, TO_LA64(sib_reg2));
                        if (sib >> 6) {
                            SLLI_D(ret, TO_LA64(sib_reg), (sib >> 6));
                            ADD_W(ret, ret, scratch);
                        } else {
                            ADD_W(ret, scratch, TO_LA64(sib_reg));
                        }
                    } else {
                        PASS3(int tmp = TO_LA64(sib_reg2));
                        ADD_W(ret, tmp, scratch);
                    }
                } else {
                    PASS3(int tmp = TO_LA64((nextop & 0x07)));
                    ADD_W(ret, tmp, scratch);
                }
            }
        }
    }
    *ed = ret;
    return addr;
}

void jump_to_epilog(dynarec_la64_t* dyn, uintptr_t ip, int reg, int ninst)
{
    MAYUSE(dyn);
    MAYUSE(ip);
    MAYUSE(ninst);
    MESSAGE(LOG_DUMP, "Jump to epilog\n");

    if (reg) {
        if (reg != xRIP) {
            MV(xRIP, reg);
        }
    } else {
        GETIP_(ip);
    }
    TABLE64(x2, (uintptr_t)la64_epilog);
    SMEND();
    BR(x2);
}

void jump_to_epilog_fast(dynarec_la64_t* dyn, uintptr_t ip, int reg, int ninst)
{
    MAYUSE(dyn);
    MAYUSE(ip);
    MAYUSE(ninst);
    MESSAGE(LOG_DUMP, "Jump to epilog\n");

    if (reg) {
        if (reg != xRIP) {
            MV(xRIP, reg);
        }
    } else {
        GETIP_(ip);
    }
    TABLE64(x2, (uintptr_t)la64_epilog_fast);
    SMEND();
    BR(x2);
}

void jump_to_next(dynarec_la64_t* dyn, uintptr_t ip, int reg, int ninst, int is32bits)
{
    MAYUSE(dyn);
    MAYUSE(ninst);
    MESSAGE(LOG_DUMP, "Jump to next\n");

    if (reg) {
        if (reg != xRIP) {
            MV(xRIP, reg);
        }
        NOTEST(x2);
        uintptr_t tbl = is32bits ? getJumpTable32() : getJumpTable64();
        MAYUSE(tbl);
        TABLE64(x3, tbl);
        if (!is32bits) {
            BSTRPICK_D(x2, xRIP, JMPTABL_START3 + JMPTABL_SHIFT3 - 1, JMPTABL_START3);
            ALSL_D(x3, x2, x3, 3);
            LD_D(x3, x3, 0);
        }
        BSTRPICK_D(x2, xRIP, JMPTABL_START2 + JMPTABL_SHIFT2 - 1, JMPTABL_START2);
        ALSL_D(x3, x2, x3, 3);
        LD_D(x3, x3, 0);
        BSTRPICK_D(x2, xRIP, JMPTABL_START1 + JMPTABL_SHIFT1 - 1, JMPTABL_START1);
        ALSL_D(x3, x2, x3, 3);
        LD_D(x3, x3, 0);
        BSTRPICK_D(x2, xRIP, JMPTABL_START0 + JMPTABL_SHIFT0 - 1, JMPTABL_START0);
        ALSL_D(x3, x2, x3, 3);
        LD_D(x2, x3, 0);
    } else {
        NOTEST(x2);
        uintptr_t p = getJumpTableAddress64(ip);
        MAYUSE(p);
        TABLE64(x3, p);
        GETIP_(ip);
        LD_D(x2, x3, 0); // LR_D(x2, x3, 1, 1);
    }
    if (reg != x1) {
        MV(x1, xRIP);
    }
    CLEARIP();
#ifdef HAVE_TRACE
// MOVx(x3, 15);    no access to PC reg
#endif
    SMEND();
    JIRL(xRA, x2, 0x0); // save LR...
}

void ret_to_epilog(dynarec_la64_t* dyn, int ninst, rex_t rex)
{
    MAYUSE(dyn);
    MAYUSE(ninst);
    MESSAGE(LOG_DUMP, "Ret to epilog\n");
    POP1z(xRIP);
    MVz(x1, xRIP);
    SMEND();
    if (box64_dynarec_callret) {
        // pop the actual return address from RV64 stack
        LD_D(x2, xSP, 0);     // native addr
        LD_D(x6, xSP, 8);     // x86 addr
        ADDI_D(xSP, xSP, 16); // pop
        BNE(x6, xRIP, 2 * 4); // is it the right address?
        BR(x2);
        // not the correct return address, regular jump, but purge the stack first, it's unsync now...
        ADDI_D(xSP, xSavedSP, -16);
    }

    uintptr_t tbl = rex.is32bits ? getJumpTable32() : getJumpTable64();
    MOV64x(x3, tbl);
    if (!rex.is32bits) {
        BSTRPICK_D(x2, xRIP, JMPTABL_START3 + JMPTABL_SHIFT3 - 1, JMPTABL_START3);
        ALSL_D(x3, x2, x3, 3);
        LD_D(x3, x3, 0);
    }
    BSTRPICK_D(x2, xRIP, JMPTABL_START2 + JMPTABL_SHIFT2 - 1, JMPTABL_START2);
    ALSL_D(x3, x2, x3, 3);
    LD_D(x3, x3, 0);
    BSTRPICK_D(x2, xRIP, JMPTABL_START1 + JMPTABL_SHIFT1 - 1, JMPTABL_START1);
    ALSL_D(x3, x2, x3, 3);
    LD_D(x3, x3, 0);
    BSTRPICK_D(x2, xRIP, JMPTABL_START0 + JMPTABL_SHIFT0 - 1, JMPTABL_START0);
    ALSL_D(x3, x2, x3, 3);
    LD_D(x2, x3, 0);
    BR(x2); // save LR
    CLEARIP();
}

void call_c(dynarec_la64_t* dyn, int ninst, void* fnc, int reg, int ret, int saveflags, int savereg)
{
    MAYUSE(fnc);
    if (savereg == 0)
        savereg = x6;
    if (saveflags) {
        RESTORE_EFLAGS(reg);
        ST_D(xFlags, xEmu, offsetof(x64emu_t, eflags));
    }
    fpu_pushcache(dyn, ninst, reg, 0);
    if (ret != -2) {
        ADDI_D(xSP, xSP, -16); // RV64 stack needs to be 16byte aligned
        ST_D(xEmu, xSP, 0);
        ST_D(savereg, xSP, 8);
        // $r4..$r20 needs to be saved by caller
        STORE_REG(RAX);
        STORE_REG(RCX);
        STORE_REG(RDX);
        STORE_REG(RBX);
        STORE_REG(RSP);
        STORE_REG(RBP);
        STORE_REG(RSI);
        STORE_REG(RDI);
        ST_D(xRIP, xEmu, offsetof(x64emu_t, ip));
    }
    TABLE64(reg, (uintptr_t)fnc);
    JIRL(xRA, reg, 0);
    if (ret >= 0) {
        MV(ret, xEmu);
    }
    if (ret != -2) {
        LD_D(xEmu, xSP, 0);
        LD_D(savereg, xSP, 8);
        ADDI_D(xSP, xSP, 16);
#define GO(A) \
    if (ret != x##A) { LOAD_REG(A); }
        GO(RAX);
        GO(RCX);
        GO(RDX);
        GO(RBX);
        GO(RSP);
        GO(RBP);
        GO(RSI);
        GO(RDI);
        if (ret != xRIP)
            LD_D(xRIP, xEmu, offsetof(x64emu_t, ip));
#undef GO
    }
    // regenerate mask
    ADDI_W(xMASK, xZR, -1);
    LU32I_D(xMASK, 0);

    fpu_popcache(dyn, ninst, reg, 0);
    if (saveflags) {
        LD_D(xFlags, xEmu, offsetof(x64emu_t, eflags));
        SPILL_EFLAGS();
    }
    SET_NODF();
    dyn->last_ip = 0;
}

void x87_forget(dynarec_la64_t* dyn, int ninst, int s1, int s2, int st)
{
    // TODO
}

// purge the SSE cache for XMM0..XMM7 (to use before function native call)
void sse_purge07cache(dynarec_la64_t* dyn, int ninst, int s1)
{
    // TODO
}

void fpu_pushcache(dynarec_la64_t* dyn, int ninst, int s1, int not07)
{
    // TODO
}

void fpu_popcache(dynarec_la64_t* dyn, int ninst, int s1, int not07)
{
    // TODO
}

void fpu_purgecache(dynarec_la64_t* dyn, int ninst, int next, int s1, int s2, int s3)
{
    // TODO
}

void fpu_reflectcache(dynarec_la64_t* dyn, int ninst, int s1, int s2, int s3)
{
    // TODO
}

void fpu_unreflectcache(dynarec_la64_t* dyn, int ninst, int s1, int s2, int s3)
{
    // TODO
}

void emit_pf(dynarec_la64_t* dyn, int ninst, int s1, int s3, int s4)
{
    MAYUSE(dyn);
    MAYUSE(ninst);
    // PF: (((emu->x64emu_parity_tab[(res&0xff) / 32] >> ((res&0xff) % 32)) & 1) == 0)
    MOV64x(s4, (uintptr_t)GetParityTab());
    SRLI_D(s3, s1, 3);
    ANDI(s3, s3, 28);
    ADD_D(s4, s4, s3);
    LD_W(s4, s4, 0);
    NOR(s4, xZR, s4);
    SRL_W(s4, s4, s1);
    ANDI(s4, s4, 1);

    BEQZ(s4, 8);
    ORI(xFlags, xFlags, 1 << F_PF);
}

void fpu_reset_cache(dynarec_la64_t* dyn, int ninst, int reset_n)
{
    // TODO
}

// propagate ST stack state, especial stack pop that are deferred
void fpu_propagate_stack(dynarec_la64_t* dyn, int ninst)
{
    // TODO
}


static void fpuCacheTransform(dynarec_la64_t* dyn, int ninst, int s1, int s2, int s3)
{
    // TODO
}

static void flagsCacheTransform(dynarec_la64_t* dyn, int ninst, int s1)
{
#if STEP > 1
    int j64;
    int jmp = dyn->insts[ninst].x64.jmp_insts;
    if(jmp<0)
        return;
    if(dyn->f.dfnone)  // flags are fully known, nothing we can do more
        return;
    MESSAGE(LOG_DUMP, "\tFlags fetch ---- ninst=%d -> %d\n", ninst, jmp);
    int go = 0;
    switch (dyn->insts[jmp].f_entry.pending) {
        case SF_UNKNOWN: break;
        case SF_SET:
            if(dyn->f.pending!=SF_SET && dyn->f.pending!=SF_SET_PENDING)
                go = 1;
            break;
        case SF_SET_PENDING:
            if(dyn->f.pending!=SF_SET
            && dyn->f.pending!=SF_SET_PENDING
            && dyn->f.pending!=SF_PENDING)
                go = 1;
            break;
        case SF_PENDING:
            if(dyn->f.pending!=SF_SET
            && dyn->f.pending!=SF_SET_PENDING
            && dyn->f.pending!=SF_PENDING)
                go = 1;
            else
                go = (dyn->insts[jmp].f_entry.dfnone  == dyn->f.dfnone)?0:1;
            break;
    }
    if(dyn->insts[jmp].f_entry.dfnone && !dyn->f.dfnone)
        go = 1;
    if(go) {
        if(dyn->f.pending!=SF_PENDING) {
            LD_W(s1, xEmu, offsetof(x64emu_t, df));
            j64 = (GETMARKF2)-(dyn->native_size);
            BEQZ(s1, j64);
        }
        CALL_(UpdateFlags, -1, 0);
        MARKF2;
    }
#endif
}

void CacheTransform(dynarec_la64_t* dyn, int ninst, int cacheupd, int s1, int s2, int s3) {
    if(cacheupd&2)
        fpuCacheTransform(dyn, ninst, s1, s2, s3);
    if(cacheupd&1)
        flagsCacheTransform(dyn, ninst, s1);
}
