    .section .multiboot, "a"
    .align 4
    .long 0x1BADB002
    .long 0x00000003
    .long -(0x1BADB002 + 0x00000003)

    .section .text
    .globl _start
_start:
    cli
    movl $stack_top, %esp
    pushl %ebx
    pushl %eax
    call kmain
1:  hlt
    jmp 1b

    .section .bss
    .align 16
stack_bottom:
    .skip 65536
stack_top:
