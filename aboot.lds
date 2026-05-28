OUTPUT_FORMAT("elf64-alpha")
ENTRY(__start)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
SECTIONS
{
  . = 0x20000000;
  .text : { *(.text) } :text
  _etext = .;
  PROVIDE (etext = .);
  .rodata : { *(.rodata) } :text
  .data : { *(.data) } :data
  .got : { *(.got) } :data
  .sdata : { *(.sdata) } :data
  _edata = .;
  PROVIDE (edata = .);
  .sbss : { *(.sbss) *(.scommon) } :data
  .bss : { *(.bss) *(COMMON) } :data
  _end = . ;
  PROVIDE (end = .);

  /DISCARD/ : { *(.eh_frame) }

  .mdebug 0 : { *(.mdebug) }
  .note 0 : { *(.note) }
  .comment 0 : { *(.comment) }
}
