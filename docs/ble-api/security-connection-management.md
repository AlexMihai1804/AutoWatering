# BLE Connection Management & Current Security Posture

Status: VERIFIED (aligned Jan 2026)

This file now focuses on factual connection & resource management behavior actually implemented in firmware. Earlier versions overstated security guarantees; see updated `security-pairing.md` for the explicit minimal security statement. References to role-based authorization, enforced authentication, or multi-client arbitration beyond a single connection have been removed where speculative.

## 1. Security Configuration (What Exists)

`CONFIG_BT_SMP=y` and `CONFIG_BT_SETTINGS=y` enable the Zephyr stack to perform bonding *if a central initiates it*. The application layer does **not** enforce a required security level; all characteristics accept access when syntactically valid. Single connection + (optionally) one bond slot form the only practical gate.

```c
// Configuration in prj.conf
CONFIG_BT_SMP=y                    // Enable Security Manager Protocol
CONFIG_BT_SETTINGS=y               // Enable persistent security settings
CONFIG_BT_MAX_PAIRED=1             // Support one paired device
```

### Authentication & Pairing (Behavior)

| Aspect | Current Behavior | Notes |
| --- | --- | --- |
| Pairing trigger | Central-initiated only | Device never forces it |
| Required level | None | Handlers lack `bt_conn_get_security` checks |
| Bond storage | Supported (1 slot) | Provides faster reconnection only |
| Enforcement on write | Not present | Future roadmap item |
| Data at rest | Plain NVS | No encryption wrapper |

See roadmap in `security-pairing.md` for planned improvements.

### Device Information Service (DIS)

The system exposes device information for identification:

```c
CONFIG_BT_DIS=y                           // Enable Device Information Service
CONFIG_BT_DIS_SERIAL_NUMBER=y             // Expose serial number
CONFIG_BT_DIS_SERIAL_NUMBER_STR="000001"  // Default serial number
CONFIG_BT_DIS_FW_REV=y                    // Expose firmware revision
CONFIG_BT_DIS_FW_REV_STR="1.0"           // Firmware version
CONFIG_BT_DIS_HW_REV=y                    // Expose hardware revision
CONFIG_BT_DIS_HW_REV_STR="v1"            // Hardware version
```

## 2. Connection Management

### Device Advertising

#### Advertising Parameters

```c
static struct bt_le_adv_param adv_param = {
    .options      = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
    .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,  // 20ms
    .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,  // 30ms
};
```

#### Advertising Data

```c
#define DEVICE_NAME "AutoWatering"

static const struct bt_data adv_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, sizeof(DEVICE_NAME) - 1),
};

static const struct bt_data adv_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, IRRIGATION_SERVICE_UUID),
};
```

#### Device Appearance

```c
CONFIG_BT_DEVICE_APPEARANCE=833  // Generic Remote Control (irrigation controller)
```

### Connection Parameters (Negotiated / Preferred)

#### Preferred Connection Parameters

The system negotiates optimized connection parameters for irrigation control:

```c
// Configuration in prj.conf
CONFIG_BT_PERIPHERAL_PREF_MIN_INT=24    // 30ms minimum interval
CONFIG_BT_PERIPHERAL_PREF_MAX_INT=40    // 50ms maximum interval  
CONFIG_BT_PERIPHERAL_PREF_LATENCY=0     // No slave latency
CONFIG_BT_PERIPHERAL_PREF_TIMEOUT=42    // 420ms supervision timeout
```

#### Dynamic Connection Parameter Update

```c
static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed with error: %u\n", err);
        return;
    }
    
    // Store connection reference
    default_conn = bt_conn_ref(conn);
    connection_active = true;
    
    // Negotiate supervision timeout of 4 seconds (400x10 ms)
    const struct bt_le_conn_param conn_params = {
        .interval_min = BT_GAP_INIT_CONN_INT_MIN,  // 24 (30ms)
        .interval_max = BT_GAP_INIT_CONN_INT_MAX,  // 40 (50ms)
        .latency = 0,                              // No latency
        .timeout = 400,                            // 4 seconds
    };
    
    int update_err = bt_conn_le_param_update(conn, &conn_params);
    if (update_err) {
        printk("Connection parameter update failed: %d\n", update_err);
    } else {
        printk("Connection parameters updated successfully\n");
    }
    
    // Initialize connection state
    memset(&notification_state, 0, sizeof(notification_state));
    init_notification_pool();
    
    printk("Connected to irrigation controller\n");
}
```

