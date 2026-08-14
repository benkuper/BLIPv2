# Firmware OTA component

`blip_ota` owns bounded application-image streaming into the inactive ESP-IDF
OTA slot. The host-portable update service validates declared project, target,
profile, image version, exact length, SHA-256, and the embedded ESP application
descriptor before activation. ESP-IDF then validates the image structure, chip
compatibility, checksum, and—when secure signed apps are configured—the image
signature.

The boot partition is changed only after every check succeeds. An interrupted or
rejected transfer therefore leaves the running slot selected. A newly booted
image remains pending until the composition root confirms it after registry,
settings migration, and diagnostics startup; reset or panic before confirmation
lets the ESP-IDF bootloader roll back on the next boot.
