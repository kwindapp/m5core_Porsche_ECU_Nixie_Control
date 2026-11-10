# AIR WERK – Porsche ECU CAN to Nixie Display

### Wireless Trijekt ECU Telemetry | Waveshare ESP32 | ESP-NOW | M5Stack Core | Arduino Mega | Nixie Tubes

An open-source project bringing modern engine telemetry and retro Nixie tube technology together in a classic Porsche 911.

The goal is simple: **Read live engine data from a modern ECU and display it wirelessly on a custom six-tube Nixie display — without adding modern-looking screens to the classic dashboard.**

## System Architecture

```text
Porsche 911 SC (1978)
       |
Trijekt Premium 3.0 ECU
       |
       | CAN Bus – 500 kbps
       | Protocol 10
       |
Waveshare ESP32 CAN Board
       |
       | ESP-NOW Wireless
       | 2.4 GHz / Channel 1
       |
M5Stack Core (ESP32)
       |
       | UART Serial – 9600 baud
       |
Arduino Mega 2560
       |
       | Nixie Driver Electronics
       |
6 × Nixie Tubes
AIR WERK DIN Radio Delete
```

## 1. Trijekt ECU – CAN Bus Integration

The Porsche 911 SC uses a 3.0-liter engine converted to electronic fuel injection and ignition, controlled by a Trijekt Premium ECU.

The ECU broadcasts engine data over its CAN interface using **Trijekt Protocol 10**.

Configuration:

- CAN bitrate: 500 kbps
- Standard 11-bit CAN identifiers
- CAN base IDs: 700 and 710 (decimal)
- Engine telemetry including RPM, engine temperature, battery voltage, throttle position and accelerator pedal position

## 2. Waveshare ESP32 – CAN Receiver and Wireless Transmitter

A Waveshare ESP32-based CAN board reads the ECU data and processes the incoming CAN frames.

The relevant measurements are converted into a compact telemetry packet and transmitted wirelessly using ESP-NOW.

ESP-NOW was selected because it provides direct ESP32-to-ESP32 communication without requiring a Wi-Fi router or internet connection.

The current telemetry packet contains:

| Data | Type |
|---|---|
| Engine RPM | `
