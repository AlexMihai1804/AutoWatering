# Fragmentation Reference (BLE Characteristics)

> **Note**: For detailed implementation analysis and status of each characteristic, see [Fragmentation Details](../fragmentation-details.md).

## Overview
The AutoWatering BLE service uses specific fragmentation strategies to handle data larger than the default BLE MTU (23 bytes).

## Fragmentation Strategies

### 1. Unified 8-Byte Header (`history_fragment_header_t`)
Used for complex data transfer where metadata is required.
*   **Structure**: `[Type(1)][Status(1)][Count(2)][FragIdx(1)][TotalFrags(1)][Size(1)][Rsvd(1)]`
*   **Used By**:
    *   **Environmental History**: Payload up to 232 bytes (Requires MTU negotiation).
    *   **Onboarding Status**: Adapts payload size to current MTU.
    *   **Auto Calc Status**: (Implementation attempts this, but currently blocked by notification limit).

### 2. Custom 3-Byte Header
Used for simple splitting of fixed-size structures.
*   **Structure**: `[Seq(1)][Total(1)][Len(1)] + Payload`
*   **Used By**:
    *   **Environmental Data**: Splits 26-byte payload into small chunks safe for any MTU.

### 3. Delta Updates
Avoids fragmentation by sending only changed fields.
*   **Used By**:
    *   **Rain Integration**: Sends 10-byte delta instead of full 78-byte structure.

### 4. Write Fragmentation (Client -> Device)
Allows clients to write large configurations.
*   **Structure**: `[Channel(1)][Type(1)][TotalSize(2)] + Payload`
*   **Used By**:
    *   **Channel Config**
    *   **Growing Environment**

## Known Limitations
Several characteristics attempt to send full structures via a notification function (`advanced_notify`) that enforces a **23-byte limit**. These notifications currently fail on the device side:
*   **Channel Config** (76 bytes)
*   **Growing Environment** (71 bytes)
*   **Auto Calc Status** (72 bytes)
*   **System Config** (56 bytes)

*Clients should rely on **Reading** these characteristics rather than waiting for notifications until this firmware limitation is resolved.*
