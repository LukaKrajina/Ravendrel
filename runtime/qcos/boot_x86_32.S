# ============================================================================
#  Multiboot1 引导（32 位，QEMU 内建加载器支持）
#
#  QEMU 的 `-kernel` 内建加载器只支持 Multiboot1 + ELF32（不支持 64 位
#  Multiboot2）。故 QEMU 直启演示用 32 位内核：Multiboot 已进入 32 位保护模式，
#  只需设置栈并调用 kernel_main()（C++ shim → RVDL 的 rvdl_main）。
# ============================================================================

.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp
    call kernel_main
.halt:
    hlt
    jmp .halt
.size _start, . - _start

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top: