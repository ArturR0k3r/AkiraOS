# OTA Updates Guide

Over-the-air firmware update procedures for AkiraOS.

## Overview

AkiraOS uses MCUboot for secure OTA updates with rollback protection.

## Update Process

### 1. Build Firmware

```bash
cd ~/akira-workspace/AkiraOS
./build.sh -b esp32s3_devkitm_esp32s3_procpu
```

### 2. Upload via HTTP

```bash
curl -X POST \
  -F "firmware=@../build/zephyr/zephyr.bin" \
  http://192.168.1.100/ota/upload
```

### 3. Device Reboots

MCUboot verifies and swaps firmware images.

### 4. Verification

Check new version:
```bash
uart:~$ kernel version
```

## Rollback Protection

If new firmware fails to boot, MCUboot automatically rolls back to previous version.

## Security

- RSA/ECDSA signature verification
- Encrypted flash (optional)
- Version anti-rollback

## Related Documentation

- [OTA Manager Architecture](../architecture/connectivity.md#ota-manager)
- [MCUboot Documentation](https://mcuboot.com)
- [Security Model](../architecture/security.md)