#### Rationale

| Parameter | Value | Rationale |
| --- | --- | --- |
| **Interval** | 30-50ms | Balance between responsiveness and power consumption |
| **Latency** | 0 | Immediate response for irrigation control |
| **Timeout** | 4 seconds | Reliable connection with reasonable recovery time |

### Connection State Management

#### Connection Callbacks

```c
BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed\n");
        connection_active = false;
        default_conn = NULL;
        return;
    }
    
    // Successful connection setup
    default_conn = bt_conn_ref(conn);
    connection_active = true;
    
    // Reset all notification states
    memset(&notification_state, 0, sizeof(notification_state));
    
    // Initialize buffer pool
    init_notification_pool();
    
    // Auto-enable all notifications for better user experience
    notification_state.valve_notifications_enabled = true;
    notification_state.flow_notifications_enabled = true;
    notification_state.status_notifications_enabled = true;
    notification_state.task_notifications_enabled = true;
    notification_state.schedule_notifications_enabled = true;
    notification_state.system_notifications_enabled = true;
    notification_state.statistics_notifications_enabled = true;
    notification_state.rtc_notifications_enabled = true;
    notification_state.alarm_notifications_enabled = true;
    notification_state.calibration_notifications_enabled = true;
    notification_state.history_notifications_enabled = true;
    notification_state.diagnostics_notifications_enabled = true;
    notification_state.growing_env_notifications_enabled = true;
    notification_state.auto_calc_notifications_enabled = true;
    notification_state.current_task_notifications_enabled = true;
    notification_state.timezone_notifications_enabled = true;
    
    printk("All notifications auto-enabled for new connection\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("Disconnected with reason: %u\n", reason);
    
    // Critical: Stop task update thread immediately to prevent freeze
    if (task_update_thread_active) {
        task_update_thread_active = false;
    }
    
    // Mark connection as inactive
    connection_active = false;
    
    // Clean up connection reference
    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }
    
    // Reset notification system
    init_notification_pool();
    buffer_pool_maintenance();
    
    // Clear all valve values and reset notification states
    memset(&notification_state, 0, sizeof(notification_state));
    
    // Schedule advertising restart
    k_work_schedule(&adv_restart_work, K_MSEC(500));
    
    printk("Connection cleanup completed\n");
}
```

### Multi-Client Handling

Only one active connection is supported. Additional central connection attempts are rejected at controller level (`CONFIG_BT_MAX_CONN=1`). There is no application queueing / handover; a new connection requires the existing central to disconnect (or timeout).

```c
CONFIG_BT_MAX_CONN=1  // Maximum one concurrent connection

// Connection validation
static bool validate_connection(struct bt_conn *conn) {
    if (!connection_active || !default_conn) {
        return false;
    }
    
    // Ensure this is the authorized connection
    if (conn != default_conn) {
        return false;
    }
    
    return true;
}
```

## 3. Advertising Management

### Automatic Advertising Restart

The system implements robust advertising restart after disconnection:

```c
static void adv_restart_work_handler(struct k_work *work) {
    int err;
    int retry_count = 0;
    const int max_retries = 3;
    
    printk("Starting advertising restart work handler\n");
    
    // Try to stop any existing advertiser first
    err = bt_le_adv_stop();
    printk("Advertising stop result: %d\n", err);
    
    // Retry loop with linear backoff
    while (retry_count < max_retries) {
        // Wait before attempting restart
        uint32_t delay_ms = 200 + (100 * retry_count);
        printk("Waiting %ums before advertising restart attempt %d\n", 
               delay_ms, retry_count + 1);
        k_sleep(K_MSEC(delay_ms));
        
        // Attempt to start advertising
        printk("Attempting to start advertising\n");
        err = bt_le_adv_start(&adv_param,
                              adv_ad, ARRAY_SIZE(adv_ad),
                              adv_sd, ARRAY_SIZE(adv_sd));
        
        if (err == 0) {
            printk("Advertising restarted successfully\n");
            return;
        }
        
        if (err == -EALREADY) {
            printk("Advertising already active\n");
            return;
        }
        
        printk("Advertising restart failed with error: %d, retrying\n", err);
        retry_count++;
    }
    
    printk("Failed to restart advertising after %d attempts\n", max_retries);
    
    // If all retries failed, schedule another attempt in 5 seconds
    k_work_schedule(&adv_restart_work, K_MSEC(5000));
}

// Delayed work item for advertising restart
static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_handler);
```

