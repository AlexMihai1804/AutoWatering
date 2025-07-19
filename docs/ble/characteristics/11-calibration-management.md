# Calibration Management Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdefb`  
**Properties:** Read, Write, Notify  
**Size:** 13 bytes  
**Description:** Interactive flow sensor calibration with guided measurement process

## Overview

The Calibration Management characteristic provides an interactive calibration system for accurate flow sensor measurements. It guides users through a step-by-step process to measure actual water volume and calculate the precise pulses-per-liter ratio.

**Real-time Updates:** ✅ Notifications during calibration process  
**Interactive Process:** ✅ Guided step-by-step workflow  
**Automatic Calculation:** ✅ Precise pulses-per-liter ratio calculation  
**Fragmentation:** ❌ NOT REQUIRED - 13 bytes fit in single BLE packet  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct calibration_data {
    uint8_t action;              // 0=stop, 1=start, 2=in progress, 3=completed
    uint32_t pulses;             // Number of pulses counted (little-endian)
    uint32_t volume_ml;          // Volume in ml (little-endian)
    uint32_t pulses_per_liter;   // Calibration result (little-endian)
} __packed;                     // TOTAL SIZE: 13 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `action` | Process state (0=stop, 1=start, 2=progress, 3=complete) |
| 1-4 | 4 | `pulses` | Pulse count during calibration (little-endian) |
| 5-8 | 4 | `volume_ml` | Actual measured volume in ml (little-endian) |
| 9-12 | 4 | `pulses_per_liter` | Calculated calibration factor (little-endian) |

### action (byte 0)
- **Values:**
  - `0` = Stop calibration and calculate result
  - `1` = Start new calibration process
  - `2` = Calibration in progress (read-only status)
  - `3` = Calibration completed (read-only status)
- **Write:** Only 0 and 1 are valid for writes
- **Read:** All values may appear in reads

### pulses (bytes 1-4, little-endian)
- **Purpose:** Flow sensor pulse count during calibration
- **Range:** 0 to 4,294,967,295
- **Update:** Automatic during calibration process
- **Usage:** Real-time progress monitoring

### volume_ml (bytes 5-8, little-endian)
- **Purpose:** Actual measured volume in milliliters
- **Range:** 1 to 4,294,967,295 ml
- **Required:** Must provide when stopping calibration (action=0)
- **Validation:** Cannot be zero for calculation

### pulses_per_liter (bytes 9-12, little-endian)
- **Purpose:** Calculated calibration factor
- **Range:** Typically 100-10,000 pulses per liter
- **Formula:** `pulses_per_liter = (total_pulses × 1000) ÷ volume_ml`
- **Update:** Automatic calculation upon completion

## Operations

### READ - Query Calibration Status
Monitor calibration progress and retrieve results.

```javascript
const data = await calibChar.readValue();
const view = new DataView(data.buffer);

const action = view.getUint8(0);
const pulses = view.getUint32(1, true);  // little-endian
const volumeMl = view.getUint32(5, true);
const pulsesPerLiter = view.getUint32(9, true);

const actionNames = {
    0: 'Stopped',
    1: 'Starting',
    2: 'In Progress',
    3: 'Completed'
};

console.log(`Calibration Status: ${actionNames[action]}`);
console.log(`Pulses counted: ${pulses}`);
console.log(`Volume: ${volumeMl} ml`);
console.log(`Calibration factor: ${pulsesPerLiter} pulses/liter`);
```

### WRITE - Control Calibration Process
Start calibration or stop with measured volume.

```javascript
// Start calibration
async function startCalibration() {
    const startCommand = new ArrayBuffer(13);
    const view = new DataView(startCommand);
    
    view.setUint8(0, 1);        // action = start
    view.setUint32(1, 0, true); // pulses = 0
    view.setUint32(5, 0, true); // volume_ml = 0
    view.setUint32(9, 0, true); // pulses_per_liter = 0
    
    await calibChar.writeValue(startCommand);
    console.log('Calibration started - begin water flow measurement');
}

// Stop calibration with measured volume
async function stopCalibration(measuredVolumeMl) {
    const stopCommand = new ArrayBuffer(13);
    const view = new DataView(stopCommand);
    
    view.setUint8(0, 0);                          // action = stop
    view.setUint32(1, 0, true);                   // pulses (ignored on write)
    view.setUint32(5, measuredVolumeMl, true);    // volume_ml = measured
    view.setUint32(9, 0, true);                   // pulses_per_liter (calculated)
    
    await calibChar.writeValue(stopCommand);
    console.log(`Calibration stopped with ${measuredVolumeMl} ml measured`);
}

// Complete calibration workflow
async function calibrateFlowSensor() {
    try {
        // Start calibration
        await startCalibration();
        
        // User measures water (this is manual step)
        console.log('Please run water through the sensor and measure the volume...');
        
        // Simulate user measurement (replace with actual UI input)
        const measuredVolume = 1000; // 1 liter = 1000 ml
        
        // Stop calibration with measurement
        await stopCalibration(measuredVolume);
        
        // Read final result
        const result = await calibChar.readValue();
        const view = new DataView(result.buffer);
        const finalFactor = view.getUint32(9, true);
        
        console.log(`Calibration complete! Factor: ${finalFactor} pulses/liter`);
        
    } catch (error) {
        console.error('Calibration failed:', error);
    }
}
```

