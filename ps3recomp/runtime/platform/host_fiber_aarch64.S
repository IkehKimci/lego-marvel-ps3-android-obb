// AArch64, AAPCS64: x19-x29, SP, low v8-v15.
// Supports Apple AArch64 (Mach-O) and Android/Linux AArch64 (ELF).
// x18 is reserved. FPCR is global, FPSR caller-saved. Only the context record
// is written, preserving both red zones. SME/MTE stacks are not qualified.
#include "host_fiber_ctx.h"
#if !defined(__aarch64__) || (!defined(__APPLE__) && !defined(__linux__) && !defined(__ANDROID__))
#error "host_fiber_aarch64.S implements Apple and Linux/Android AArch64 ABIs only"
#endif
#if defined(__arm64e__)
#error "arm64e pointer-authenticated returns are not qualified"
#endif
#if defined(__APPLE__)
#define HF_SYM(name) _##name
#define HF_HIDDEN(name) .private_extern _##name
#define HF_TYPE_FUNC(name)
#else
#define HF_SYM(name) name
#define HF_HIDDEN(name) .hidden name
#define HF_TYPE_FUNC(name) .type name, %function
#endif
.text
.p2align 4
.globl HF_SYM(ps3_host_ctx_switch)
HF_HIDDEN(ps3_host_ctx_switch)
HF_TYPE_FUNC(ps3_host_ctx_switch)
HF_SYM(ps3_host_ctx_switch):
.cfi_startproc
    stp x19, x20, [x0, #HOST_CTX_X19]
    stp x21, x22, [x0, #HOST_CTX_X21]
    stp x23, x24, [x0, #HOST_CTX_X23]
    stp x25, x26, [x0, #HOST_CTX_X25]
    stp x27, x28, [x0, #HOST_CTX_X27]
    stp x29, x30, [x0, #HOST_CTX_FP]
    mov x9, sp
    str x9, [x0, #HOST_CTX_SP]
    stp d8, d9, [x0, #HOST_CTX_D8]
    stp d10, d11, [x0, #HOST_CTX_D10]
    stp d12, d13, [x0, #HOST_CTX_D12]
    stp d14, d15, [x0, #HOST_CTX_D14]
    ldp x19, x20, [x1, #HOST_CTX_X19]
    ldp x21, x22, [x1, #HOST_CTX_X21]
    ldp x23, x24, [x1, #HOST_CTX_X23]
    ldp x25, x26, [x1, #HOST_CTX_X25]
    ldp x27, x28, [x1, #HOST_CTX_X27]
    ldp x29, x30, [x1, #HOST_CTX_FP]
    ldp d8, d9, [x1, #HOST_CTX_D8]
    ldp d10, d11, [x1, #HOST_CTX_D10]
    ldp d12, d13, [x1, #HOST_CTX_D12]
    ldp d14, d15, [x1, #HOST_CTX_D14]
    ldr x9, [x1, #HOST_CTX_SP]
    mov sp, x9
    ret
.cfi_endproc

// New contexts have x19=f and x29 pointing at a zero frame record.
.p2align 2
.globl HF_SYM(ps3_host_fiber_trampoline)
HF_HIDDEN(ps3_host_fiber_trampoline)
HF_TYPE_FUNC(ps3_host_fiber_trampoline)
HF_SYM(ps3_host_fiber_trampoline):
.cfi_startproc
.cfi_undefined x30
    mov x0, x19
    bl HF_SYM(ps3_host_fiber_start)
    brk #0xf1be
.cfi_endproc
#if !defined(__APPLE__)
.section .note.GNU-stack,"",%progbits
#endif
