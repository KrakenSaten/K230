# Get started with DOORS

This guide takes you from a downloaded DOORS image to a working device:
write the microSD card, boot, join Wi-Fi and find your way around. You do
not need a compiler or a development environment.

> **Availability.** DOORS has no public release yet. The images published
> so far, up to and including v0.3.0, are for internal development and
> testing only: DOORS' own licence is still being finalised and those
> images carry an AGPL-3.0 AI model (docs/LICENSING.md, items 1 and 10).
> This guide is written for the first public release and describes v0.3.0,
> the current image. Read the release notes of the image you download; they
> are the authority on what that image contains.

## 1. What you need

- A **LILYGO T-Display K230**. The keyboard base is optional: DOORS works
  with the touchscreen alone, and the base adds the physical keyboard and
  F-key shortcuts.
- A **microSD card**. Everything DOORS uses lives on it (bootloader,
  kernel and system), so it replaces whatever the card held before. The
  v0.3.0 image is 763 MB once unpacked, so the card must be larger than
  that.
- A **microSD card reader** for your computer: a built-in slot or a USB
  reader.
- A **USB-C cable** and power for the K230.
- A computer with **balenaEtcher** (Windows, macOS or Linux) or **Rufus**
  (Windows only).
- For RIFT (LoRa messaging, section 6): a **LoRa antenna** connected to the
  radio before the radio is switched on.

For v0.3.0 only, you also need a one-time serial connection; see section 4.

## 2. Download the image

Images are attached to the releases on the project's GitHub page:
<https://github.com/KrakenSaten/K230/releases>.

Each release has three files. You need the first; the second lets you
check it:

| File | What it is |
|---|---|
| `doors-<version>-tdisplay-k230-<build>.img.gz` | The card image, compressed. For v0.3.0: `doors-0.3.0-tdisplay-k230-2956eff.img.gz` |
| `doors-<version>-tdisplay-k230-<build>.img.gz.sha256` | Its SHA-256 checksum |
| `BUILD_INFO.txt` | How the image was built; not needed to install |

Do not download "Source code (zip)" or "Source code (tar.gz)": those are
the source, not an image. Keep the `.img.gz` file as it is; both tools
below unpack it while writing.

**Check the download** (recommended). Compare the file's SHA-256 with the
value in the `.sha256` file, or with the one in the release notes:

- Windows (PowerShell): `Get-FileHash .\doors-0.3.0-tdisplay-k230-2956eff.img.gz -Algorithm SHA256`
- macOS: `shasum -a 256 doors-0.3.0-tdisplay-k230-2956eff.img.gz`
- Linux: `sha256sum doors-0.3.0-tdisplay-k230-2956eff.img.gz`

The v0.3.0 value is
`0360f4bc2ec8b01d24e41dfd2021eef0b1aadf262fbc894148de5ecc418c6337`
(docs/releases/v0.3.0.md). If it differs, download again.

## 3. Write the microSD card

> **Warning: writing the image erases the selected card completely.**
> Everything on it is lost. Before you start, copy off anything you want
> to keep and double-check that you have selected the microSD card, not
> another drive. If you are re-flashing a card that already runs DOORS,
> its messages, settings, Wi-Fi networks and RIFT identity are erased too
> (section 8).

### With balenaEtcher (Windows, macOS, Linux)

Download it from the official site, <https://etcher.balena.io/>.

1. Insert the microSD card.
2. Open balenaEtcher and choose **Flash from file**. Select the `.img.gz`
   file.
3. Choose **Select target** and pick the microSD card. Check its size and
   name.
4. Choose **Flash!** and wait. Etcher writes the card and then validates
   it. Do not remove the card until it says the flash is complete.

### With Rufus (Windows alternative)

Download it from the official site, <https://rufus.ie/>. Rufus runs on
Windows only; the portable version needs no installation.

1. Insert the microSD card.
2. Open Rufus. Under **Device**, select the microSD card.
3. Choose **SELECT** and pick the `.img.gz` file. Rufus writes compressed
   disk images directly, so there is no need to unpack it first. Leave the
   other options as Rufus sets them.
4. Choose **START**, read the warning that all data on the device will be
   destroyed, confirm, and wait until the status says it is ready.

If the card does not appear in Rufus's Device list, use balenaEtcher.

## 4. First boot

Eject the card safely, put it into the K230, and connect USB-C power.

**v0.3.0: one-time switch to DOORS.** On a freshly written v0.3.0 card the
device first shows the LILYGO launcher, not DOORS. The switch is made once,
from the serial console, and is kept from then on:

1. Connect the K230's USB-C port that carries the serial console to your
   computer (the board's USB-to-serial chip is a WCH CH342; two serial
   ports appear, and the console is the lower-numbered one). If no port
   appears on Windows, install WCH's CH343SER driver.
