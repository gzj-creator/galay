#if defined(__x86_64__) && defined(__linux__)

#define CTX_RBX 0
#define CTX_RBP 8
#define CTX_R12 16
#define CTX_R13 24
#define CTX_R14 32
#define CTX_R15 40
#define CTX_RIP 48
#define CTX_RSP 56

.text
.p2align 4

.globl galay_coro_context_switch
.type galay_coro_context_switch, @function
galay_coro_context_switch:
    movq %rbx, CTX_RBX(%rdi)
    movq %rbp, CTX_RBP(%rdi)
    movq %r12, CTX_R12(%rdi)
    movq %r13, CTX_R13(%rdi)
    movq %r14, CTX_R14(%rdi)
    movq %r15, CTX_R15(%rdi)
    leaq .Lgalay_coro_context_resume(%rip), %rax
    movq %rax, CTX_RIP(%rdi)
    movq %rsp, CTX_RSP(%rdi)

    movq CTX_RBX(%rsi), %rbx
    movq CTX_RBP(%rsi), %rbp
    movq CTX_R12(%rsi), %r12
    movq CTX_R13(%rsi), %r13
    movq CTX_R14(%rsi), %r14
    movq CTX_R15(%rsi), %r15
    movq CTX_RSP(%rsi), %rsp
    movq CTX_RIP(%rsi), %rax
    jmp *%rax

.Lgalay_coro_context_resume:
    ret
.size galay_coro_context_switch, .-galay_coro_context_switch

.globl galay_coro_context_entry
.type galay_coro_context_entry, @function
galay_coro_context_entry:
    movq %r12, %rdi
    call galay_coro_context_trampoline
.Lgalay_coro_context_entry_spin:
    jmp .Lgalay_coro_context_entry_spin
.size galay_coro_context_entry, .-galay_coro_context_entry

.section .note.GNU-stack,"",@progbits

#endif