### NOTIFY - Real-time Progress Updates
Monitor calibration progress with live pulse counting.

```javascript
await calibChar.startNotifications();

calibChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const action = view.getUint8(0);
    const pulses = view.getUint32(1, true);
    const volumeMl = view.getUint32(5, true);
    const pulsesPerLiter = view.getUint32(9, true);
    
    switch(action) {
        case 1:
            console.log('🔧 Calibration started - pulse counter reset');
            updateCalibrationUI('started', pulses, 0, 0);
            break;
            
        case 2:
            console.log(`📊 Calibration in progress - ${pulses} pulses counted`);
            updateCalibrationUI('progress', pulses, 0, 0);
            break;
            
        case 3:
            console.log(`✅ Calibration completed!`);
            console.log(`   Final pulses: ${pulses}`);
            console.log(`   Measured volume: ${volumeMl} ml`);
            console.log(`   Calibration factor: ${pulsesPerLiter} pulses/liter`);
            updateCalibrationUI('completed', pulses, volumeMl, pulsesPerLiter);
            break;
            
        case 0:
            console.log('⏹️ Calibration stopped');
            updateCalibrationUI('stopped', pulses, volumeMl, pulsesPerLiter);
            break;
    }
});

function updateCalibrationUI(status, pulses, volume, factor) {
    const statusElement = document.getElementById('calib-status');
    const pulsesElement = document.getElementById('calib-pulses');
    const resultElement = document.getElementById('calib-result');
    
    statusElement.textContent = status.toUpperCase();
    pulsesElement.textContent = `Pulses: ${pulses}`;
    
    if (status === 'completed') {
        resultElement.textContent = `Factor: ${factor} pulses/liter (${volume} ml measured)`;
        resultElement.className = 'calibration-success';
    } else if (status === 'progress') {
        resultElement.textContent = 'Measuring... please wait';
        resultElement.className = 'calibration-progress';
    }
}
```

## Calibration Workflow

