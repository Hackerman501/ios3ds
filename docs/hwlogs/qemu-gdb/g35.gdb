set pagination off
set confirm off
set schedule-multiple on
target extended-remote :1234
add-inferior
inferior 2
attach 2
inferior 1
thread 1.1
hbreak *0x80840010 thread 1.1
condition 1 *(unsigned*)0x80840080 == 33
continue
printf "PRE34 lr=%08x sp=%08x cpsr=%08x calls=%u dec=%u\n", $lr, $sp, $cpsr, *(unsigned*)0x80840080, *(unsigned*)0x8084008c
x/32wx $sp
condition 1 *(unsigned*)0x80840080 == 34
continue
printf "PRE35 lr=%08x sp=%08x cpsr=%08x calls=%u dec=%u\n", $lr, $sp, $cpsr, *(unsigned*)0x80840080, *(unsigned*)0x8084008c
x/32wx $sp
