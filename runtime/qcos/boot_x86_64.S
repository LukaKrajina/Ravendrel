.set MB2_MAGIC,    0xE85250D6
.set MB2_ARCH, 0
.set MB2_HDR_LEN,  (mb2_end - mb2_start)
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_HDR_LEN)

.section .multiboot, "a"
.align 8
mb2_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HDR_LEN
    .long MB2_CHECKSUM
    .short 3
    .short 0
    .long 12
    .long _start
    .short 0
    .short 0
    .long 8
mb2_end:

.section .text
.code32
.global _start
_start:
    cli

    movl $pml4, %eax
    orl $0x3, %eax
    movl %eax, pml4

    movl $pdpt, %eax
    orl  $0x3, %eax
    movl %eax, pdpt

    movl $0, %ecx
    movl $0x00000083, %eax
1:
    movl %eax, pd(,%ecx,8)
    addl $0x200000, %eax
    incl %ecx
    cmpl $512, %ecx
    jne  1b

    movl $pml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr

    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    lgdt gdt_desc

    ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov %cr0, %rax
    and $~0xC, %rax
    mov %rax, %cr0
    mov %cr4, %rax
    or  $0x600, %rax
    mov %rax, %cr4
    fninit

    mov $stack_top, %rsp
    call kernel_main

.halt:
    hlt
    jmp .halt

.section .bss
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 4096

.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .data
.align 8
gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00AF92000000FFFF
gdt_end:

gdt_desc:
    .word (gdt_end - gdt - 1)
    .long gdt