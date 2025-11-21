import gdb
import re
import sys
import os

def main(idx, base_addr, addr, binary):
  block = gdb.block_for_pc(addr)
  sym   = gdb.execute(f"info symbol {addr:#x}", to_string = True)
  line  = gdb.find_pc_line(addr)
  if hasattr(line.symtab, 'filename'):
    a   = str(line.symtab.filename) + ":" + str(line.line);
  else:
    a   = str("unknown") + ":" + str(line.line);

  sym = sym.strip().split("in section")
  if "+" in sym[ 0 ]:
    sym = sym[ 0 ].split("+")
  sym = sym[ 0 ].strip().replace(" ", "#########")

  while block is not None and block.function is None:
    block = block.superblock

  if block is not None:
    line  = gdb.find_pc_line(block.start)
    if hasattr(line.symtab, 'filename'):
      b   = str(line.symtab.filename) + ":" + str(line.line);
    else:
      b   = str("unknown") + ":" + str(line.line);

    if block.function is not None:
      function = str(block.function).strip().replace(" ", "#########")

      print(f"{idx:>10} :: {hex(base_addr):>12} {hex(addr):>32} {function:>48} {function:>48} {binary} {a}")
    else:
      print(f"{idx:>10} :: {hex(base_addr):>12} {hex(addr):>32} {str("__UNK_FNC__"):>48} {function:>48} {binary} {a}")
  else:
    if "No symbol matches" in sym:
      print(f"{idx:>10} :: {hex(base_addr):>12} {hex(addr):>32} {str("__UNK_BLK__"):>48} {str("__UNK_SYM__"):>48} {binary} {a}")
    else:
      print(f"{idx:>10} :: {hex(base_addr):>12} {hex(addr):>32} {str("__UNK_BLK__"):>48} {sym:>48} {binary} {a}")

if __name__ == "__main__":
  gdb.execute("set pagination off", to_string = True)
  gdb.execute("set confirm off",    to_string = True)

  log    = os.getenv("INSTS_FILE_PATH")
  binary = os.getenv("BINARY")
  with open(f"{log}", "r") as f:
    for l in f:
      w = l.split()
      if " -> " in l and binary in w[ 5 ]:
        main(w[ 0 ], int("0x" + w[ 3 ], 0), int("0x" + w[ 3 ], 0), w[ 5 ])
      else:
        print(l)
        print("Err")

  gdb.execute("quit", to_string = True)
