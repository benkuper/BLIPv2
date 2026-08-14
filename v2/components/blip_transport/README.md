# blip_transport

Owns the versioned transport envelope and Serial implementation, plus future BLE,
ESP-NOW, and compatibility protocol adapters. Wi-Fi station/AP lifecycle and IP
provisioning live in `blip_network`; network protocols consume its `transport.wifi`
service without taking ownership of the radio lifecycle.
