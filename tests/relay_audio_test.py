#!/usr/bin/env python3
"""Foreground production audio service, ALSA file/null, exclusion and cleanup.
Copyright (c) 2026 PocketOS authors.
SPDX-License-Identifier: Apache-2.0
"""
import os, pathlib, select, signal, struct, subprocess, sys, tempfile, time, wave
helper=str(pathlib.Path(sys.argv[1]).resolve())
out=pathlib.Path(sys.argv[2]).resolve() if len(sys.argv)>2 else None
checks=0
def check(x):
 global checks
 checks+=1
 assert x
with tempfile.TemporaryDirectory(prefix='relay-audio-') as tmp:
 root=pathlib.Path(tmp);raw=root/'effects.raw';cfg=root/'alsa.conf'
 cfg.write_text(f'''pcm.null {{ type null }}
pcm.relayfile {{ type file slave.pcm "null" file "{raw}" format "raw" }}
''')
 env=dict(os.environ,POCKETOS_AUDIO_BOARD='generic',POCKETOS_AUDIO_PCM='relayfile',POCKETOS_RUNTIME_DIR=tmp,ALSA_CONFIG_PATH=str(cfg))
 def start():
  return subprocess.Popen([helper,'45'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
 def ready(p):
  check(bool(select.select([p.stdout],[],[],2)[0]));return p.stdout.readline().strip()
 p=start();check(ready(p)==b'READY')
 q=start();check(ready(q).startswith(b'UNAVAILABLE'));check(q.wait(timeout=2)==1)
 muted=subprocess.Popen([helper,'0'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
 check(ready(muted)==b'MUTED');muted.stdin.close();check(muted.wait(timeout=2)==0)
 begin=time.monotonic()
 for k in [0,1,2,4,5,3,6,7]:
  stamp=int(time.monotonic()*1000)&0xffffffff
  p.stdin.write(struct.pack('<B3xI',k,stamp));p.stdin.flush();time.sleep(.08)
 p.stdin.close();check(p.wait(timeout=2)==0);check(time.monotonic()-begin<2)
 data=raw.read_bytes();samples=struct.unpack('<'+'h'*(len(data)//2),data)
 check(len(samples)>10000);check(max(map(abs,samples))>100);check(max(map(abs,samples))<=8192)
 if out:
  out.parent.mkdir(parents=True,exist_ok=True)
  with wave.open(str(out),'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(data)
 # SIGTERM closes the real PCM/lock; SIGKILL is reconciled by recover.
 p=start();check(ready(p)==b'READY');p.terminate();check(p.wait(timeout=2)==0)
 p=start();check(ready(p)==b'READY');p.kill();p.wait(timeout=2)
 check(subprocess.run([helper,'recover'],env=env,timeout=2).returncode==0)
 p=start();check(ready(p)==b'READY');p.stdin.close();check(p.wait(timeout=2)==0)
 # Expired events do not become delayed gunshots.
 raw.unlink(missing_ok=True)
 p=start();check(ready(p)==b'READY');p.stdin.write(struct.pack('<B3xI',0,(int(time.monotonic()*1000)-1000)&0xffffffff));p.stdin.flush();time.sleep(.08);p.stdin.close();check(p.wait(timeout=2)==0)
 check(not any(raw.read_bytes()))
print(f'relay_audio_test: {checks} checks passed; production service captured original mixed PCM; no device audio used')
