# BLE Troubleshooting Guide

## Common Issues and Solutions

### Connection Issues

#### Device Not Found
**Problem:** Device doesn't appear in Bluetooth scan  
**Causes:**
- Device not advertising
- Bluetooth not enabled on client device  
- Device already connected to another client
- Signal range issues

**Solutions:**
```javascript
// Check if Bluetooth is available
if (!navigator.bluetooth) {
    console.error('Bluetooth not supported');
    return;
}

// Request device with specific service
const device = await navigator.bluetooth.requestDevice({
    filters: [{ services: ['12345678-1234-5678-1234-56789abcdef0'] }],
    optionalServices: [] // Add optional services if needed
});
```

#### Connection Drops
**Problem:** Frequent disconnections  
**Causes:**
- Signal interference
- Power saving mode
- Too many concurrent operations
- BLE stack issues

**Solutions:**
```javascript
// Add connection monitoring
device.addEventListener('gattserverdisconnected', onDisconnected);

function onDisconnected(event) {
    console.log('Device disconnected, attempting reconnect...');
    setTimeout(reconnect, 1000);
}

async function reconnect() {
    try {
        await device.gatt.connect();
        console.log('Reconnected successfully');
    } catch (error) {
        console.error('Reconnect failed:', error);
    }
}
```

### Characteristic Access Issues

#### Write Failures
**Problem:** Write operations fail with errors  
**Common Error Codes:**
- `BT_ATT_ERR_VALUE_NOT_ALLOWED` - Invalid parameter value
- `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` - Wrong data size
- `BT_ATT_ERR_WRITE_NOT_PERMITTED` - Characteristic not writable

**Solutions:**
```javascript
// Validate data before writing
function validateValveControl(channelId, taskType, value) {
    if (channelId < 0 || channelId > 7) {
        throw new Error('Invalid channel_id: must be 0-7');
    }
    if (taskType !== 0 && taskType !== 1) {
        throw new Error('Invalid task_type: must be 0 or 1');
    }
    if (value <= 0 || value > 65535) {
        throw new Error('Invalid value: must be 1-65535');
    }
}

// Safe write function with validation
async function safeWriteValveControl(characteristic, channelId, taskType, value) {
    try {
        validateValveControl(channelId, taskType, value);
        
        const command = new ArrayBuffer(4);
        const view = new DataView(command);
        view.setUint8(0, channelId);
        view.setUint8(1, taskType);
        view.setUint16(2, value, true);
        
        await characteristic.writeValue(command);
        console.log('Write successful');
    } catch (error) {
        console.error('Write failed:', error);
        throw error;
    }
}
```

#### Notification Issues
**Problem:** Notifications not received  
**Causes:**
- Notifications not enabled
- Characteristic doesn't support notifications
- Rate limiting on device
- Connection issues

**Solutions:**
```javascript
// Proper notification setup
async function setupNotifications(characteristic) {
    try {
        // Check if notifications are supported
        if (!characteristic.properties.notify) {
            console.warn('Characteristic does not support notifications');
            return false;
        }
        
        // Enable notifications
        await characteristic.startNotifications();
        console.log('Notifications enabled');
        
        // Add event listener
        characteristic.addEventListener('characteristicvaluechanged', handleNotification);
        
        return true;
    } catch (error) {
        console.error('Failed to setup notifications:', error);
        return false;
    }
}

function handleNotification(event) {
    const data = event.target.value;
    console.log('Notification received:', data);
    // Process notification data
}
```

### Data Parsing Issues

#### Endianness Problems
**Problem:** Incorrect multi-byte value interpretation  
**Solution:** Always specify little-endian for this device

```javascript
// Correct way to read 16-bit values
const value = view.getUint16(offset, true); // true = little-endian

// Correct way to write 16-bit values  
view.setUint16(offset, value, true); // true = little-endian
```

#### Buffer Size Mismatch
**Problem:** DataView buffer size errors  
**Solution:** Always verify buffer size

```javascript
function safeGetUint32(dataView, offset) {
    if (dataView.byteLength < offset + 4) {
        throw new Error(`Buffer too small: need ${offset + 4} bytes, got ${dataView.byteLength}`);
    }
    return dataView.getUint32(offset, true);
}
```

### Performance Issues

#### Slow Response Times
**Problem:** Commands take too long to execute  
**Causes:**
- Multiple concurrent operations
- Large data transfers
- Network congestion
- Device processing delays

