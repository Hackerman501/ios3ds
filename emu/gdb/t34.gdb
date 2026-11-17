set pagination off
set confirm off
target extended-remote :1234
thread 1.1
hbreak *0x80840010 thread 1.1
condition 1 *(unsigned*)0x80840080 == 33
continue
delete
printf "AT34 lr=%08x cpsr=%08x\n", $lr, $cpsr
set scheduler-locking step
source emu/gdb/trace_irq_on.py
