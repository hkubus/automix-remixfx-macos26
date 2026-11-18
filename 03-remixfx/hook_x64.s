# hook_x64.s — shared entry for planner27.c's function-entry hooks (x86_64).
#
# Each hooked function's first bytes are replaced by `jmp [rip+0] -> stub`. The per-hook stub does
#     movabs r11, <p27_hook*> ; jmp [rip+0] -> p27_hook_common
# so on arrival here: rsp -> return address into the hooked function's caller, r11 = hook descriptor,
# every argument register (integer, xmm0-7, Swift's r12/r13/r14) still holds the caller's values.
#
# We spill everything into a p27_ctx on the stack, call p27_dispatch(ctx, hook), reload the (possibly
# modified) registers, then either
#     eax == 0: jump to hook->resume (the displaced prologue + jmp back into the function), or
#     eax != 0: return straight to the caller (the handler has produced the function's result itself).
#
# p27_ctx layout (must match planner27.c):
#   0x00 rax 0x08 rbx 0x10 rcx 0x18 rdx 0x20 rsi 0x28 rdi 0x30 rbp 0x38 r8 0x40 r9 0x48 r10 0x50 r11
#   0x58 r12 0x60 r13 0x68 r14 0x70 r15 0x78 rsp-at-entry (points at the return address)
#   0x80..0xff xmm0..xmm7
# Frame is 0x108 bytes: entry rsp ≡ 8 (mod 16), so rsp ≡ 0 at the call below, as the SysV ABI requires.

    .intel_syntax noprefix
    .text
    .globl _p27_hook_common
    .p2align 4
_p27_hook_common:
    sub     rsp, 0x108
    mov     qword ptr [rsp + 0x00], rax
    mov     qword ptr [rsp + 0x08], rbx
    mov     qword ptr [rsp + 0x10], rcx
    mov     qword ptr [rsp + 0x18], rdx
    mov     qword ptr [rsp + 0x20], rsi
    mov     qword ptr [rsp + 0x28], rdi
    mov     qword ptr [rsp + 0x30], rbp
    mov     qword ptr [rsp + 0x38], r8
    mov     qword ptr [rsp + 0x40], r9
    mov     qword ptr [rsp + 0x48], r10
    mov     qword ptr [rsp + 0x50], r11
    mov     qword ptr [rsp + 0x58], r12
    mov     qword ptr [rsp + 0x60], r13
    mov     qword ptr [rsp + 0x68], r14
    mov     qword ptr [rsp + 0x70], r15
    lea     rax, [rsp + 0x108]
    mov     qword ptr [rsp + 0x78], rax
    movdqu  xmmword ptr [rsp + 0x80], xmm0
    movdqu  xmmword ptr [rsp + 0x90], xmm1
    movdqu  xmmword ptr [rsp + 0xa0], xmm2
    movdqu  xmmword ptr [rsp + 0xb0], xmm3
    movdqu  xmmword ptr [rsp + 0xc0], xmm4
    movdqu  xmmword ptr [rsp + 0xd0], xmm5
    movdqu  xmmword ptr [rsp + 0xe0], xmm6
    movdqu  xmmword ptr [rsp + 0xf0], xmm7
    mov     rdi, rsp
    mov     rsi, r11
    cld
    call    _p27_dispatch
    movdqu  xmm0, xmmword ptr [rsp + 0x80]
    movdqu  xmm1, xmmword ptr [rsp + 0x90]
    movdqu  xmm2, xmmword ptr [rsp + 0xa0]
    movdqu  xmm3, xmmword ptr [rsp + 0xb0]
    movdqu  xmm4, xmmword ptr [rsp + 0xc0]
    movdqu  xmm5, xmmword ptr [rsp + 0xd0]
    movdqu  xmm6, xmmword ptr [rsp + 0xe0]
    movdqu  xmm7, xmmword ptr [rsp + 0xf0]
    mov     rbx, qword ptr [rsp + 0x08]
    mov     rcx, qword ptr [rsp + 0x10]
    mov     rdx, qword ptr [rsp + 0x18]
    mov     rsi, qword ptr [rsp + 0x20]
    mov     rdi, qword ptr [rsp + 0x28]
    mov     rbp, qword ptr [rsp + 0x30]
    mov     r8,  qword ptr [rsp + 0x38]
    mov     r9,  qword ptr [rsp + 0x40]
    mov     r10, qword ptr [rsp + 0x48]
    mov     r12, qword ptr [rsp + 0x58]
    mov     r13, qword ptr [rsp + 0x60]
    mov     r14, qword ptr [rsp + 0x68]
    mov     r15, qword ptr [rsp + 0x70]
    mov     r11, qword ptr [rsp + 0x50]
    test    eax, eax
    mov     rax, qword ptr [rsp + 0x00]
    jnz     Lp27_return
    mov     r11, qword ptr [r11]
    add     rsp, 0x108
    jmp     r11
Lp27_return:
    add     rsp, 0x108
    ret

# uint64_t p27_swiftcall(const void* fn, void* self, void* indirect, uint64_t a0, uint64_t a1)
# Calls a Swift function of 26's planner that takes `self` in r13 and writes its result through rax
# (indirect return), with optional plain arguments in rdi/rsi. r12 (swifterror) starts at 0.
    .globl _p27_swiftcall
    .p2align 4
_p27_swiftcall:
    push    rbp
    mov     rbp, rsp
    push    r13
    push    r12
    mov     r11, rdi
    mov     r13, rsi
    mov     rax, rdx
    mov     rdi, rcx
    mov     rsi, r8
    xor     r12d, r12d
    call    r11
    pop     r12
    pop     r13
    pop     rbp
    ret