2. Open the console with a serial terminal (for example PuTTY, from
   <https://www.chiark.greenend.org.uk/~sgtatham/putty/>) at **115200
   baud, 8N1**. Press Enter and log in as `root` (no password on a fresh
   card).
3. Type:

   ```sh
   echo ENABLE=0 > /etc/default/k230_phone_ui
   echo ENABLE=1 > /etc/default/doors-shell
   reboot
   ```

After the reboot DOORS starts on its own every time. This step has already
been removed on the development branch, so a fresh card boots straight into
DOORS there; it will go away for users when a release containing that change
is published, and that release's notes will say so.

## 5. Find your way around

- **Lock screen.** DOORS starts at the lock screen. Swipe up to open it
  (or press Enter, Space or Up on the keyboard base). It is not a security
  lock: there is no code.
- **Launcher.** Three favourite slots (tap an empty one, or long-press one,
  to choose an app), then Terminal, RIFT, Browser and Settings, and the
  folders **Apps**, **Utilities** and **Games**.
- **Back.** The top-left corner of an app's header takes you back.
- **Controls.** The Controls button on the launcher opens quick settings:
  Wi-Fi, LoRa radio, rotation, display mode, brightness, volume, Lock and
  Power.
- **Keyboard base shortcuts.** F1 Home, F2 Settings, F3/F4 keyboard light,
  F5/F6 volume, F7 screenshot, F8 Terminal, F9 RIFT, F10/F11 brightness.
- **Text size and theme.** Settings > Appearance.
- **Rotation.** The device follows its orientation (Automatic); Controls
  cycles through the rotation modes. Timber is portrait-only.
- **Power off or restart.** Controls > Power opens System, where both ask
  for confirmation first.

### Connect to Wi-Fi

1. Open **Settings**. Wi-Fi is the first panel.
2. Turn Wi-Fi on if it is off, and wait for the list of networks (or tap
   **Scan**).
3. Tap your network, type the password with the on-screen keyboard or the
   keyboard base, and tap **JOIN**.

Networks you have joined are remembered and shown as SAVED. WPA3-only,
WEP, enterprise and hidden networks cannot be joined from Settings in
v0.3.0.

## 6. Set up RIFT (LoRa messaging)

RIFT is DOORS' MeshCore messenger. It is **off on a fresh card** and needs
three deliberate steps, because it transmits on the LoRa radio.

> **Before you start:** connect a LoRa antenna. Transmitting without one
> may damage the radio. DOORS uses the MeshCore EU profile (869.618 MHz,
> 2 dBm by default; docs/services/MESHCORED.md). Use it only where that
> frequency and power are permitted for you.

1. Open **Terminal** from the launcher (it is a root shell on the device)
   and type:

   ```sh
   echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
   echo MESHCORED_ENABLE=1 > /etc/default/meshcored
   reboot
   ```

   The first line uses the real LoRa chip instead of the simulated one;
   the second starts the MeshCore service at boot.
2. After the reboot, open **Controls** and tap the **LoRa radio** tile.
   DOORS asks you to confirm that an antenna is connected; choose
   **Enable radio**. The choice is kept across reboots.
3. Open **RIFT**. Other MeshCore nodes appear under NODES as they are
   heard. ACTIVITY > CHANNELS lists, adds and leaves channels, and THIS
   DEVICE gives your node a name.

To switch the radio off again, tap the LoRa radio tile in Controls.

## 7. Troubleshooting

| Problem | Try this |
|---|---|
| Nothing on the screen, or the device does not start | Write the card again and let the tool validate it; check the download's checksum; try another card. Everything the device boots is on the card, so a fresh write is always a clean start. |
| The LILYGO launcher appears instead of DOORS | You are on v0.3.0 and the one-time switch in section 4 has not been made yet. |
| Wi-Fi does not connect | Read the message under the Wi-Fi switch in Settings (for example a wrong passphrase); check the password, then turn Wi-Fi off and on. |
| RIFT stays empty or offline | Check the three steps in section 6: both settings files, the reboot, and the radio switched on in Controls. Other nodes appear only once they are heard. |
| The device freezes after opening and closing Camera or Vision repeatedly | A known issue; disconnect power and start it again. |
| A rotation closed the Terminal session | Expected in v0.3.0; the Terminal starts a new shell. |

More: the known limitations in the release notes
([v0.3.0](releases/v0.3.0.md#known-limitations)), the full list in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md), and recovery over the serial console
in [FIRST_BOOT.md, "Recovery"](hardware/FIRST_BOOT.md#recovery).

## 8. Updating to a new release

A new image is written the same way, and it replaces the whole card:
messages, settings, saved Wi-Fi networks and the RIFT identity included.
To keep them, the release notes describe which directories to save first
(for v0.3.0: [Upgrading from v0.2.1](releases/v0.3.0.md#upgrading-from-v021)).
Copying them off the device needs SSH, which requires setting a root
password first ([platforms/k230/README.md](../platforms/k230/README.md),
"First login and remote access").