### Advertising Error Handling

```c
// Common advertising error codes and handling
switch (err) {
    case 0:
        printk("Advertising started successfully\n");
        break;
        
    case -EALREADY:
        printk("Advertising already active\n");
        break;
        
    case -EINVAL:
        printk("Invalid advertising parameters\n");
        break;
        
    case -ENOTSUP:
        printk("Advertising not supported\n");
        break;
        
    case -ENOMEM:
        printk("Insufficient memory for advertising\n");
        break;
        
    default:
        printk("Advertising failed with error: %d\n", err);
        break;
}
```

## 4. Buffer and Memory Management

### BLE Buffer Configuration

Optimized buffer configuration for stable notifications:

```c
// Enhanced buffer configuration for notification stability
CONFIG_BT_BUF_ACL_TX_COUNT=12      // 12 ACL TX buffers
CONFIG_BT_BUF_ACL_TX_SIZE=251      // 251 bytes per buffer
CONFIG_BT_BUF_EVT_RX_COUNT=16      // 16 event RX buffers
CONFIG_BT_BUF_EVT_RX_SIZE=255      // 255 bytes per buffer
CONFIG_BT_L2CAP_TX_MTU=247         // 247 bytes L2CAP MTU
CONFIG_BT_ATT_TX_COUNT=12          // 12 ATT TX buffers
```

### Connection Limits

```c
CONFIG_BT_MAX_CONN=1               // Single connection support
CONFIG_BT_MAX_PAIRED=1             // Single paired device
CONFIG_BT_CONN_TX_MAX=16           // 16 TX buffers per connection
```

### Thread Stack Sizes

```c
CONFIG_BT_HCI_TX_STACK_SIZE=2048   // HCI TX thread stack
CONFIG_BT_RX_STACK_SIZE=2560       // BLE RX thread stack
```

## 5. Connection Optimization

### MTU and Data Length

```c
CONFIG_BT_L2CAP_TX_MTU=247         // Maximum transmission unit
CONFIG_BT_CTLR_DATA_LENGTH_MAX=251 // Maximum data length extension
CONFIG_BT_BUF_ACL_RX_SIZE=255      // ACL RX buffer size
```

### PHY Configuration

```c
CONFIG_BT_CTLR_PHY_2M=y            // Enable 2M PHY for higher throughput
CONFIG_BT_CTLR_TX_PWR_PLUS_4=y     // +4dBm transmission power
```

### Connection Parameter Updates

```c
CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=y    // Automatic parameter negotiation
CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT=5000   // 5 second update timeout
```

## 6. Platform Implementation Sketches (Trimmed)

### JavaScript/Web Bluetooth (Generic Skeleton)

