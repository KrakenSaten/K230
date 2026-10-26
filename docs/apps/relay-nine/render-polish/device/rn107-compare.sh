#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# PR #107 alternating K230 comparison on unit B, temporary. Run from anywhere.
#   rn107-compare.sh capture | timing
# Each run: rn-start.sh in that kit, same keyed route, rn-measure, rn-restore.sh.
set -u
B=/root/relay-nine-efc55d43; P=/root/relay-nine-777d48aa
SLICE_ROUTE="w:900 space:1500 w+space:1500 w:1200 left:600 right:1200 w+right:1500 s:1000 space:1200 tap:r wait:1800 w+left+space:2500 wait:1500"
FULL_ROUTE="space:2500 w:1300 tap:f wait:1200 w:600 left:654 w+space:3100 wait:300 tap:q wait:300 space:1200 tap:r wait:1800 space:900 tap:q wait:400 tap:r wait:2000 right:1300 w+right+space:2000 s:800 left+space:1500 a:600 d:600 w+left:1500 space:1500 wait:2000"
cleanup() { for k in $B $P; do if [ -s $k/test-shell.pid ]; then echo "cleanup $k"; $k/rn-restore.sh | tail -1; fi; done; }
trap 'cleanup; exit 1' INT TERM
run() { # run <kit> <label> <slice|full> <capture?>
	K=$1; L=$2; S=$3; C=$4
	D="python3 $K/rn-drive.py play"; mkdir -p $K/log
	if [ "$S" = slice ]; then $K/rn-start.sh $L slice > $K/log/start-$L.txt 2>&1; else $K/rn-start.sh $L > $K/log/start-$L.txt 2>&1; fi ||
		{ echo "$L: START FAILED"; tail -5 $K/log/start-$L.txt; $K/rn-restore.sh | tail -1; return 1; }
	if [ "$S" = slice ]; then R=$SLICE_ROUTE; N=26; else R=$FULL_ROUTE; N=36; fi
	if [ -n "$C" ]; then
		# Matching captures: the opening view (exact state) and fixed points of the route.
		$D open wait:2000 tap:enter wait:1500 shot:$L-a-start > $K/log/drive-$L.txt 2>&1
		if [ "$S" = slice ]; then
			$D w:900 space:1500 w+space:1500 shot:$L-b-combat w:1200 left:600 right:1200 shot:$L-c-gallery >> $K/log/drive-$L.txt 2>&1
		else
			$D space:2500 w:1300 tap:f wait:1200 w:600 shot:$L-b-door left:654 w+space:3100 wait:300 shot:$L-c-bay tap:r wait:400 shot:$L-d-reload >> $K/log/drive-$L.txt 2>&1
		fi
	else
		$D open wait:2000 tap:enter wait:1500 > $K/log/drive-$L.txt 2>&1
		$K/rn-measure.sh $L $N > $K/log/measure-$L.txt 2>&1 &
		M=$!
		$D $R >> $K/log/drive-$L.txt 2>&1
		wait $M
	fi
	$D home wait:1200 >> $K/log/drive-$L.txt 2>&1
	echo "$L: $(tail -1 $K/log/measure-$L.txt 2>/dev/null | cut -c1-200)"
	$K/rn-restore.sh > $K/log/restore-$L.txt 2>&1; echo "$L: $(tail -1 $K/log/restore-$L.txt)"
	sleep 4
}
case "${1:-}" in
capture)
	run $B base-cap-slice slice 1; run $P pr-cap-slice slice 1
	run $P pr-cap-full full 1; run $B base-cap-full full 1 ;;
timing)
	for r in 1 2 3; do
		if [ $r = 2 ]; then
			run $P pr-slice-$r slice ""; run $B base-slice-$r slice ""; run $B base-full-$r full ""; run $P pr-full-$r full ""
		else
			run $B base-slice-$r slice ""; run $P pr-slice-$r slice ""; run $P pr-full-$r full ""; run $B base-full-$r full ""
		fi
	done ;;
*) echo "usage: $0 capture|timing"; exit 2 ;;
esac
cleanup
