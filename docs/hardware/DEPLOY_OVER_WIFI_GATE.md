# deploy.sh over Wi-Fi: unit A gate

**The unit carries build `09be665`** (`Doors 0.0.13 (build 09be665)`), the
same build it carried before the gate. Both deploys sent that build again, from
the SDK target tree in WSL (`~/work/t-display-k230`), whose seven service and
CLI binaries were byte-identical to the unit's beforehand (sha256 of sysd,
netd, radiod, meshcored, doors-shell, doors, pos-supervise). So the gate
exercises the deploy mechanism and nothing else changed on the unit. Nothing
was flashed; the golden SD card was not touched.

**Result: PASS on unit A, 2026-09-25.** `deploy.sh` from `fix/deploy-over-wifi`
`56e8534`, run from a clean clone of that commit, to the unit's Wi-Fi address
and then to its eth0 address. Both runs exited 0, and in both the unit's
deploy finished while its Wi-Fi was down.

## The failure this replaces

On 2026-09-25 (build `09be665`, docs/hardware/DEVICE_CONTROLS_GATE.md),
`deploy.sh` to `192.168.10.171` stopped netd from inside the ssh session that
was piping it the archive. netd manages wlan0, the connection was lost, and
`tar -C / -xf -` waited for the rest of the archive with every service already
stopped. The unit was recovered by running the deploy again over eth0.

Now the archive and `deploy_unit.sh` are copied to `/tmp/doors-deploy` and
checked with `sha256sum -c` before anything is stopped. `run.sh` is then
started with `setsid nohup`, and the host polls its log and status (see
docs/BUILD_ENVIRONMENT.md, "Deploy without reflashing").

## Before

| Item | State | Class |
|---|---|---|
| Build | `/etc/doors-release` `BUILD_ID=09be665`, `doors shell info` build `09be665` | VERIFIED |
| Services | sysd, netd (wlan0), radiod (sx1262, EU868, 2 dBm), meshcored (`Mstr_k230`), doors-shell, each under pos-supervise | VERIFIED |
| Addresses | eth0 `192.168.10.157`, wlan0 `192.168.10.171`, both /24, a default route on each | VERIFIED |
| Applets | `/usr/bin/setsid`, `/usr/bin/nohup`, `/usr/bin/sha256sum`, BusyBox v1.37.0 | VERIFIED |
| `/tmp` | tmpfs, 483.7 MB, 1.6 MB used; no `/tmp/doors-deploy` | VERIFIED |

## Run 1: to the Wi-Fi address, 192.168.10.171

Host output, seconds since start:

```
[   0.1] INSTALLATION CHECK: PASS
[   0.1] Deploying Doors 0.0.13 to root@192.168.10.171
[   2.8] payload.tar: OK / deploy_unit.sh: OK / run.sh: OK
[   3.3] Stopping doors-shell: OK
[  12.9] Stopping meshcored: OK ... Starting doors-shell: OK
[  13.0] Doors 0.0.13 (build 09be665) / pos 0.0.13 (build 09be665)
[  13.0] Done.
EXIT 0 after 13.0 s
```

Unit timeline (UTC, file times in `/tmp/doors-deploy` and netd.log):

| Time | Event |
|---|---|
| 13:53:02-03 | archive, scripts and SHA256SUMS staged |
| 13:53:06 | `run.sh` started (pid file) |
| 13:53:08.504 | netd shutting down; DHCP client and wpa_supplicant stopped: **Wi-Fi down** |
| 13:53:11.653 | netd started again (build 09be665) |
| 13:53:13 | **`status` = 0 written: the deploy finished** |
| 13:53:14.051 | associated again; 14.302 connected: **Wi-Fi back** |

## Run 2: to the eth0 address, 192.168.10.157

Also exit 0, after 12.6 s, with the same output and pattern. Wi-Fi was down
from 13:53:57.799 to 13:54:03.306; `status` = 0 was written at 13:54:02.

## Findings

- **The deploy on the unit finished with no Wi-Fi link.** Both times the
  status was written before wlan0 re-associated. This is what the change was
  for: the old pipe needed that link to carry the rest of the archive.
- **The host never reported the unit as unreachable.** The Wi-Fi gap was
  5.5 s, and the poll that was open at the time stalled on TCP retransmits
  (the 3.3 s to 12.9 s gap in the output) and then completed, inside the 15 s
  ServerAlive window. The retry path ("does not answer ... Still polling") ran
  only in the host test (tests/deploy_staging_test.sh), not here. A longer
  outage, such as netd failing to reconnect, would use it.
- **Run 2 was probably not over eth0.** Windows' ARP table on this build
  host mapped both `192.168.10.157` and `192.168.10.171` to the unit's wlan0
  MAC (`88:3b:dc:b7:9e:c7`; eth0 is `00:e0:4c:3a:5e:d0`) before and after both
  runs. The unit answers ARP for either address on either interface
  (`arp_ignore` 0). Run 2's identical stall fits that, so "over eth0" here
  means "to the eth0 address". The ARP cache was not changed (that needs an
  elevated prompt). A deploy that is really over eth0 does not depend on
  netd, so this does not affect the result.
- netd's runtime start connected both times (associated 2.4 s and 2.3 s after
  it started). The runtime-start timeout recorded in the Device Controls gate
  did not recur in these two starts.
- `shell processes=0` in the end-of-deploy count, as before this change: the
  shell had been started under its supervisor and was not yet running when
  the unit counted. `doors shell info` answers with build `09be665` afterwards.

## As left

Build `09be665` (unchanged), every service running under pos-supervise, Wi-Fi
connected at `192.168.10.171`, eth0 at `192.168.10.157`, meshcored as
`Mstr_k230`. `/tmp/doors-deploy` holds run 2's `log` and `status` (0) and no
archive. Host logs: WSL `~/work/wifi-gate/{wifi,eth}.log`.