**Solutions:**
```javascript
// Implement command queuing
class BLECommandQueue {
    constructor() {
        this.queue = [];
        this.processing = false;
    }
    
    async executeCommand(command) {
        return new Promise((resolve, reject) => {
            this.queue.push({ command, resolve, reject });
            this.processQueue();
        });
    }
    
    async processQueue() {
        if (this.processing || this.queue.length === 0) {
            return;
        }
        
        this.processing = true;
        
        while (this.queue.length > 0) {
            const { command, resolve, reject } = this.queue.shift();
            
            try {
                const result = await command();
                resolve(result);
                // Add delay between commands
                await new Promise(resolve => setTimeout(resolve, 100));
            } catch (error) {
                reject(error);
            }
        }
        
        this.processing = false;
    }
}
```

#### Memory Leaks
**Problem:** Browser memory usage increases over time  
**Solution:** Proper cleanup of event listeners

```javascript
class BLEConnection {
    constructor() {
        this.characteristics = new Map();
        this.eventListeners = new Map();
    }
    
    async disconnect() {
        // Remove all event listeners
        for (const [characteristic, listeners] of this.eventListeners) {
            for (const listener of listeners) {
                characteristic.removeEventListener('characteristicvaluechanged', listener);
            }
        }
        
        // Clear maps
        this.characteristics.clear();
        this.eventListeners.clear();
        
        // Disconnect GATT
        if (this.device && this.device.gatt.connected) {
            this.device.gatt.disconnect();
        }
    }
}
```

## Debugging Tools

### BLE Inspector
```javascript
// Enable verbose logging
function enableBLEDebug() {
    const originalLog = console.log;
    console.log = function(...args) {
        const timestamp = new Date().toISOString();
        originalLog(`[${timestamp}] BLE:`, ...args);
    };
}

// Log all characteristic operations
async function debugReadCharacteristic(characteristic) {
    console.log(`Reading characteristic: ${characteristic.uuid}`);
    const data = await characteristic.readValue();
    console.log(`Read result:`, new Uint8Array(data.buffer));
    return data;
}

async function debugWriteCharacteristic(characteristic, data) {
    console.log(`Writing to characteristic: ${characteristic.uuid}`);
    console.log(`Write data:`, new Uint8Array(data));
    await characteristic.writeValue(data);
    console.log(`Write completed`);
}
```

### Error Recovery
```javascript
// Automatic error recovery
class BLEErrorRecovery {
    constructor(device) {
        this.device = device;
        this.retryCount = 0;
        this.maxRetries = 3;
    }
    
    async executeWithRetry(operation) {
        for (let i = 0; i < this.maxRetries; i++) {
            try {
                return await operation();
            } catch (error) {
                console.warn(`Operation failed (attempt ${i + 1}):`, error);
                
                if (i === this.maxRetries - 1) {
                    throw error;
                }
                
                // Wait before retry
                await new Promise(resolve => setTimeout(resolve, 1000 * (i + 1)));
                
                // Try to reconnect if needed
                if (!this.device.gatt.connected) {
                    await this.device.gatt.connect();
                }
            }
        }
    }
}
```

## Best Practices

### 1. Always Validate Input
```javascript
function validateInput(value, min, max, name) {
    if (typeof value !== 'number' || value < min || value > max) {
        throw new Error(`Invalid ${name}: must be ${min}-${max}, got ${value}`);
    }
}
```

### 2. Handle Disconnections Gracefully
```javascript
device.addEventListener('gattserverdisconnected', () => {
    console.log('Device disconnected');
    // Update UI state
    // Stop any ongoing operations
    // Offer reconnect option
});
```

### 3. Use Appropriate Data Types
```javascript
// Use typed arrays for binary data
const buffer = new ArrayBuffer(4);
const view = new DataView(buffer);
const uint8View = new Uint8Array(buffer);
```

### 4. Implement Timeouts
```javascript
async function withTimeout(promise, timeoutMs) {
    const timeout = new Promise((_, reject) => 
        setTimeout(() => reject(new Error('Operation timeout')), timeoutMs)
    );
    
    return Promise.race([promise, timeout]);
}

// Usage
await withTimeout(characteristic.readValue(), 5000);
```

### 5. Monitor Connection Quality
```javascript
function monitorConnectionQuality() {
    let missedNotifications = 0;
    let lastNotificationTime = Date.now();
    
    characteristic.addEventListener('characteristicvaluechanged', () => {
        const now = Date.now();
        const timeSinceLastNotification = now - lastNotificationTime;
        
        if (timeSinceLastNotification > 5000) { // 5 seconds
            console.warn('Possible connection quality issues');
        }
        
        lastNotificationTime = now;
    });
}
