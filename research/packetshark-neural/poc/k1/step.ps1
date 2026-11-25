param([string]$Step, [int]$PollS = 100, [switch]$NoStart)
# Starts one k1.sh step in the background on Unit A, then waits (bounded) for
# its done marker and prints its output. Never signals any process.
$ser = 'C:\K230-artifacts\vision-stream\scripts\ser.ps1'
$log = "C:\K230-artifacts\packetshark-neural\k1-unit-a\logs\step-$Step.log"
if (-not $NoStart) {
  & $ser -Port COM5 -Cmd "sh /tmp/k1/k1.sh $Step >/dev/null 2>&1 & echo ST`$((1+1))GO" -Until 'ST2GO' -MaxMs 8000 -Out $log | Out-Null
}
$n = [int]($PollS / 2)
$c = "i=0; while [ ! -e /tmp/k1/out/$Step.done ] && [ `$i -lt $n ]; do sleep 2; i=`$((i+1)); done; ls /tmp/k1/out/$Step.done; cat /tmp/k1/out/$Step.jsonl; echo PL`$((1+1))DONE"
& $ser -Port COM5 -Cmd $c -Until 'PL2DONE' -MaxMs (($PollS + 30) * 1000) -Out $log
