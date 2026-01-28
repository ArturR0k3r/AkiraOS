AkiraOS Connectivity Layer (v1.4.x)

A unified data transmission and network services layer.

1. High-Level Architecture

The Connectivity Layer operates as a "black box" for receiving and transmitting data. It remains completely unaware of the WASM runtime's existence and communicates exclusively with the App Loader and FS Manager via standardized events and callbacks.

2. Module Structure

2.1 Transports

Each transport is handled as an independent thread or service within Zephyr 4.3.0:

Bluetooth (BLE): Custom GATT services for firmware management, remote control, and app delivery.

WiFi: Native TCP/IP stack for high-speed data transfer (primarily for ESP32-S3/C6).

USB: MSC (Mass Storage Class) for "drag-and-drop" file loading and CDC for the System Shell.

AkiraMesh: A low-level, decentralized protocol operating over LoRa (LR1121) or FSK (CC1101).

2.2 Connectivity Manager

File: src/connectivity/conn_manager.c

Role: Traffic routing and session management.

Logic: When a file arrives via BLE or WiFi, the manager determines whether to route it to the fs_manager for persistent storage or directly to the app_loader for immediate execution.

2.3 Cloud Client (AkiraHub Interface)

File: src/connectivity/cloud/akira_hub_client.c

Role: Synchronization with the AkiraHub ecosystem.

Logic:

Implements CoAP/MQTT protocols for low-power cloud communication.

Sends "Heartbeat" telemetry (battery status, firmware version, system health).

Handles remote requests for the application catalog and OTA triggers.

2.4 OTA Engine

File: src/connectivity/ota/ota_engine.c

Role: Secure system and application updates.

Logic: Utilizes MCUboot for primary firmware updates and internal atomic swapping mechanisms for updating standalone WASM modules.

3. The "Loosely Coupled" Principle

Isolation: The Connectivity layer never calls runtime functions directly.

Hardware Safety: It has no access to display or sensor drivers.

Communication: All interaction is handled via an asynchronous Event Bus or direct app_loader_receive_chunk() calls to prevent blocking the network stack.

4. Roadmap (v1.4.5+)

End-to-End Encryption: Implementation of hardware-accelerated encryption for all AkiraMesh packets.

Multi-Transport Bonding: Support for concurrent active connections (e.g., maintaining a BLE control link while downloading via WiFi).