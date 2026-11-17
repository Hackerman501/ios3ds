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
ignore 1 32
continue
printf "HIT33 lr=%08x sp=%08x\n", $lr, $sp
x/24wx $sp
continue
printf "HIT34 lr=%08x sp=%08x\n", $lr, $sp
x/24wx $sp
continue
printf "HIT35 lr=%08x sp=%08x\n", $lr, $sp
x/24wx $sp
continue
printf "HIT36 lr=%08x sp=%08x\n", $lr, $sp
x/24wx $sp
