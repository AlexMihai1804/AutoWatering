# Diagnostics Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdefd`  
**Properties:** Read, Write, Notify  
**Size:** 12 bytes  
**Description:** System health monitoring with uptime, error tracking, valve status, and battery level

## Overview

The Diagnostics characteristic provides comprehensive system health monitoring including uptime tracking, error counting, valve status monitoring, and battery level reporting. It enables proactive maintenance and system reliability assessment.

**Real-time Monitoring:** ✅ Current system health and valve states  
**Error Tracking:** ✅ Cumulative error count and last error code  
**Uptime Tracking:** ✅ Continuous operation time monitoring  
**Battery Monitoring:** ✅ Power level for portable systems  
**Fragmentation:** ❌ NOT REQUIRED - 12 bytes fit in single BLE packet  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct diagnostics_data {
    uint32_t uptime;             // System uptime in minutes (little-endian)
    uint16_t error_count;        // Total error count since boot (little-endian)
    uint8_t last_error;          // Last error code (0 = no error)
    uint8_t valve_status;        // Valve bitmap (bit 0 = channel 0, etc.)
    uint8_t battery_level;       // Battery percentage (0xFF = N/A)
    uint8_t reserved[3];         // Future expansion
} __packed;                      // TOTAL SIZE: 12 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-3 | 4 | `uptime` | System uptime in minutes (little-endian) |
| 4-5 | 2 | `error_count` | Total errors since boot (little-endian) |
| 6 | 1 | `last_error` | Last error code (0 = no recent error) |
| 7 | 1 | `valve_status` | Valve bitmap status |
| 8 | 1 | `battery_level` | Battery percentage (0xFF = N/A) |
| 9-11 | 3 | `reserved` | Future expansion (set to 0) |

### uptime (bytes 0-3, little-endian)
- **Purpose:** Continuous system operation time with persistent tracking
- **Unit:** Minutes (for sufficient precision and extended range)
- **Range:** 0 - 4,294,967,295 minutes (~8,174 years)
- **Calculation:** 
  - **RTC Available:** `(timezone_get_unix_utc() - boot_time_utc) / 60`
  - **RTC Unavailable:** `k_uptime_get() / (1000 * 60)` (fallback)
- **Persistence:** Maintains accurate uptime across system reboots using RTC
- **Usage:** True system stability monitoring and maintenance scheduling

### error_count (bytes 4-5, little-endian)
- **Purpose:** Cumulative error count since last boot
- **Range:** 0 - 65,535 errors
- **Increment:** On each system error detection
- **Reset:** Only on system restart
- **Monitoring:** Error trends and system health assessment

### last_error (byte 6)
- **Purpose:** Most recent error code for quick diagnosis
- **Range:** 0-255 (0 = no recent error)
- **Error Codes:**
  - `0` - No error
  - `1` - Flow sensor error
  - `2` - Unexpected flow detected
  - `3` - Valve failure
  - `4` - RTC error
  - `5` - Power supply issue
  - `6` - Communication error
  - `7` - Calibration error
  - `8-255` - Reserved for future error types

### valve_status (byte 7, bitmap)
- **Purpose:** Real-time status of all valves
- **Format:** Each bit represents a valve channel
  - Bit 0: Channel 0 (1=active, 0=inactive)
  - Bit 1: Channel 1 (1=active, 0=inactive)
  - Bit 2: Channel 2 (1=active, 0=inactive)
  - Bit 3: Channel 3 (1=active, 0=inactive)
  - Bit 4: Channel 4 (1=active, 0=inactive)
  - Bit 5: Channel 5 (1=active, 0=inactive)
  - Bit 6: Channel 6 (1=active, 0=inactive)
  - Bit 7: Channel 7 (1=active, 0=inactive)
- **Examples:**
  - `0x01` = Only channel 0 active
  - `0x05` = Channels 0 and 2 active
  - `0x00` = All valves closed

### battery_level (byte 8)
- **Purpose:** Battery level for portable systems
- **Range:** 0-100% (values 0-100)
- **Special Value:** `0xFF` = Not applicable (mains powered system)
- **Usage:** Low battery alerts and power management

### reserved (bytes 9-11)
- **Purpose:** Future system expansion
- **Current Value:** `0x00 0x00 0x00`
- **Future Use:** Temperature monitoring, performance stats, extended diagnostics

## Operations

### READ - Query System Health
Retrieve current system diagnostics and health status.

