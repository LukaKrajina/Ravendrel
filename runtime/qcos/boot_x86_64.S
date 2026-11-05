# ============================================================================
#  Multiboot2 引导（x86_64，32 位入口切长模式）
#
#  Multiboot1 只支持 32 位；x86_64 需 Multiboot2。QEMU 的 -kernel 加载器检测
#  Multiboot2 头后，以 32 位保护模式跳转到 _start，由本文件负责：
#    1. 建立身份映射（前 1GB，2MB 大页）
#    2. 使能 PAE + 长模式（EFER.LME）+ 分页（CR0.PG）
#    3. 加载 64 位 GDT，远跳进入 64 位代码
#    4. 设置栈并调用 kernel_main()
# ============================================================================

# ---- Multiboot2 头 ----
.set MB2_MAGIC,    0xE85250D6
.set MB2_ARCH,     0                       # i386（x86_64 沿用）
.set MB2_HDR_LEN,  (mb2_end - mb2_start)
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_HDR_LEN)

.section .multiboot, "a"
.align 8
mb2_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HDR_LEN
    .long MB2_CHECKSUM
    # entry address tag（type 3）
    .short 3
    .short 0
    .long 12
    .long _start
    # end tag（type 0）
    .short 0
    .short 0
    .long 8
mb2_end:

# ---- 32 位入口 ----
.section .text
.code32
.global _start
_start:
    cli

    # 身份映射：PML4[0] -> PDPT
    movl $pml4, %eax
    orl  $0x3, %eax                 # present | writable
    movl %eax, pml4

    # PDPT[0] -> PD
    movl $pdpt, %eax
    orl  $0x3, %eax
    movl %eax, pdpt

    # PD[0..511] -> 2MB 大页（present | writable | huge）
    movl $0, %ecx
    movl $0x00000083, %eax
1:
    movl %eax, pd(,%ecx,8)
    addl $0x200000, %eax            # 下一个 2MB
    incl %ecx
    cmpl $512, %ecx
    jne  1b

    # 装载 PML4 到 CR3
    movl $pml4, %eax
    movl %eax, %cr3

    # 使能 PAE（CR4.PAE）
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    # 使能长模式（EFER.LME）
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr

    # 使能分页（CR0.PG）
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    # 加载 GDT（32 位格式）
    lgdt gdt_desc

    # 远跳进入 64 位代码（CS = 0x08）
    ljmp $0x08, $long_mode_entry

# ---- 64 位代码 ----
.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    # 使能 x87 FPU + SSE（双精度量子服务需要）：
    #   清 CR0.EM(2)/TS(3)，置 CR4.OSFXSR(9)/OSXMMEXCPT(10)，再 fninit
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

# ---- 页表（.bss，4KB 对齐）----
.section .bss
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 4096

.align 16
stack_bottom:
    .skip 16384
stack_top:

# ---- GDT ----
.section .data
.align 8
gdt:
    .quad 0x0000000000000000       # null
    .quad 0x00AF9A000000FFFF       # 64-bit code segment
    .quad 0x00AF92000000FFFF       # data segment
gdt_end:

# 32 位 lgdt 描述符：16 位 limit + 32 位 base
gdt_desc:
    .word (gdt_end - gdt - 1)
    .long gdt