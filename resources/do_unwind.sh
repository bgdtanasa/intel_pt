#!/bin/bash

if [[ ! -v RESOURCES_PATH ]];
then
    export RESOURCES_PATH=$(pwd)
fi
export PID=$1

PID_NAME=$(grep Name /proc/${PID}/status | awk '{print $2}')
if [[ -z "$PID_NAME" ]];
then
	PID_NAME="my_pid"
fi
PID_CPU=$(grep Cpus_allowed_list /proc/${PID}/status | awk '{print $2}')
if [[ -z "$PID_CPU" ]];
then
	PID_CPU="0"
fi

rm -rf ${RESOURCES_PATH}/../unwind.log
awk -v pid="${PID}" -v pid_name="${PID_NAME}" -v pid_cpu="${PID_CPU}" '
	function print_stk(n, stk, tsc) {
		if (n >= 1) {
			# Printing the call stack
			print pid_name "\t" pid " [" pid_cpu "] " tsc ": cpu-clock:"
			for (i = n; i >= 1; i--) {
				print "\t" stk[ i ] " " stk[ i ] " ([xxxxxx])"
			}
			print ""
		}
	}
	BEGIN {
		f = 0
	} {
		if (f == 0) {
			if (index($0, pid_name "\t0 [000] ")) {
				# The ptrace unwind starts here
				f = 1
				n = 0
			} else {
				f = 0
				n = 0
			}
		} else if (f == 1) {
			if ($0 == "") {
				# The ptrace unwind stops here
				f = 2

				# The ptrace unwind needs to be reversed
				for (i = 1; i <= int(n / 2); i++) {
					a = stk[ i ]
					stk[ i ] = stk[ n - i + 1 ]
					stk[ n - i + 1 ] = a
				}
				print_stk(n, stk, 0)
			} else {
				# Recording the ptrace unwind
				n++
				stk[ n ] = $1
			}
		} else if (f >= 2) {
			if ((index($0, "O ::   ")) || (index($0, "D ::   "))) {
				#print "Stack Reset"
				#print_stk(n, stk, 0)

				# Reseting the call stack
				f = 0
				n = 0
			} else if (index($0, " -> ")) {
				if (f == 3) {
					if (index($7, "SYSCALL")) {
						#print "Going into the kernel via syscall"
					}
					if (index($7, "CLAC")) {
						#print "Going into the kernel via async event"
					}
					if ((index($7, "CALL")) || (index($7, "SYSCALL")) || (index($7, "CLAC"))) {
						n++
						stk[ n ] = $9
					} else if ((index($7, "RET")) || (index($7, "SYSRET")) || (index($7, "IRET"))) {
						if (n >= 1) {
							n--
							print_stk(n, stk, $12)
						}
					}
				} else {
					if ($4 == stk[ n ]) {
						f = 3
					}
				}
			}
		}
	}
' ${RESOURCES_PATH}/../branches.log > ${RESOURCES_PATH}/../unwind.log
echo "Generating the raw unwind file ... Done"

rm -rf ${RESOURCES_PATH}/../unwind.log.x
if [[ -f "addr2lines/addr2line.${PID}.1" && -s "addr2lines/addr2line.${PID}.1" ]];
then
	awk '
		{
			if (NF == 10) {
				print $4 " " $6 " " $7 " " $8 " " $9
			} if (NF == 11) {
				print $4 " " $6 " " $7 " " $9 " " $10
			}
		}
	' addr2lines/addr2line.${PID}.1 | sort -k 1 -n | uniq > addr2lines/addr2line.${PID}.all

	awk '
		NR==FNR {
			u[ $1, "x" ] = $1
			u[ $1, "y" ] = $2
			u[ $1, "c" ] = $3
			u[ $1, "f" ] = $4
			u[ $1, "b" ] = $5
			next
		}

		{
			if (NF == 3) {
				k = $1

				if ((k, "x") in u) {
					c = u[ k, "c" ]
					f = u[ k, "f" ]
					b = u[ k, "b" ]
					x = u[ k, "x" ]
					y = u[ k, "y" ]
				} else {
					printf "\t%16s %s ([xxxxxx])\n", k, k
					next
				}
				printf "\t%16s %s ([%s])\n", k, f, b
				next
			} else {
				print $0
			}
		}
	' addr2lines/addr2line.${PID}.all ${RESOURCES_PATH}/../unwind.log > ${RESOURCES_PATH}/../unwind.log.x

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
