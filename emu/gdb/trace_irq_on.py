import gdb
out = open("build/trace34.txt", "w")
n = 0
prev = None
while n < 400000:
    gdb.execute("stepi", to_string=True)
    f = gdb.selected_frame()
    pc = int(f.pc()) & 0xffffffff
    cpsr = int(gdb.parse_and_eval("$cpsr")) & 0xffffffff
    if pc != prev:
        out.write("%08x %08x\n" % (pc, cpsr))
    prev = pc
    n += 1
    if not (cpsr & 0x80):
        out.write("IRQ ENABLED at %08x after %d\n" % (pc, n))
        break
out.close()
print("done", n)
