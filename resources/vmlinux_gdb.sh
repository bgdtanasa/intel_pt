#!/bin/bash

export LINUX_PATH=$1
export LOG_FILE_PATH=$2
export PID=$3

rm -rf addr2line.*
rm -rf insts.*

awk '{
	if (index($0, " -> ")) {
		if (index($5, "ffffffff")) {
			f = "insts.vmlinux"
		} else {
			f = "insts." $6;
		}
		print $0 >> f;
	}
}' $LOG_FILE_PATH
echo "Generating the insts.* files ... Done"

U=$(grep "r[-w]x[sp]" /proc/${PID}/maps | awk '{print $6}')
for l in $U;
do
    if [[ -f "$l" ]];
    then
        a=$(basename $l)
        if [[ "$a" == *"vdso"* ]];
        then
			b="[vdso]"
        else
			b=${a:0:31}
		fi
        #awk -v binary="${b}" '{ if (index($0, " " binary " ")) { print $0 } }' $LOG_FILE_PATH > insts.${a}

		if [[ -s "insts.${a}" ]];
		then
			echo "Generating resources for $l :: $b"
			DEBUGINFOD_URLS= INSTS_FILE_PATH=insts.${a} BINARY=${b} gdb -q -batch -ex "source binary_gdb_addr2func.py" --args $l 2>&1 > addr2line.${a}
		fi
    else
        echo "Invalid resource $l"
    fi
done

l=vmlinux
a=vmlinux
b=vmlinux
#awk '{ if (index($5,"ffffffff")) { print $0 } }' $LOG_FILE_PATH > insts.${a}

echo "Generating resources for $l :: $b"
DEBUGINFOD_URLS= INSTS_FILE_PATH=insts.${a} BINARY=${b} gdb -q -batch -ex "source vmlinux_gdb_addr2func.py" 2>&1 > addr2line.${a}

echo "Generating the addr2line.* files ... Done"
rm -rf addr2line.${PID}*
sort -n -k 1 addr2line.* -o addr2line.${PID}.0
paste <(awk '{printf("%15s %8s %18s -> %18s %15s %20s\n", $1, $4, $5, $10, $8, $13)}' ${LOG_FILE_PATH}) <(awk '{printf("%48s %32s %s\n", $6, $7, $8)}' addr2line.${PID}.0) > addr2line.${PID}.1
sed -i 's/#########/ /g' addr2line.${PID}.0
sed -i 's/#########/ /g' addr2line.${PID}.1
