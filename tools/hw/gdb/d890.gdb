# GDB setup for the D890UV (STM32U575) cfw.  Usually: make gdb [GDBPORT=2331]   (in the cfw dir, after make DEBUG=1)
# or: gdb-multiarch -x tools/hw/gdb/d890.gdb -ex 'target extended-remote :3333' cfw/build_dbg/cfw.elf
# Server first (nothing here starts one):  python3 tools/hw/hw.py --radio d890 --probe jlink|stlink server
#   OpenOCD -> :3333, JLinkGDBServer -> :2331.  This only ATTACHES: it never resets on connect, never loads.
set pagination off
set confirm off
set print pretty on
set mem inaccessible-by-default off
set remotetimeout 10

# flash writes only through tools/hw/hw.py flash (guards: app window, backup, protection, confirmation)
define hook-load
  python raise gdb.GdbError("load is disabled here: flash with tools/hw/hw.py --radio d890 flash <file>")
end

define ocd
  target extended-remote localhost:3333
end
document ocd
Attach to OpenOCD (ST-Link or J-Link through OpenOCD).
end
define jl
  target extended-remote localhost:2331
end
document jl
Attach to SEGGER JLinkGDBServer.
end

# reset + halt at the reset vector (OpenOCD / J-Link spelling)
define rh
  monitor reset halt
end
define rhj
  monitor reset
  monitor halt
end

# OpenOCD only: serve the RTT log (make DEBUG=1) on localhost:19021 (J-Link GDB server already does: same port)
define rtt
  eval "monitor rtt setup 0x%x 72 \"SEGGER RTT\"", &_SEGGER_RTT
  monitor rtt start
  monitor rtt server start 19021 0
  echo RTT: nc localhost 19021\n
end

# fault registers
define faults
  printf "ICSR 0x%08x CFSR 0x%08x HFSR 0x%08x MMFAR 0x%08x BFAR 0x%08x\n", *(unsigned*)0xE000ED04, *(unsigned*)0xE000ED28, *(unsigned*)0xE000ED2C, *(unsigned*)0xE000ED34, *(unsigned*)0xE000ED38
end

# stop in the C part of the HardFault handler (startup/startup.c); 'faults' + 'bt' then show where
break hardfault_c
