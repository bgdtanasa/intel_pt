#!/bin/bash

function my_sed() {
    sed -i 's/RAX/R0/g'  $1
    sed -i 's/RDX/R1/g'  $1
    sed -i 's/RCX/R2/g'  $1
    sed -i 's/RBX/R3/g'  $1
    sed -i 's/RSI/R4/g'  $1
    sed -i 's/RDI/R5/g'  $1
    sed -i 's/RBP/R6/g'  $1
    sed -i 's/RSP/R7/g'  $1
    sed -i 's/RIP/R16/g' $1

    sed -i 's/CFA=R0:/CFA=R0+0:/g' $1
    sed -i 's/CFA=R1:/CFA=R1+0:/g' $1
    sed -i 's/CFA=R2:/CFA=R2+0:/g' $1
    sed -i 's/CFA=R3:/CFA=R3+0:/g' $1
    sed -i 's/CFA=R4:/CFA=R4+0:/g' $1
    sed -i 's/CFA=R5:/CFA=R5+0:/g' $1
    sed -i 's/CFA=R6:/CFA=R6+0:/g' $1
    sed -i 's/CFA=R7:/CFA=R7+0:/g' $1

    sed -i 's/undefined/X/g' $1
}

rm -rf objdumps/objdump.*
rm -rf dwarfs/dwarf.*

XED_KIT=xed-install-base-2025-12-04-lin-x86-64

U=$(grep "r[-w]x[sp]" /proc/$1/maps | awk '{print $6}')
while IFS= read -r l;
do
    if [[ -f ${l} ]];
    then
        a=$(basename "${l}")
        echo "Generating resources for ${l} :: ${a}"

        #objdump -d "${l}" | sed -n 's/^\s*\([0-9a-fA-F]*\):\s*\(\([0-9a-fA-F][0-9a-fA-F]\s\)*\).*/\1 \2/p' > objdumps/objdump.${a}
        ../../xed/kits/${XED_KIT}/examples/obj/xed -i "${l}" | grep XDIS | awk '{
          gsub(":", "", $2)
          printf("%s ", $2)
          for (i = 1; i <= length($5) - 2; i += 2) {
            printf("%s ", substr($5, i, 2))
          }
          printf("%s\n", substr($5, i, 2))
        }' > objdumps/objdump.${a}
        llvm-dwarfdump --debug-frame "${l}" | grep "  0x" | sort -k 1 -g > dwarfs/dwarf.${a}
        my_sed dwarfs/dwarf.${a}

        if [[ ! -s "objdumps/objdump.${a}" ]];
        then
          echo "Resource not created :: objdumps/objdump.${a}"
          exit -1
        fi
        if [[ ! -s "dwarfs/dwarf.${a}" ]];
        then
          echo "Resource not created :: dwarfs/dwarf.${a}"
        fi
    else
      echo "Invalid resource ${l}"
    fi
done <<< "${U}"
