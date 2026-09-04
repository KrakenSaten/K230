# First boot runbook (T-Display K230)

Goal of day one: turn the DOCUMENTED claims in T-DISPLAY-K230.md into
VERIFIED ones, then boot the first PocketOS image. Expect two to three hours.

## 0. Before the boards arrive (Windows host)

Status 2026-09-04: the host has OpenSSH and Windows Terminal, no WCH serial
driver, no flash tool, and no visible SD card reader.

- [ ] SD card reader: check for a laptop microSD slot (it only appears in
      Windows once a card is inserted), otherwise a USB reader.
- [ ] Flash tool: Rufus portable (no install) or balenaEtcher, downloaded
      from their official sites. WSL2 cannot write to USB card readers.
- [ ] Serial driver: the board's USB-UART is a WCH CH342K (DOCUMENTED,
      schematic). Windows 11 may load an inbox driver; if no COM ports appear
      after plugging in, install the official WCH CH343SER driver. This is a
      system change: the developer installs it.
- [ ] Serial terminal: PuTTY from the official site
      (https://www.chiark.greenend.org.uk/~sgtatham/putty/, not putty.org),
      115200 8N1. Windows Terminal has no serial support of its own. Two COM ports will appear: the lower one is K230
      UART0 (console), the other is UART3.
- [ ] Two microSD cards (8 GB or more): one with the vendor image, one for
      PocketOS, so switching back is a card swap.
- [ ] SSH key: `ssh-keygen` on Windows, and keep the public key handy for the
      device (`/root/.ssh/authorized_keys`).

## 1. Unbox and inspect (both units)

- [ ] Photograph both PCBs. Read the silkscreen revision and the LoRa module
      marking (13A / 16A / T89 / 16E footprint options; HPD16A = SX1262).
      Record in T-DISPLAY-K230.md under "Board revision".
- [ ] Note which base boards, if any, came with the units.

## 2. Vendor image, unit A

1. Write the vendor image (`k230_bsp/images/sysimage-sdcard.img` from the
   WSL build, copied to `out/`) to card 1 with the flash tool.
2. Insert, connect the USB-UART port, open the console at 115200, power on.
   Expect U-Boot, the LILYGO logo and the LVGL launcher within a minute.
3. From the launcher, connect Wi-Fi or plug Ethernet and read the IP address.
4. `ssh root@<ip>` (vendor image: no password; set one).
5. Copy and run the inventory:

```sh
scp tools/hwcheck/hwcheck.sh root@<ip>:/tmp/ && ssh root@<ip> 'sh /tmp/hwcheck.sh'
scp -r root@<ip>:/root/hwcheck/ docs/hardware/hwcheck-unitA/
```

6. Run it again with `--lora` (it stops the launcher first) and check the
   register read returns `14 24`. Restart the launcher afterwards.
7. Answer the open items: RAM size (`MemTotal`), Wi-Fi chip (`lsmod`,
   `/sys/bus/sdio/devices`), input device names, DRM connector modes.

## 3. PocketOS image, unit A

1. Write `out/k230/sysimage-sdcard.img` to card 2. Boot. The vendor launcher
   still starts (expected); PocketOS services run alongside.
2. Over SSH:

```sh
pos system info
pos hardware list
pos radio info          # backend mock at this point
pos logs
```

3. Switch to PocketOS on the panel (see platforms/k230/README.md):

```sh
/etc/init.d/S99zz_k230_phone_ui stop
echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
/etc/init.d/S60radiod restart && pos radio info
echo ENABLE=1 > /etc/default/pocketos-shell
/etc/init.d/S90pocketos-shell start
pos app list && pos shell screenshot /root/shell.png
```

4. Record what works and what does not in KNOWN_ISSUES.md. Likely first
   failures: DRM mode selection or rotation for the RM69A10, touch axis
   mapping, SX1262 begin (TCXO setting).

## 4. Radio link, both units

With PocketOS radiod on both units (sx1262 backend):

```sh
tests/hw/lora_pair_test.sh <ip-A> <ip-B>
```

Passing means a packet sent by B was received by A with RSSI and SNR
reported, which is the v0.1 radio acceptance criterion.

## 5. Afterwards

- Update T-DISPLAY-K230.md evidence classes and commit the hwcheck reports.
- File the vendor build's `make legal-info` output under docs/legal/.