```javascript
const data = await diagnosticsChar.readValue();
const view = new DataView(data.buffer);

const uptimeMinutes = view.getUint32(0, true);  // little-endian
const errorCount = view.getUint16(4, true);     // little-endian
const lastError = view.getUint8(6);
const valveStatus = view.getUint8(7);
const batteryLevel = view.getUint8(8);

// Convert uptime to human-readable format
const uptimeHours = Math.floor(uptimeMinutes / 60);
const uptimeDays = Math.floor(uptimeHours / 24);

console.log(`System Uptime: ${uptimeDays} days, ${uptimeHours % 24} hours, ${uptimeMinutes % 60} minutes`);
console.log(`Total Errors: ${errorCount}`);

// Parse last error
const errorMessages = {
    0: 'No Error',
    1: 'Flow Sensor Error',
    2: 'Unexpected Flow',
    3: 'Valve Failure',
    4: 'RTC Error',
    5: 'Power Supply Issue',
    6: 'Communication Error',
    7: 'Calibration Error'
};

console.log(`Last Error: ${errorMessages[lastError] || `Unknown (${lastError})`}`);

// Parse valve status
console.log('Valve Status:');
for (let i = 0; i < 8; i++) {
    const isActive = (valveStatus & (1 << i)) !== 0;
    console.log(`  Channel ${i}: ${isActive ? 'ACTIVE' : 'inactive'}`);
}

// Battery level
if (batteryLevel === 0xFF) {
    console.log('Power: Mains powered (no battery)');
} else {
    console.log(`Battery: ${batteryLevel}%`);
    if (batteryLevel < 20) {
        console.log('⚠️ Low battery warning!');
    }
}
```

### WRITE - Reset Error Counters
Clear error counters for maintenance purposes.

```javascript
// Reset error count (maintenance operation)
async function resetErrorCounters() {
    const resetCommand = new ArrayBuffer(12);
    const view = new DataView(resetCommand);
    
    // Keep uptime, reset only error count and last error
    const currentData = await diagnosticsChar.readValue();
    const currentView = new DataView(currentData.buffer);
    
    view.setUint32(0, currentView.getUint32(0, true), true); // Keep uptime
    view.setUint16(4, 0, true);                              // Reset error_count
    view.setUint8(6, 0);                                     // Reset last_error
    view.setUint8(7, currentView.getUint8(7));              // Keep valve_status
    view.setUint8(8, currentView.getUint8(8));              // Keep battery_level
    
    await diagnosticsChar.writeValue(resetCommand);
    console.log('Error counters reset');
}

// System maintenance reset
async function performMaintenanceReset() {
    await resetErrorCounters();
    console.log('✅ Maintenance reset completed');
}
```

### NOTIFY - Health Status Changes
Receive notifications when system health changes.

```javascript
await diagnosticsChar.startNotifications();

diagnosticsChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const errorCount = view.getUint16(4, true);
    const lastError = view.getUint8(6);
    const valveStatus = view.getUint8(7);
    const batteryLevel = view.getUint8(8);
    
    // Check for new errors
    if (lastError !== 0) {
        const errorMessages = {
            1: 'Flow Sensor Error',
            2: 'Unexpected Flow Detected',
            3: 'Valve Failure',
            4: 'RTC Error',
            5: 'Power Supply Issue',
            6: 'Communication Error',
            7: 'Calibration Error'
        };
        
        console.log(`🚨 System Error: ${errorMessages[lastError] || `Code ${lastError}`}`);
        console.log(`   Total errors: ${errorCount}`);
        
        showErrorNotification(lastError, errorCount);
    }
    
    // Check valve status changes
    const activeValves = [];
    for (let i = 0; i < 8; i++) {
        if (valveStatus & (1 << i)) {
            activeValves.push(i);
        }
    }
    
    if (activeValves.length > 0) {
        console.log(`💧 Active valves: ${activeValves.join(', ')}`);
    }
    
    // Check battery level
    if (batteryLevel !== 0xFF && batteryLevel < 20) {
        console.log(`🔋 Low battery warning: ${batteryLevel}%`);
        showBatteryWarning(batteryLevel);
    }
    
    updateSystemHealthDisplay(data);
});

function showErrorNotification(errorCode, totalErrors) {
    const notification = document.createElement('div');
    notification.className = 'error-notification';
    notification.innerHTML = `
        <div class="error-header">⚠️ SYSTEM ERROR</div>
        <div class="error-code">Error Code: ${errorCode}</div>
        <div class="error-count">Total Errors: ${totalErrors}</div>
        <button onclick="acknowledgeError()">Acknowledge</button>
    `;
    
    document.body.appendChild(notification);
}

function showBatteryWarning(level) {
    const warning = document.createElement('div');
    warning.className = 'battery-warning';
    warning.innerHTML = `
        <div class="warning-header">🔋 LOW BATTERY</div>
        <div class="battery-level">Battery: ${level}%</div>
        <div class="warning-message">Please charge or replace battery</div>
    `;
    
    document.body.appendChild(warning);
}
```

