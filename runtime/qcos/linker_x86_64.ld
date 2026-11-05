/* ==========================================================================
   linker_x86_64.ld —— x86_64 裸机链接脚本
   ========================================================================== */

ENTRY(_start)

SECTIONS
{
    /* 加载到 1 MB（Multiboot 约定：内核映像位于 1MB 起） */
    . = 1M;

    .multiboot : {
        KEEP(*(.multiboot))
    }

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /* 丢弃宿主编译器附带、裸机用不到的内容 */
    /DISCARD/ : {
        *(.note*)
        *(.eh_frame*)
        *(.comment*)
    }
}
