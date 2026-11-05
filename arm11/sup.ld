/* The supervisor runs right where the app copies it, entry point first, with
 * JobParams in the page before. JOB_LOAD wipes .bss; .sup_state (stacks and
 * the core's saved registers) lives outside it so the wipe can't eat them. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(job_start)

SECTIONS
{
    . = 0x25101000;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(16);
    }
    .data : {
        *(.data .data.*)
        . = ALIGN(16);
    }
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    .sup_state (NOLOAD) : {
        *(.sup_state)
    }
    __sup_end = .;
    ASSERT(__sup_end <= 0x25300000, "the supervisor does not fit below SUP_END")
    /DISCARD/ : {
        *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) *(.init_array*) *(.fini_array*)
    }
}
