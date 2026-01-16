#!/bin/bash

if [[ ! -v RESOURCES_PATH ]];
then
    export RESOURCES_PATH=$(pwd)
fi
export LINUX_PATH=$1
export LOG_FILE_PATH=$2
export PID=$3

rm -rf addr2line.*
rm -rf insts.*

awk '{
	if (index($0, " -> ")) {
		if (index($4, "ffffffff")) {
			f = "insts.vmlinux"
		} else {
			f = "insts." $5;
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

		if [[ -s "insts.${a}" ]];
		then
			echo "Generating resources for $l :: $b"
			DEBUGINFOD_URLS= INSTS_FILE_PATH=insts.${a} BINARY=${b} gdb -q -batch -ex "source ${RESOURCES_PATH}/binary_gdb_addr2func.py" --args $l 2>&1 > addr2line.${a}
		fi
    else
        echo "Invalid resource $l"
    fi
done

l=vmlinux
a=vmlinux
b=vmlinux

if [[ -s "insts.${a}" ]];
then
	echo "Generating resources for $l :: $b"
	DEBUGINFOD_URLS= INSTS_FILE_PATH=insts.${a} BINARY=${b} gdb -q -batch -ex "source ${RESOURCES_PATH}/vmlinux_gdb_addr2func.py" 2>&1 > addr2line.${a}
fi

echo "Generating the addr2line.* files ... Done"
sort -n -k 1 addr2line.* -o addr2line.${PID}.0
paste <(awk '{printf("%10s %2s %8s %16s -> %18s %15s %20s\n", $1, $2, $3, $4, $9, $7, $12)}' ${LOG_FILE_PATH}) <(awk '{printf("%48s %32s %s\n", $6, $7, $8)}' addr2line.${PID}.0) > addr2line.${PID}.1
wc -l addr2line.${PID}.0
U=$(wc -l addr2line.${PID}.1 | awk '{print $1}')
V=$(wc -l ${LOG_FILE_PATH} | awk '{print $1}')
if [[ "$U" == "$V" ]];
then
	echo "Success"

	rm -rf ${RESOURCES_PATH}/../branches.log.x
	awk '
	NR==FNR {
		key = sprintf("%10s %2s %8s %16s", $1, $2, $3, $4)
		a[ key ] = sprintf("%48s %32s %s", $9, $10, $11)
		next
	} {
		key = sprintf("%10s %2s %8s %16s", $1, $2, $3, $4)
		if (key in a) {
			print $0 " " a[key]
		} else {
			print $0
		}
	}' addr2line.${PID}.1 ${RESOURCES_PATH}/../branches.log > ${RESOURCES_PATH}/../branches.log.x
else
	echo "Error"
fi

#sed -i 's/#########/ /g' addr2line.${PID}.0
#sed -i 's/#########/ /g' addr2line.${PID}.1