```javascript
class BLEConnectionManager {
    constructor() {
        this.device = null;
        this.server = null;
        this.service = null;
        this.characteristics = new Map();
        this.connectionState = 'disconnected';
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 3;
    }
    
    async connect() {
        try {
            console.log('Requesting Bluetooth device...');
            
            this.device = await navigator.bluetooth.requestDevice({
                filters: [
                    { name: 'AutoWatering' },
                    { services: ['your-service-uuid'] }
                ],
                optionalServices: ['your-service-uuid']
            });
            
            console.log('Device selected:', this.device.name);
            
            // Add disconnect event listener
            this.device.addEventListener('gattserverdisconnected', 
                this.onDisconnected.bind(this));
            
            // Connect to GATT server
            console.log('Connecting to GATT server...');
            this.server = await this.device.gatt.connect();
            
            // Get primary service
            console.log('Getting primary service...');
            this.service = await this.server.getPrimaryService('your-service-uuid');
            
            // Initialize characteristics
            await this.initializeCharacteristics();
            
            this.connectionState = 'connected';
            this.reconnectAttempts = 0;
            
            console.log('Successfully connected to AutoWatering device');
            return true;
            
        } catch (error) {
            console.error('Connection failed:', error);
            this.connectionState = 'disconnected';
            throw error;
        }
    }
    
    async initializeCharacteristics() {
        const characteristicUUIDs = [
            'valve-control-uuid',
            'flow-sensor-uuid',
            'system-status-uuid',
            // Add other characteristic UUIDs...
        ];
        
        for (const uuid of characteristicUUIDs) {
            try {
                const characteristic = await this.service.getCharacteristic(uuid);
                this.characteristics.set(uuid, characteristic);
                
                // Subscribe to notifications if supported
                if (characteristic.properties.notify) {
                    await characteristic.startNotifications();
                    characteristic.addEventListener('characteristicvaluechanged', 
                        this.onCharacteristicChanged.bind(this));
                    console.log(`Subscribed to notifications for ${uuid}`);
                }
            } catch (error) {
                console.warn(`Failed to initialize characteristic ${uuid}:`, error);
            }
        }
    }
    
    onDisconnected(event) {
        console.log('Device disconnected');
        this.connectionState = 'disconnected';
        this.server = null;
        this.service = null;
        this.characteristics.clear();
        
        // Attempt automatic reconnection
        if (this.reconnectAttempts < this.maxReconnectAttempts) {
            console.log(`Attempting reconnection ${this.reconnectAttempts + 1}/${this.maxReconnectAttempts}`);
            setTimeout(() => this.reconnect(), 2000 * (this.reconnectAttempts + 1));
        } else {
            console.log('Maximum reconnection attempts reached');
        }
    }
    
    async reconnect() {
        try {
            this.reconnectAttempts++;
            this.connectionState = 'reconnecting';
            
            if (this.device && this.device.gatt) {
                this.server = await this.device.gatt.connect();
                this.service = await this.server.getPrimaryService('your-service-uuid');
                await this.initializeCharacteristics();
                
                this.connectionState = 'connected';
                this.reconnectAttempts = 0;
                console.log('Reconnection successful');
            }
        } catch (error) {
            console.error('Reconnection failed:', error);
            this.connectionState = 'disconnected';
            
            if (this.reconnectAttempts < this.maxReconnectAttempts) {
                setTimeout(() => this.reconnect(), 2000 * (this.reconnectAttempts + 1));
            }
        }
    }
    
    async disconnect() {
        if (this.server && this.server.connected) {
            this.server.disconnect();
        }
        this.connectionState = 'disconnected';
        console.log('Manually disconnected');
    }
    
    isConnected() {
        return this.connectionState === 'connected' && 
               this.server && this.server.connected;
    }
    
    getConnectionState() {
        return this.connectionState;
    }
}

// Usage example
const connectionManager = new BLEConnectionManager();

async function connectToDevice() {
    try {
        await connectionManager.connect();
        console.log('Connected successfully');
    } catch (error) {
        console.error('Failed to connect:', error);
    }
}
```

### iOS Swift (Generic Skeleton)

