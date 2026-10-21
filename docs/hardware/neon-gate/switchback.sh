D="python3 /tmp/neon-drive.py"; S=/var/lib/pocketos/screenshots; O=/root/neon-p98-f398ecb/log/switchback; mkdir -p $O
ls $S > /tmp/shots-before.txt
shot() { # $1 name
  $D call shell.action '{"action":"screenshot"}' >/dev/null; sleep 4
  f=$(ls -t $S | head -1); grep -qx "$f" /tmp/shots-before.txt && { echo "no new capture"; return 1; }
  mv "$S/$f" "$O/$1.png"; echo "$f" >> /tmp/shots-before.txt; }
for scr in launcher settings; do
  $D prepare doors normal >/dev/null; [ $scr = settings ] && $D call shell.open '{"id":"settings"}' >/dev/null; sleep 2
  shot $scr-doors-start; t1=$(date -u +%H:%M)
  for m in normal outdoor night; do $D prepare neon $m >/dev/null; done
  $D prepare doors normal >/dev/null; [ $scr = settings ] && $D call shell.open '{"id":"settings"}' >/dev/null; sleep 2
  shot $scr-doors-return; t2=$(date -u +%H:%M)
  cmp -s $O/$scr-doors-start.png $O/$scr-doors-return.png && r="byte-identical" || r="DIFFERS"
  echo "$scr: $r after live Neon Normal/Outdoor/Night round trip (minutes $t1/$t2)"
done
sha256sum $O/*.png; ls $S | wc -l; $D state
