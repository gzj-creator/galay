#if defined(__aarch64__)

#define CTX_X19 0
#define CTX_X20 8
#define CTX_X21 16
#define CTX_X22 24
#define CTX_X23 32
#define CTX_X24 40
#define CTX_X25 48
#define CTX_X26 56
#define CTX_X27 64
#define CTX_X28 72
#define CTX_X29 80
#define CTX_PC  88
#define CTX_SP  96
#define CTX_D8  104
#define CTX_D9  112
#define CTX_D10 120
#define CTX_D11 128
#define CTX_D12 136
#define CTX_D13 144
#define CTX_D14 152
#define CTX_D15 160

.text
.align 2

.globl _galay_coro_context_switch
_galay_coro_context_switch:
    stp x19, x20, [x0, #CTX_X19]
    stp x21, x22, [x0, #CTX_X21]
    stp x23, x24, [x0, #CTX_X23]
    stp x25, x26, [x0, #CTX_X25]
    stp x27, x28, [x0, #CTX_X27]
    str x29, [x0, #CTX_X29]
    str x30, [x0, #CTX_PC]
    mov x2, sp
    str x2, [x0, #CTX_SP]
    str d8,  [x0, #CTX_D8]
    str d9,  [x0, #CTX_D9]
    str d10, [x0, #CTX_D10]
    str d11, [x0, #CTX_D11]
    str d12, [x0, #CTX_D12]
    str d13, [x0, #CTX_D13]
    str d14, [x0, #CTX_D14]
    str d15, [x0, #CTX_D15]

    ldr x2, [x1, #CTX_SP]
    mov sp, x2
    ldr d8,  [x1, #CTX_D8]
    ldr d9,  [x1, #CTX_D9]
    ldr d10, [x1, #CTX_D10]
    ldr d11, [x1, #CTX_D11]
    ldr d12, [x1, #CTX_D12]
    ldr d13, [x1, #CTX_D13]
    ldr d14, [x1, #CTX_D14]
    ldr d15, [x1, #CTX_D15]
    ldp x19, x20, [x1, #CTX_X19]
    ldp x21, x22, [x1, #CTX_X21]
    ldp x23, x24, [x1, #CTX_X23]
    ldp x25, x26, [x1, #CTX_X25]
    ldp x27, x28, [x1, #CTX_X27]
    ldr x29, [x1, #CTX_X29]
    ldr x30, [x1, #CTX_PC]
    ret

.globl _galay_coro_context_entry
_galay_coro_context_entry:
    mov x0, x19
    bl _galay_coro_context_trampoline
1:
    b 1b

#endif
