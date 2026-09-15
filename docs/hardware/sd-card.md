---
layout: default
title: SD Card Setup
parent: Hardware
nav_order: 2
---

# SD Card Setup

How to prepare and use a microSD card with AkiraConsole / AkiraOS.

---

## Supported Cards

| Parameter | Requirement |
|-----------|-------------|
| Form factor | microSD / microSDHC |
| Capacity | 64 MB – 32 GB (FAT32 range) |
| Speed class | Class 4 or faster (Class 10 / UHS-I recommended) |
| Filesystem | **FAT32 only** |

> **Not supported:** exFAT, NTFS, ext4, SDXC cards formatted as exFAT.  
> Cards larger than 32 GB are typically sold pre-formatted as exFAT and must be re-formatted as FAT32 before use.

---

## Formatting the Card

### Linux / WSL

```bash
# Find your card device (e.g. /dev/sdb, /dev/mmcblk0)
lsblk

# Unmount if auto-mounted
sudo umount /dev/sdX1

# Create a new MBR partition table and a single FAT32 partition
sudo parted /dev/sdX --script mklabel msdos
sudo parted /dev/sdX --script mkpart primary fat32 1MiB 100%

# Format
sudo mkfs.vfat -F 32 -n AKIRA /dev/sdX1
```

### Windows

1. Open **File Explorer** → right-click the SD card → **Format…**
2. Set **File system** to `FAT32`
3. Set **Allocation unit size** to `32768` (32 KB) — matches AkiraOS cluster size
4. Tick **Quick Format**
5. Click **Start**

> On Windows, the FAT32 option is hidden for cards larger than 32 GB.  
> Use [Rufus](https://rufus.ie) (select "FAT32" manually) or `diskpart` + `format fs=fat32 quick` for larger cards.

### macOS

```bash
# Find the disk identifier
diskutil list

# Erase and format as FAT32 (MS-DOS FAT)
diskutil eraseDisk FAT32 AKIRA MBRFormat /dev/diskN
```

---

## Required Directory Structure

AkiraOS expects this layout on the card root:

```
/SD:
└── apps/          ← WASM app bundles go here
    ├── hello.wasm
    ├── game.wasm
    └── ...
```

The `apps/` directory is created automatically on first mount if it does not exist.  
If it is missing (e.g. after re-formatting), insert the card and reboot — AkiraOS will recreate it.

---

## Installing Apps

Copy compiled `.wasm` files into `/apps/` on the card from a PC:

```bash
# Linux / WSL — mount and copy
sudo mount /dev/sdX1 /mnt/sd
cp myapp.wasm /mnt/sd/apps/
sudo umount /mnt/sd
```

On Windows/macOS, drag the `.wasm` file into the `apps` folder in File Explorer / Finder.

Once inserted and the device rebooted (or `app scan sd` run from the shell):

```
AkiraOS:~$ app scan sd
<inf> sd_manager: SD card available at /SD: (via sd_card driver)
<inf> sd_manager: Found 2 apps in /SD:/apps

=== sd Apps Found ===
  hello.wasm
  game.wasm
Total: 2
```

---

## Shell Commands

| Command | Description |
|---------|-------------|
| `app scan sd` | Detect card and list available apps |
| `app install <name>` | Install an app from `/SD:/apps/` to internal flash |
| `app list` | Show all installed apps |
| `app run <name>` | Run an installed app |

---

## Troubleshooting

### "SD card not present" at boot

The card was not detected during boot-time SPI probe.

1. Ensure the card is **fully seated** in the slot (push until it clicks).
2. Verify the card is **FAT32** formatted — exFAT is not recognized.
3. Try a different card; some no-name cards have poor SPI compatibility.
4. Enable debug logging to see the exact SPI error:
   ```
   # boards/akiraconsole_prod_esp32s3_procpu.conf
   CONFIG_SDHC_LOG_LEVEL_DBG=y
   CONFIG_SD_LOG_LEVEL_DBG=y
   ```
   Rebuild, flash, and check the serial output.

### "SD card available" but read errors on first access

```
<err> sd: Failed to read from SDMMC -116
```

`-116` is `ETIMEDOUT` — the card was slow to respond on the first SPI transaction after mount.  
This is a one-shot event common on cheap cards and does not indicate permanent failure.  
Run `app scan sd` again; subsequent accesses are typically clean.

### "Found 0 apps" on a card that has apps

- Confirm the files are in `/apps/` (not in a subfolder or the root).
- Confirm the extension is `.wasm` (lowercase).
- Check that the card was safely ejected after copying (flush write cache on the PC before removing).

### Card works on PC but not on AkiraConsole

- Re-format as FAT32 with 32 KB cluster size.
- Avoid cluster sizes above 32 KB — FatFs has a 64 MB limit per cluster at higher sizes.
- Avoid SD cards with hardware write-protection switches accidentally in the locked position.

---

## Technical Details

| Parameter | Value |
|-----------|-------|
| Interface | SPI (SPI2 bus) |
| Chip select | GPIO14 (`SD_CS`) — direct ESP32-S3 GPIO |
| Card detect | GPIO via TCA6408 P5 (`SD_DET`) — available when expander is fitted |
| Max SPI clock | 25 MHz |
| Mount point | `/SD:` |
| Apps directory | `/SD:/apps/` |
| Filesystem driver | FatFs (Elm ChaN) via `CONFIG_FAT_FILESYSTEM_ELM` |

---

*Last updated: 2026-09-14 (AkiraOS v1.6.5)*
