ENTRY(Reset_Handler)
EXTERN(_sbrk)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
}

_Min_Stack_Size = 0x1000;
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector ORIGIN(FLASH) : ALIGN(0x200)
    {
        __vector_table_start__ = .;
        KEEP(*(.isr_vector))
        __vector_table_end__ = .;
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(8);
        _sbss = .;
        __bss_start__ = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
        __bss_end__ = .;
    } > RAM

    .aymos_heap (NOLOAD) :
    {
        . = ALIGN(8);
        __aymos_heap_start__ = .;
        . = ORIGIN(RAM) + LENGTH(RAM) - _Min_Stack_Size;
        __aymos_heap_end__ = .;
    } > RAM

    __newlib_heap_start__ = __aymos_heap_start__;
    __newlib_heap_end__ = __aymos_heap_start__;
    __msp_stack_limit__ = __aymos_heap_end__;
    __msp_stack_top__ = _estack;
    _end = __newlib_heap_start__;
    end = _end;
    _img_end = __aymos_heap_start__;

    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.gnu_debuglink)
        *(.gnu.lto_*)
    }
}

ASSERT(__vector_table_start__ == ORIGIN(FLASH), "vector table must start at flash origin")
ASSERT((__vector_table_start__ & 0x1ff) == 0, "vector table must be 0x200 aligned")
ASSERT(__vector_table_end__ <= ORIGIN(FLASH) + 0x200, "F401 vector table exceeds alignment slot")
ASSERT(__aymos_heap_start__ <= __aymos_heap_end__, "RAM image overlaps MSP reservation")
ASSERT((__aymos_heap_start__ & 7) == 0, "AymOS heap must be eight-byte aligned")
ASSERT(__newlib_heap_start__ == __newlib_heap_end__, "newlib heap must remain disabled")
ASSERT(__msp_stack_top__ == ORIGIN(RAM) + LENGTH(RAM), "MSP top must equal RAM end")
