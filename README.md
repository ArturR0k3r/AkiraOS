
# Bootload 
```shell
west build --pristine -b nrf5340dk_nrf5340_cpuapp ~/zephyrdemo/bootloader/mcuboot/boot/zephyr -d build-mcuboot -- -DCONFIG_BOOT_MAX_IMG_SECTORS=1024 -DDTC_OVERLAY_FILE="~/zephyrdemo/application/boards/nrf5340dk_nrf5340_cpuapp.overlay;~/zephyrdemo/bootloader/mcuboot/boot/zephyr/app.overlay" -DMODULE_EXT_ROOT=../../../../application
```
# Firmware
```shell
west build --pristine -b nrf5340dk_nrf5340_cpuapp ~/zephyrdemo/application -d build  -- -DCONFIG_BOOTLOADER_MCUBOOT=y  -DCONFIG_MCUBOOT_SIGNATURE_KEY_FILE=\"bootloader/mcuboot/root-rsa-2048.pem\" -DMODULE_EXT_ROOT=.
```