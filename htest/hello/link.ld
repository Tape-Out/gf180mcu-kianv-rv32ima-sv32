OUTPUT_ARCH("riscv")
ENTRY(_start)
MEMORY { flash (rx) : ORIGIN = 0x20100000, LENGTH = 1M }
SECTIONS {
  .text : { *(.text.start) *(.text*) *(.rodata*) *(.srodata*) } > flash
  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame) }
}
