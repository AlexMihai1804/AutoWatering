# BLE Fragmentation & Large Data Transfer Guide

This document details the fragmentation strategies used in the AutoWatering BLE API to handle data payloads larger than the standard BLE MTU (typically 23-247 bytes).

## Overview

The system employs two distinct strategies depending on the direction of data transfer:

1.  **Write Fragmentation (Client -> Device)**: A custom header-based protocol used for configuring complex characteristics (e.g., Channel Config, Growing Environment).
2.  **Notification Fragmentation (Device -> Client)**:
    *   **Unified History Protocol**: A header-based streaming protocol for large datasets (History, Auto Calc).
    *   **Implicit MTU**: Relies on the BLE stack's ability to handle large notifications if the client supports it (Channel Config, System Config).

---

## 1. Write Fragmentation (Client -> Device)

Used when a client needs to write a structure larger than the negotiated MTU. The client splits the data into chunks, and the firmware reassembles them.

**Applicable Characteristics:**
*   **Channel Configuration** (UUID: `...1237`)
*   **Growing Environment** (UUID: `...1241`)

### Protocol Structure

The write sequence consists of an **Initial Header Packet** followed by one or more **Continuation Packets**.

#### A. Initial Header Packet
Must be the first packet sent. It initializes the reassembly buffer.

| Byte Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0 | `Channel ID` | 1 Byte | Target Channel (0-7) |
| 1 | `Frag Type` | 1 Byte | Type of data being sent (see below) |
| 2-3 | `Total Size` | 2 Bytes | Total expected size of the payload (Little Endian or Big Endian depending on Type) |
| 4+ | `Data...` | Variable | Start of the payload data (optional in header) |

**Fragmentation Types (`Frag Type`):**
*   `0x01`: **Name Update Only** (Channel Config). Payload is a null-terminated string.
*   `0x02`: **Full Structure (Big Endian)**. Payload is the complete C-struct.
*   `0x03`: **Full Structure (Little Endian)**. Payload is the complete C-struct.

#### B. Continuation Packets
Subsequent writes contain only raw data, which is appended to the buffer until `Total Size` is reached.

| Byte Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0+ | `Data...` | Variable | Raw payload data |

### Firmware Logic
*   **Timeout**: The reassembly buffer resets if no packet is received within **5 seconds**.
*   **Validation**: The write is only processed once `received_bytes == total_size`.
*   **State Machine**: Handled by `channel_frag` and `growing_env_frag` structures in `src/bt_irrigation_service.c`.

---

## 2. Notification Fragmentation (Device -> Client)

Used when the device needs to send large datasets or structures to the client.

### Strategy A: Unified History Protocol (Streaming)

Used for characteristics that return variable-length lists of records or complex statuses that exceed a single packet.

**Applicable Characteristics:**
*   **Rain History** (UUID: `...1247`)
*   **Environmental History** (UUID: `...1249`)
*   **Auto Calc Status** (UUID: `...1242`) - *Uses this header even for single-packet updates.*

#### Header Structure (`history_fragment_header_t`)
Every notification packet starts with this 8-byte header.

| Byte Offset | Field | Size | Description |
| :--- | :--- | :--- | :--- |
| 0 | `Data Type` | 1 Byte | Context-specific type (e.g., 0=Hourly, 1=Daily) |
| 1 | `Status` | 1 Byte | 0=OK, Non-zero=Error Code |
| 2-3 | `Entry Count` | 2 Bytes | Number of records in this fragment |
| 4 | `Frag Index` | 1 Byte | Current fragment number (0-based) |
| 5 | `Total Frags` | 1 Byte | Total number of fragments to expect |
| 6 | `Frag Size` | 1 Byte | Size of the payload in this fragment |
| 7 | `Reserved` | 1 Byte | Padding/Future use |
| 8+ | `Payload` | Variable | Array of records or data structure |

#### Streaming Behavior
1.  **Request**: Client writes a command to the characteristic (e.g., "Get Last 24h").
2.  **Preparation**: Firmware fetches data, calculates required fragments.
3.  **Streaming**: Firmware sends notifications sequentially (`Frag 0` -> `Frag 1` -> ... -> `Frag N`).
4.  **Completion**: Client reconstructs the full dataset once all fragments are received.

---

### Strategy B: Implicit MTU (Large Notifications)

Used for characteristics that are large but typically fit within a modern BLE connection's MTU (e.g., 76 bytes).

**Applicable Characteristics:**
*   **Channel Configuration** (76 bytes)
*   **System Configuration** (56 bytes)
*   **Rain Integration Status** (78 bytes - Full Update)

#### Mechanism
The firmware uses `advanced_notify` (or `safe_notify`) to attempt to send the entire structure in one notification.

1.  **MTU Check**: The stack checks if the payload fits the negotiated MTU.
2.  **Transmission**:
    *   **High MTU (e.g., 247)**: Sent as a single LE packet.
    *   **Low MTU (e.g., 23)**: The operation may fail or be truncated depending on the stack configuration. *Note: The client is expected to request a larger MTU (e.g., 512) during connection setup.*

---

## 3. Delta Updates (Optimization)

**Applicable Characteristics:**
*   **Rain Integration Status** (UUID: `...124D`)

To save bandwidth, this characteristic supports two notification formats:

1.  **Delta Update (Default)**: A small packet containing only changed fields (e.g., `status`, `mode`, `timer`). Sent frequently.
2.  **Full Update**: The complete 78-byte structure. Sent only upon request or significant state transitions.

The client distinguishes these by the packet length.
