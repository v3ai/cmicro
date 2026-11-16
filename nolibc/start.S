/*
 * start.S - process entry point for Linux x86-64 (no C runtime).
 *
 * The kernel enters _start with rsp pointing at:
 *     argc, argv[0..argc-1], NULL, envp[...], NULL, auxv...
 * We pass that pointer to __nolibc_start (in C), with the stack aligned to
 * 16 bytes as the System V ABI requires at a call.
 */
    .text
    .globl _start
    .type _start, @function
_start:
    xorl    %ebp, %ebp          /* mark the outermost stack frame */
    movq    %rsp, %rdi          /* argument: pointer to argc */
    andq    $-16, %rsp          /* align the stack */
    call    __nolibc_start      /* never returns */
    hlt

/* Signal handlers return here: the kernel needs rt_sigreturn (15) to
   restore the interrupted context. Installed as sa_restorer. */
    .globl __nolibc_restorer
    .type __nolibc_restorer, @function
__nolibc_restorer:
    movl    $15, %eax
    syscall
    hlt

    .section .note.GNU-stack, "", @progbits