```swift
import CoreBluetooth

class BLEConnectionManager: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var characteristics: [CBUUID: CBCharacteristic] = [:]
    private var connectionState: ConnectionState = .disconnected
    private var reconnectAttempts = 0
    private let maxReconnectAttempts = 3
    
    enum ConnectionState {
        case disconnected, scanning, connecting, connected, reconnecting
    }
    
    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }
    
    // MARK: - CBCentralManagerDelegate
    
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            print("Bluetooth is powered on")
            startScanning()
        case .poweredOff:
            print("Bluetooth is powered off")
            connectionState = .disconnected
        case .unauthorized:
            print("Bluetooth access denied")
        case .unsupported:
            print("Bluetooth not supported")
        default:
            print("Bluetooth state: \(central.state.rawValue)")
        }
    }
    
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, 
                       advertisementData: [String : Any], rssi RSSI: NSNumber) {
        
        guard let name = peripheral.name, name == "AutoWatering" else {
            return
        }
        
        print("Discovered AutoWatering device: \(peripheral)")
        
        self.peripheral = peripheral
        peripheral.delegate = self
        
        centralManager.stopScan()
        connectionState = .connecting
        centralManager.connect(peripheral, options: nil)
    }
    
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("Connected to peripheral: \(peripheral)")
        
        connectionState = .connected
        reconnectAttempts = 0
        
        // Discover services
        peripheral.discoverServices([CBUUID(string: "your-service-uuid")])
    }
    
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("Disconnected from peripheral: \(peripheral)")
        
        if let error = error {
            print("Disconnection error: \(error)")
        }
        
        connectionState = .disconnected
        characteristics.removeAll()
        
        // Attempt automatic reconnection
        if reconnectAttempts < maxReconnectAttempts {
            reconnectAttempts += 1
            connectionState = .reconnecting
            
            DispatchQueue.main.asyncAfter(deadline: .now() + Double(reconnectAttempts) * 2.0) {
                self.attemptReconnection()
            }
        } else {
            print("Maximum reconnection attempts reached")
        }
    }
    
    // MARK: - CBPeripheralDelegate
    
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil else {
            print("Error discovering services: \(error!)")
            return
        }
        
        guard let services = peripheral.services else {
            return
        }
        
        for service in services {
            print("Discovered service: \(service.uuid)")
            peripheral.discoverCharacteristics(nil, for: service)
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil else {
            print("Error discovering characteristics: \(error!)")
            return
        }
        
        guard let characteristics = service.characteristics else {
            return
        }
        
        for characteristic in characteristics {
            print("Discovered characteristic: \(characteristic.uuid)")
            self.characteristics[characteristic.uuid] = characteristic
            
            // Subscribe to notifications if supported
            if characteristic.properties.contains(.notify) {
                peripheral.setNotifyValue(true, for: characteristic)
                print("Subscribed to notifications for \(characteristic.uuid)")
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            print("Notification subscription error: \(error)")
        } else {
            print("Notification subscription successful for \(characteristic.uuid)")
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            print("Characteristic read error: \(error)")
            return
        }
        
        guard let data = characteristic.value else {
            return
        }
        
        // Handle characteristic value update
        handleCharacteristicUpdate(characteristic: characteristic, data: data)
    }
    
    // MARK: - Connection Management
    
    private func startScanning() {
        guard centralManager.state == .poweredOn else {
            return
        }
        
        connectionState = .scanning
        centralManager.scanForPeripherals(withServices: [CBUUID(string: "your-service-uuid")], 
                                        options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        
        // Stop scanning after 10 seconds
        DispatchQueue.main.asyncAfter(deadline: .now() + 10.0) {
            if self.connectionState == .scanning {
                self.centralManager.stopScan()
                self.connectionState = .disconnected
                print("Scan timeout")
            }
        }
    }
    
    private func attemptReconnection() {
        guard let peripheral = peripheral else {
            startScanning()
            return
        }
        
        print("Attempting reconnection \(reconnectAttempts)/\(maxReconnectAttempts)")
        connectionState = .connecting
        centralManager.connect(peripheral, options: nil)
    }
    
    func disconnect() {
        guard let peripheral = peripheral else {
            return
        }
        
        centralManager.cancelPeripheralConnection(peripheral)
        connectionState = .disconnected
    }
    
    func isConnected() -> Bool {
        return connectionState == .connected && peripheral?.state == .connected
    }
    
    private func handleCharacteristicUpdate(characteristic: CBCharacteristic, data: Data) {
        // Implement characteristic-specific handling
        print("Received data for \(characteristic.uuid): \(data)")
    }
}
```

### Android Kotlin (Generic Skeleton)

