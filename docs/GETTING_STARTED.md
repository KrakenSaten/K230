# Get started with DOORS

This guide takes you from a downloaded DOORS image to a working device:
write the microSD card, boot, join Wi-Fi, set up RIFT and find your way
around. You do not need a compiler or a development environment.

It describes **DOORS 0.3.5**, the current release
(<https://github.com/KrakenSaten/K230/releases/tag/v0.3.5>). The release
notes ([docs/releases/v0.3.5.md](releases/v0.3.5.md)) are the authority on
what an image contains; read them for the image you download.

> **Licences.** DOORS' own code is Apache-2.0. The image is a whole Linux
> system and is not Apache-2.0 as a whole: its parts keep their own
> licences (THIRD_PARTY_NOTICES.txt, in the image and on the release). The
> release notes list the third-party questions that are still open.

## 1. What you need

- A **LILYGO T-Display K230**. The keyboard base is optional: DOORS works
  with the touchscreen alone, and the base adds the physical keyboard and
  F-key shortcuts.
- A **microSD card**. Everything DOORS uses lives on it (bootloader,
  kernel and system), so it replaces whatever the card held before. The
  image is 763 MB once unpacked, so the card must be larger than that.
- A **microSD card reader** for your computer: a built-in slot or a USB
  reader.
- A **USB-C cable** and power for the K230.
- A computer with **balenaEtcher** (Windows, macOS or Linux) or **Rufus**
  (Windows only).
- For RIFT (LoRa messaging, section 6): a **LoRa antenna** connected to the
  radio before the radio is switched on.

## 2. Download the image

**Download the image directly:
[doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz](https://github.com/KrakenSaten/K230/releases/download/v0.3.5/doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz)**
(116 MB). This is the ready-to-flash microSD card image, compressed.

The same file is on the release page,
<https://github.com/KrakenSaten/K230/releases/tag/v0.3.5>, together with:

| File | What it is |
|---|---|
| `doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz` | **The card image, compressed. This is the one you need.** |
| `doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz.sha256` | Its SHA-256 checksum |

The release also carries `SHA256SUMS.txt` (the checksums of every file),
`BUILD_INFO.txt` (how the image was built), the licence and notices files,
and `doors-0.3.5-3d4ea6e-legal-info.tar` (about 880 MB of third-party
source material). None of these is needed to install.

> **Image, not source code.** GitHub adds "Source code (zip)" and "Source
> code (tar.gz)" to every release. Those are DOORS' source code and cannot
> be written to a card. The file to flash is the `.img.gz` above.

Keep the `.img.gz` file as it is; both tools below unpack it while writing.

**Check the download** (recommended). Its SHA-256 must be
`b011bb28feb3fa19a2f84c35bd281a87ededa0e4712be22648e65adf24c973c0`:

- Windows (PowerShell): `Get-FileHash .\doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz -Algorithm SHA256`
- macOS: `shasum -a 256 doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz`
- Linux: `sha256sum doors-0.3.5-tdisplay-k230-3d4ea6e.img.gz`

If it differs, download it again.

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

Eject the card safely, put it into the K230 while it is powered off, and
connect USB-C power. A freshly written card boots **straight into DOORS**;
there is no setup step and nothing to switch over.

To check the version: **Settings**, then the System page. DOORS 0.3.5 shows
version `0.3.5` and build `3d4ea6e`.

## 5. Find your way around

- **Lock screen.** DOORS starts at the lock screen. Swipe up to open it
  (or press Enter, Space or Up on the keyboard base). It is not a security
  lock: there is no code.
- **Launcher.** Three favourite slots (tap an empty one, or long-press one,
  to choose an app), then Terminal, RIFT, Browser and Settings, and the
  folders **Apps**, **Utilities** and **Games**.
- **Back.** The top-left corner of an app's header takes you back.
- **Buttons on the board.** The **BOOT** button: a short press goes back,
  a hold of about a second goes Home. The **power** button: a short press
  turns the screen off and on, a hold of about a second opens the power
  menu (restart, power off).
- **Controls.** The Controls button on the launcher opens quick settings:
  Wi-Fi, LoRa radio, rotation, display mode, brightness, volume, Lock and
  Power.
- **Keyboard base shortcuts.** F1 Home, F2 Settings, F3/F4 keyboard light,
  F5/F6 volume, F7 screenshot, F8 Terminal, F9 RIFT, F10/F11 brightness.
- **Text size and theme.** Settings, Appearance.
- **Rotation.** The device follows its orientation (Automatic); Controls
  cycles through the rotation modes. Timber is portrait-only.

### Connect to Wi-Fi

1. Open **Settings** and its Wi-Fi page.
2. Turn Wi-Fi on if it is off, and wait for the list of networks.
3. Tap your network, type the password with the on-screen keyboard or the
   keyboard base, and join.

Networks you have joined are remembered and reconnect after a restart.
WPA3-only, WEP and enterprise networks cannot be joined from Settings, and
hidden networks are not listed (docs/apps/SETTINGS.md).

### Vision

The camera apps work as soon as the card boots. **Vision** in 0.3.5 has no
object-detection model: COLOR, EDGE and LINE TRACE work, and DETECT, TRACK
and TRAFFIC are switched off, which Vision says under the picture.
DeskBuddy shows "NO VISION YET" for the same reason.

## 6. Set up RIFT (LoRa messaging)

RIFT is DOORS' MeshCore messenger. On a fresh card the LoRa radio and the
MeshCore service are both off; one question in Controls sets them up.

> **Before you start:** connect a LoRa antenna to the radio's antenna
> socket (MMCX1). Transmitting without one may damage the radio. DOORS uses
> the MeshCore EU profile (869.618 MHz, 2 dBm by default;
> [MESHCORED.md](services/MESHCORED.md)). Use it only where that frequency
> and power are permitted for you. Once set up, your node can answer other
> nodes on the air.

1. Open **Controls** and tap the **LoRa radio** tile.
2. DOORS asks **"Set up the LoRa radio for RIFT?"** and reminds you about
   the antenna. With the antenna connected, choose **Set up radio**.
3. The tile shows "Setting up" for a few seconds, then DOORS says **"The
   LoRa radio is set up."**
4. Open **RIFT**. It goes online, and other MeshCore nodes appear under
   NODES as they are heard. ACTIVITY > CHANNELS lists, adds and leaves
   channels, and THIS DEVICE gives your node a name.

The setup is kept across restarts. Afterwards the LoRa radio tile switches
the radio off with one tap; switching it on again asks about the antenna
once more.

## 7. Troubleshooting

| Problem | Try this |
|---|---|
| Nothing on the screen, or the device does not start | Write the card again and let the tool validate it; check the download's checksum; try another card. Everything the device boots is on the card, so a fresh write is always a clean start. |
| Wi-Fi does not connect | Read the message on the Wi-Fi page in Settings (for example a wrong passphrase); check the password, then turn Wi-Fi off and on. |
| "The radio could not be set up." | The message says which step failed; your previous settings were put back. Check the antenna and try the LoRa radio tile again. |
| RIFT stays offline or empty | Check that the LoRa radio tile reads "Receiving". Other nodes appear only once they are heard. |
| The device freezes after opening and closing Camera or Vision repeatedly | A known issue; disconnect power and start it again. |
| A rotation closed the Terminal session | Expected; the Terminal starts a new shell. |

More: the known limitations in the release notes
([v0.3.5](releases/v0.3.5.md#known-limitations)), the full list in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md), and recovery over the serial console
in [FIRST_BOOT.md, "Recovery"](hardware/FIRST_BOOT.md#recovery).

## 8. Updating to a new release

A new image is written the same way, and it replaces the whole card:
messages, settings, saved Wi-Fi networks and the RIFT identity included.
To keep them, the release notes describe which directories to save first
(for 0.3.5: [Upgrading from v0.3.0](releases/v0.3.5.md#upgrading-from-v030)).
Copying them off the device needs SSH, which requires setting a root
password first ([platforms/k230/README.md](../platforms/k230/README.md),
"First login and remote access").
