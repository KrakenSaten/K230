#!/bin/sh
# K1 on-unit driver (PacketShark NEURAL research kit; ROADMAP.md section 1,
# poc/README.md). Lives only in /tmp/k1 and is removed afterwards.
# Usage: sh /tmp/k1/k1.sh STEP   (run in the background; see the host scripts)
# It never signals a process. Everything it starts ends by itself; the CPU
# hog of P8 stops when the stop file appears. P7 (SIGKILL) is not here.
# Output: /tmp/k1/out/STEP.jsonl, then /tmp/k1/out/STEP.done
K=/tmp/k1
O=$K/out
B=$K/infer-bench
D="--in $K/in.f32 --batches 32 --weights $K/w.f32"
L="--lock $K/kpu.lock"
U8="--kmodel $K/u8.kmodel --expect $K/sim_u8.f32"
A16="--kmodel $K/a16.kmodel --expect $K/sim_a16.f32"
s=$1
mkdir -p $O
cma() { sync; echo 1 > /proc/sys/vm/drop_caches; sleep 2; awk '/^CmaFree:/{print $2}' /proc/meminfo; }
# cma_alloc_success / alloc_pages_success count explicit CMA allocations
# (dma_alloc_coherent, so mmz), not movable pages that merely sit in CMA.
mark() {
	a=$(awk '/^cma_alloc_success/{print $2}' /proc/vmstat)
	p=$(cat /sys/kernel/mm/cma/reserved/alloc_pages_success)
	echo "{\"what\":\"step\",\"step\":\"$1\",\"uptime\":\"$(cut -d' ' -f1 /proc/uptime)\",\"loadavg\":\"$(cut -d' ' -f1-3 /proc/loadavg)\",\"cma_alloc_success\":$a,\"cma_alloc_pages\":$p,\"CmaFree_dropped_kB\":$(cma)}"
}
run() { "$@"; echo "{\"what\":\"rc\",\"rc\":$?}"; }
step() {
	case "$s" in
	idle)
		mark idle1; sleep 20; mark idle2; sleep 20; mark idle3 ;;
	p1)
		mark before
		run $B --mode kpu $U8 $D --iters 10
		run $B --mode kpu $U8 $D --iters 10
		mark after ;;
	reff)
		run $B --mode ref $D --expect $K/float.f32 --iters 32 ;;
	p2u8)
		mark before
		run $B --mode both $U8 $D --iters 1000 $L --dump $O/kpu_u8.f32
		mark after ;;
	p2a16)
		mark before
		run $B --mode both $A16 $D --iters 1000 $L --dump $O/kpu_a16.f32
		mark after ;;
	p3)
		mark before
		run $B --mode kpu $U8 $D --iters 200 --profile
		mark after ;;
	p8)
		mark before
		rm -f $K/stop
		( while [ ! -e $K/stop ]; do :; done ) &
		sleep 5
		echo "{\"what\":\"hog\",\"loadavg\":\"$(cut -d' ' -f1-3 /proc/loadavg)\"}"
		run $B --mode both $U8 $D --iters 300
		echo "{\"what\":\"hog\",\"loadavg\":\"$(cut -d' ' -f1-3 /proc/loadavg)\"}"
		touch $K/stop
		wait
		rm -f $K/stop
		mark after ;;
	p9)
		mark before
		run $B --mode kpu $U8 $D --interval-ms 1000 --duration-s 600 $L
		mark after ;;
	p10)
		mark during
		run $B --mode both $U8 $D --iters 10 $L ;;
	*)
		echo "{\"what\":\"error\",\"step\":\"$s\"}" ;;
	esac
}
step > $O/$s.jsonl 2>&1
echo "{\"what\":\"done\",\"step\":\"$s\"}" >> $O/$s.jsonl
touch $O/$s.done
