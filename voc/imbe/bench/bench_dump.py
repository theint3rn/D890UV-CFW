# Renode monitor command: bench_dump <file> <addr>  ->  "<executed instructions> <word at addr>"
def mc_bench_dump(path, addr):
    m = self.Machine
    try:
        txt = "%d %d" % (int(m["sysbus.cpu"].ExecutedInstructions), m["sysbus"].ReadDoubleWord(int(addr)))
    except Exception as e:
        txt = "ERR %s" % e
    f = open(path, "w")
    f.write(txt)
    f.close()