## System Health Monitoring

### Health Score Calculation
```javascript
function calculateSystemHealth(diagnosticsData) {
    const view = new DataView(diagnosticsData.buffer);
    
    const uptimeMinutes = view.getUint32(0, true);
    const errorCount = view.getUint16(4, true);
    const lastError = view.getUint8(6);
    const batteryLevel = view.getUint8(8);
    
    let healthScore = 100; // Start with perfect health
    
    // Deduct for errors
    if (errorCount > 0) {
        const errorPenalty = Math.min(50, errorCount * 2); // Max 50 point penalty
        healthScore -= errorPenalty;
    }
    
    // Deduct for recent critical errors
    if (lastError >= 3 && lastError <= 5) { // Critical errors
        healthScore -= 20;
    }
    
    // Deduct for low battery
    if (batteryLevel !== 0xFF && batteryLevel < 20) {
        healthScore -= 15;
    }
    
    // Bonus for long uptime (stability)
    const uptimeHours = uptimeMinutes / 60;
    if (uptimeHours > 168) { // 1 week
        healthScore += 5;
    }
    
    return Math.max(0, Math.min(100, healthScore));
}

function getHealthStatus(score) {
    if (score >= 90) return { status: 'Excellent', color: 'green' };
    if (score >= 75) return { status: 'Good', color: 'lightgreen' };
    if (score >= 60) return { status: 'Fair', color: 'yellow' };
    if (score >= 40) return { status: 'Poor', color: 'orange' };
    return { status: 'Critical', color: 'red' };
}

// Display system health
function updateSystemHealthDisplay(diagnosticsData) {
    const score = calculateSystemHealth(diagnosticsData);
    const health = getHealthStatus(score);
    
    const healthElement = document.getElementById('system-health');
    healthElement.textContent = `System Health: ${health.status} (${score}%)`;
    healthElement.style.color = health.color;
}
```

### Diagnostic Reports
```javascript
class DiagnosticReporter {
    constructor(characteristic) {
        this.diagnosticsChar = characteristic;
        this.history = [];
    }
    
    async generateReport() {
        const data = await this.diagnosticsChar.readValue();
        const view = new DataView(data.buffer);
        
        const report = {
            timestamp: new Date(),
            uptime: {
                minutes: view.getUint32(0, true),
                hours: Math.floor(view.getUint32(0, true) / 60),
                days: Math.floor(view.getUint32(0, true) / (60 * 24))
            },
            errors: {
                total: view.getUint16(4, true),
                lastCode: view.getUint8(6)
            },
            valves: this.parseValveStatus(view.getUint8(7)),
            battery: view.getUint8(8) === 0xFF ? 'N/A' : `${view.getUint8(8)}%`,
            healthScore: calculateSystemHealth(data)
        };
        
        this.history.push(report);
        return report;
    }
    
    parseValveStatus(bitmap) {
        const status = {};
        for (let i = 0; i < 8; i++) {
            status[`channel_${i}`] = (bitmap & (1 << i)) !== 0;
        }
        return status;
    }
    
    exportReport(format = 'json') {
        const report = this.history[this.history.length - 1];
        
        if (format === 'json') {
            return JSON.stringify(report, null, 2);
        } else if (format === 'text') {
            return `
System Diagnostic Report
========================
Generated: ${report.timestamp.toLocaleString()}

System Uptime: ${report.uptime.days} days, ${report.uptime.hours % 24} hours
Total Errors: ${report.errors.total}
Last Error Code: ${report.errors.lastCode}
Battery Level: ${report.battery}
Health Score: ${report.healthScore}%

Valve Status:
${Object.entries(report.valves)
    .map(([channel, active]) => `  ${channel}: ${active ? 'ACTIVE' : 'inactive'}`)
    .join('\\n')}
            `.trim();
        }
    }
}

// Usage
const reporter = new DiagnosticReporter(diagnosticsChar);
const report = await reporter.generateReport();
console.log(reporter.exportReport('text'));
```

## Related Characteristics

- **[System Status](03-system-status.md)** - High-level system operational status
- **[Alarm Status](10-alarm-status.md)** - Critical alarms and error notifications
- **[Valve Control](01-valve-control.md)** - Valve status monitoring
- **[Flow Sensor](02-flow-sensor.md)** - Flow sensor health and errors
- **[System Configuration](06-system-configuration.md)** - Power management settings