```kotlin
class BLEConnectionManager(private val context: Context) {
    private var bluetoothAdapter: BluetoothAdapter? = null
    private var bluetoothGatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private val characteristics = mutableMapOf<UUID, BluetoothGattCharacteristic>()
    
    private var connectionState = ConnectionState.DISCONNECTED
    private var reconnectAttempts = 0
    private val maxReconnectAttempts = 3
    
    enum class ConnectionState {
        DISCONNECTED, SCANNING, CONNECTING, CONNECTED, RECONNECTING
    }
    
    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    Log.i(TAG, "Connected to GATT server")
                    connectionState = ConnectionState.CONNECTED
                    reconnectAttempts = 0
                    
                    // Discover services
                    gatt.discoverServices()
                }
                
                BluetoothProfile.STATE_DISCONNECTED -> {
                    Log.i(TAG, "Disconnected from GATT server")
                    connectionState = ConnectionState.DISCONNECTED
                    characteristics.clear()
                    
                    // Attempt automatic reconnection
                    if (reconnectAttempts < maxReconnectAttempts) {
                        reconnectAttempts++
                        connectionState = ConnectionState.RECONNECTING
                        
                        Handler(Looper.getMainLooper()).postDelayed({
                            attemptReconnection()
                        }, (reconnectAttempts * 2000).toLong())
                    } else {
                        Log.w(TAG, "Maximum reconnection attempts reached")
                    }
                }
            }
        }
        
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.i(TAG, "Services discovered")
                
                val service = gatt.getService(UUID.fromString("your-service-uuid"))
                service?.characteristics?.forEach { characteristic ->
                    Log.i(TAG, "Discovered characteristic: ${characteristic.uuid}")
                    characteristics[characteristic.uuid] = characteristic
                    
                    // Subscribe to notifications if supported
                    if (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) {
                        gatt.setCharacteristicNotification(characteristic, true)
                        
                        val descriptor = characteristic.getDescriptor(
                            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
                        )
                        descriptor?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        gatt.writeDescriptor(descriptor)
                        
                        Log.i(TAG, "Subscribed to notifications for ${characteristic.uuid}")
                    }
                }
            } else {
                Log.w(TAG, "Service discovery failed with status: $status")
            }
        }
        
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val data = characteristic.value
            Log.d(TAG, "Characteristic changed: ${characteristic.uuid}, data: ${data.contentToString()}")
            
            // Handle characteristic value update
            handleCharacteristicUpdate(characteristic, data)
        }
        
        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "Characteristic write successful: ${characteristic.uuid}")
            } else {
                Log.e(TAG, "Characteristic write failed: ${characteristic.uuid}, status: $status")
            }
        }
    }
    
    fun connect(deviceAddress: String): Boolean {
        bluetoothAdapter = BluetoothAdapter.getDefaultAdapter()
        
        if (bluetoothAdapter == null) {
            Log.e(TAG, "Bluetooth not supported")
            return false
        }
        
        device = bluetoothAdapter?.getRemoteDevice(deviceAddress)
        if (device == null) {
            Log.e(TAG, "Device not found")
            return false
        }
        
        connectionState = ConnectionState.CONNECTING
        bluetoothGatt = device?.connectGatt(context, false, gattCallback)
        
        return bluetoothGatt != null
    }
    
    private fun attemptReconnection() {
        device?.let { device ->
            Log.i(TAG, "Attempting reconnection $reconnectAttempts/$maxReconnectAttempts")
            connectionState = ConnectionState.CONNECTING
            bluetoothGatt?.close()
            bluetoothGatt = device.connectGatt(context, false, gattCallback)
        }
    }
    
    fun disconnect() {
        bluetoothGatt?.disconnect()
        connectionState = ConnectionState.DISCONNECTED
    }
    
    fun close() {
        bluetoothGatt?.close()
        bluetoothGatt = null
        connectionState = ConnectionState.DISCONNECTED
    }
    
    fun isConnected(): Boolean {
        return connectionState == ConnectionState.CONNECTED
    }
    
    private fun handleCharacteristicUpdate(characteristic: BluetoothGattCharacteristic, data: ByteArray) {
        // Implement characteristic-specific handling
        Log.d(TAG, "Handling update for ${characteristic.uuid}")
    }
    
    companion object {
        private const val TAG = "BLEConnectionManager"
    }
}
```

## 7. Best Practices (Current State)

| Area | Practical Guidance (Today) | Future Hardening Hook |
| --- | --- | --- |
| Security | Treat link as unauthenticated unless you initiate pairing manually | Enforce min security + passkey callbacks |
| Single Connection | Expect exclusive access; handle disconnect + fast reconnect | Consider optional multi-conn w/ authorization |
| Notification Load | Rely on internal adaptive throttling already documented per characteristic | Tunable QoS per client |
| Advertising Restart | Allow up to ~0.5-5 s for restart sequence after disconnect | Smarter backoff + health metrics |
| Memory Buffers | Current counts sized for burst history fragments | Dynamic scaling by feature usage |

## 8. Interplay With Security Roadmap

Once enforcement lands, this file will gain a matrix mapping connection security states to allowed operations (e.g., "unencrypted: read-only subset"). For now, all documented write semantics in characteristic specs apply regardless of security level.

---
Revision note: Removed marketing / aspirational security phrasing and reframed examples as skeletal templates. For explicit security roadmap see `security-pairing.md`.
