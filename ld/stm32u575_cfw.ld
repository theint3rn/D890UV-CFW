/* D890UV custom firmware, STM32U575 (2 MB flash, 768 KB SRAM).
 *
 * Flash map (what we may and may not touch):
 *   0x08000000-0x08009FFF  stock bootloader            NOT OURS (never write)
 *   0x0800A000-0x0800A00F  activation key (chip UID)   NOT OURS
 *   0x0800C000-0x0817FFFF  APP: this image (vectors first, the bootloader jumps here)
 *   0x08180000-0x081DFFFF  cfw codeplug written by the CPS link (384 KB, app/cfwcp.c only, address-guarded)
 *   0x081E0000-0x081E1FFF  cfw settings page (8 KB, written by drivers/settings.c only)
 *   0x081E2000-0x081E3FFF  unused (emu puts its idle cave for the stock image at 0x081E3F00)
 *   0x081E4000-0x081FFFFF  stock runtime table cache   NOT OURS
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x0800C000, LENGTH = 0x08180000 - 0x0800C000   /* 0x08180000-0x081DFFFF: CPS codeplug, 384 KB (app/cfwcp.c) */
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 768K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_settings_page = 0x081E0000;

SECTIONS
{
  .isr_vector ORIGIN(FLASH) :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
    __proto_table_start = .;
    KEEP(*(.cfw_proto))
    __proto_table_end = .;
    . = ALIGN(4);
  } > FLASH

  .ARM.exidx : { *(.ARM.exidx*) } > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* not zeroed at reset: hal/safety.c fault record (survives the fault reset) */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit .noinit.*)
    . = ALIGN(4);
    _enoinit = .;
  } > RAM

  _image_end = LOADADDR(.data) + SIZEOF(.data);
  ASSERT(_image_end <= 0x08180000, "image runs into the CPS codeplug region")
  ASSERT(ORIGIN(FLASH) == 0x0800C000, "app must start at 0x0800C000")

  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