### Interactive Calibration Guide
```javascript
class CalibrationWizard {
    constructor(characteristic) {
        this.calibChar = characteristic;
        this.currentStep = 0;
        this.measuredVolume = 0;
    }
    
    async startWizard() {
        console.log('🔧 Flow Sensor Calibration Wizard');
        console.log('=====================================');
        
        await this.step1_preparation();
    }
    
    async step1_preparation() {
        console.log('Step 1: Preparation');
        console.log('- Get a measuring container (1-2 liters recommended)');
        console.log('- Ensure flow sensor is properly connected');
        console.log('- Make sure no other watering is active');
        
        // Wait for user confirmation
        await this.waitForConfirmation('Ready to start calibration?');
        await this.step2_startMeasurement();
    }
    
    async step2_startMeasurement() {
        console.log('Step 2: Starting Measurement');
        
        // Start calibration
        await this.calibChar.writeValue(this.createCommand(1, 0, 0, 0));
        console.log('✅ Calibration started');
        
        await this.step3_measureWater();
    }
    
    async step3_measureWater() {
        console.log('Step 3: Water Measurement');
        console.log('- Turn on the water valve');
        console.log('- Fill your measuring container');
        console.log('- Note the exact volume collected');
        console.log('- Monitoring pulse count...');
        
        // Monitor progress for 30 seconds max
        let monitoring = true;
        setTimeout(() => { monitoring = false; }, 30000);
        
        while (monitoring) {
            const data = await this.calibChar.readValue();
            const view = new DataView(data.buffer);
            const pulses = view.getUint32(1, true);
            
            process.stdout.write(`\\rPulses counted: ${pulses}`);
            await new Promise(resolve => setTimeout(resolve, 500));
        }
        
        console.log('\\n');
        await this.step4_enterVolume();
    }
    
    async step4_enterVolume() {
        console.log('Step 4: Enter Measured Volume');
        
        // In real implementation, get from user input
        this.measuredVolume = await this.getUserInput('Enter measured volume in ml: ');
        
        // Stop calibration with measured volume
        await this.calibChar.writeValue(
            this.createCommand(0, 0, this.measuredVolume, 0)
        );
        
        await this.step5_results();
    }
    
    async step5_results() {
        console.log('Step 5: Calibration Results');
        
        // Wait a moment for calculation
        await new Promise(resolve => setTimeout(resolve, 1000));
        
        const data = await this.calibChar.readValue();
        const view = new DataView(data.buffer);
        
        const pulses = view.getUint32(1, true);
        const volumeMl = view.getUint32(5, true);
        const pulsesPerLiter = view.getUint32(9, true);
        
        console.log('📊 Calibration Results:');
        console.log(`   Total pulses: ${pulses}`);
        console.log(`   Measured volume: ${volumeMl} ml`);
        console.log(`   Calibration factor: ${pulsesPerLiter} pulses/liter`);
        
        // Validate result
        if (pulsesPerLiter < 100 || pulsesPerLiter > 10000) {
            console.log('⚠️  Warning: Unusual calibration factor - please verify measurement');
        } else {
            console.log('✅ Calibration completed successfully!');
        }
    }
    
    createCommand(action, pulses, volume, factor) {
        const command = new ArrayBuffer(13);
        const view = new DataView(command);
        
        view.setUint8(0, action);
        view.setUint32(1, pulses, true);
        view.setUint32(5, volume, true);
        view.setUint32(9, factor, true);
        
        return command;
    }
    
    async waitForConfirmation(message) {
        // Simplified - in real implementation, show UI prompt
        console.log(message + ' (Press Enter to continue)');
        return new Promise(resolve => {
            process.stdin.once('data', () => resolve());
        });
    }
    
    async getUserInput(prompt) {
        // Simplified - in real implementation, show input dialog
        console.log(prompt);
        return 1000; // Default 1 liter for demo
    }
}

// Usage
const wizard = new CalibrationWizard(calibChar);
await wizard.startWizard();
```

### Calibration Validation
```javascript
// Validate calibration factor
function validateCalibrationFactor(pulsesPerLiter, measuredVolume, totalPulses) {
    const validation = {
        isValid: true,
        warnings: [],
        recommendations: []
    };
    
    // Check factor range
    if (pulsesPerLiter < 100) {
        validation.warnings.push('Very low calibration factor - check sensor sensitivity');
    } else if (pulsesPerLiter > 10000) {
        validation.warnings.push('Very high calibration factor - check for sensor noise');
    }
    
    // Check measurement volume
    if (measuredVolume < 500) {
        validation.warnings.push('Small measurement volume may reduce accuracy');
        validation.recommendations.push('Use at least 1 liter for better precision');
    }
    
    // Check pulse count
    if (totalPulses < 50) {
        validation.warnings.push('Low pulse count may indicate flow sensor issues');
    }
    
    // Calculate precision
    const pulsesPerMl = pulsesPerLiter / 1000;
    const precision = 1 / pulsesPerMl; // ml per pulse
    
    validation.precision = precision;
    validation.recommendations.push(`Measurement precision: ±${precision.toFixed(2)} ml per pulse`);
    
    return validation;
}

// Apply calibration to system
async function applyCalibration(pulsesPerLiter) {
    // Update system configuration with new calibration
    // This would integrate with System Configuration characteristic
    
    console.log(`Applying calibration factor: ${pulsesPerLiter} pulses/liter`);
    
    // Save to system configuration
    // await updateSystemConfig({ flow_calibration: pulsesPerLiter });
    
    console.log('✅ Calibration applied to system');
}
```

## Troubleshooting

### Common Calibration Issues
- **No pulses detected:** Check flow sensor connection and power
- **Inconsistent readings:** Ensure steady water flow during measurement
- **Very high/low factors:** Verify sensor type and measurement accuracy
- **Calibration timeouts:** Check for water flow and sensor responsiveness

### Calibration Best Practices
- Use measuring containers of 1-2 liters for accuracy
- Maintain steady flow rate during measurement
- Perform calibration with clean water
- Recalibrate after sensor maintenance or replacement

## Related Characteristics

- **[System Configuration](06-system-configuration.md)** - Stores calibration factor
- **[Flow Sensor](02-flow-sensor.md)** - Uses calibration for volume conversion
- **[Statistics](08-statistics.md)** - Volume calculations depend on calibration
- **[Diagnostics](13-diagnostics.md)** - Calibration validation and sensor testing
