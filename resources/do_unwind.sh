#!/bin/bash

if [[ ! -v RESOURCES_PATH ]];
then
    export RESOURCES_PATH=$(pwd)
fi
export PID=$1

PID_NAME=$(grep Name /proc/${PID}/status | awk '{print $2}')
PID_CPU=$(grep Cpus_allowed_list /proc/${PID}/status | awk '{print $2}')

rm -rf ${RESOURCES_PATH}/../unwind.log
awk -v pid="${PID}" -v pid_name="${PID_NAME}" -v pid_cpu="${PID_CPU}" '
	BEGIN {
		f = 0
	} {
		if (f == 0) {
			if (index($0, pid_name "\t0 [000] ")) {
				# The ptrace unwind starts here
				f = 1;
				n = 0;
			} else {
				f = 0;
				n = 0;
			}
		} else if (f == 1) {
			if ($0 == "") {
				# The ptrace unwind stops here
				f = 2;

				# The ptrace unwind needs to be reversed
				for (i = 1; i <= int(n / 2); i++) {
					a = stk[ i ];
					stk[ i ] = stk[ n - i + 1 ];
					stk[ n - i + 1 ] = a;
				}
			} else {
				# Recording the ptrace unwind
				n++;
				stk[ n ] = $1;
			}
		} else if (f == 2) {
			if ((index($0, "O ::   ")) || (index($0, "D ::   "))) {
				# Reseting the call stack
				f = 0;
				n = 0;
			} else if (index($0, " -> ")) {
				# Updating the call stack
				if (index($7, "CALL")) {
					n++;
					stk[ n ] = $9
				} else if (index($7, "RET")) {
					if (n >= 1) {
						# Printing the call stack
						print pid_name "\t" pid " [" pid_cpu "] " $12 ": cpu-clock:"
						for (i = n; i >= 1; i--) {
							print "\t" stk[ i ] " " stk[ i ] " ([xxxxxx])"
						}
						print ""

						n--;
					}
				}
			}
		}
	}
' ${RESOURCES_PATH}/../branches.log > ${RESOURCES_PATH}/../unwind.log
echo "Generating the raw unwind file ... Done"

rm -rf ${RESOURCES_PATH}/../unwind.log.x
if [[ -f "addr2line.${PID}.1" && -s "addr2line.${PID}.1" ]];
then
	awk '{print $4 " " $6 " " $9 " " $10}' addr2line.${PID}.1 | sort -k 1 -n | uniq > addr2line.${PID}.all
	awk '
		NR==FNR {
			k = $1

			a[ k, "a_0" ] = $1
			a[ k, "a_1" ] = $2
			a[ k, "f" ] = $3
			a[ k, "b" ] = $4
			
			x[ $2 ] = k
			next
		} {
			if (NF == 3) {
				if (($1, "a_0") in a) {
					k = $1
					printf("\t%16s %s ([%s])\n", a[ k, "a_0" ], a[ k, "f" ], a[ k, "b" ])
				} else if ($1 in x) {
					k = x[ $1 ]
					printf("\t%16s %s ([%s])\n", a[ k, "a_1" ], a[ k, "f" ], a[ k, "b" ])
				} else {
					printf("\t%16s %s ([xxxxxx])\n", $1, $1)
				}
			} else {
				print $0
			}
		}
	' addr2line.${PID}.all ${RESOURCES_PATH}/../unwind.log > ${RESOURCES_PATH}/../unwind.log.x

	echo "Generating the modified unwind file ... Done"
fi

if [[ -f "${RESOURCES_PATH}/../unwind.log.x" && -s "${RESOURCES_PATH}/../unwind.log.x" ]];
then
	echo "Success"

	#sed -i 's/#########/ /g' ${RESOURCES_PATH}/../unwind.log
	#sed -i 's/#########/ /g' ${RESOURCES_PATH}/../unwind.log.x

	#${RESOURCES_PATH}/../../FlameGraph/stackcollapse-perf.pl ${RESOURCES_PATH}/../unwind.log.x > ${RESOURCES_PATH}/../unwind.folded
	#${RESOURCES_PATH}/../../FlameGraph/flamegraph.pl ${RESOURCES_PATH}/../unwind.folded > ${RESOURCES_PATH}/../unwind.svg
else
	echo "Error"
fi
