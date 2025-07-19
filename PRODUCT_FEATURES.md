# AutoWatering - Smart Irrigation System - Complete Feature List

## 🌟 Product Overview

AutoWatering is an advanced smart irrigation system built on the Zephyr RTOS platform, designed for precision watering control with comprehensive monitoring capabilities. The system can manage up to 8 independent irrigation channels with automatic scheduling, flow monitoring, and remote control via Bluetooth Low Energy.

---

## 🔧 **HARDWARE CAPABILITIES**

### **Multi-Channel Valve Control**
- **8 independent irrigation channels** with individual control
- **nRF52840 ProMicro controller** providing GPIO and BLE connectivity
- **12V DC solenoid valves** controlled via 8-channel relay module
- **Safety protections**: Maximum 1 valve active simultaneously to prevent power overload
- **Real-time valve status feedback** with BLE notifications
- **GPIO pin assignments**: P0.17, P0.20, P0.22, P0.24, P1.0, P0.11, P1.4, P1.6
- **Hardware debouncing** and timeout protection for reliable operation

### **🚀 Smart Master Valve System**
- **Intelligent main water control** on GPIO P0.08 with automatic timing management
- **Pre/Post timing control**: Configurable delays before/after zone valve operation
  - **Pre-start delay**: 0-255 seconds (default: 3s) - Master opens BEFORE zone valve
  - **Post-stop delay**: 0-255 seconds (default: 2s) - Master stays open AFTER zone valve closes
  - **Negative delay support**: Master can open AFTER zone valve if delay is negative
- **Overlap detection**: Smart grace period (default: 5s) for consecutive tasks
  - Keeps master valve open when next task starts within grace period
  - Prevents unnecessary open/close cycles for back-to-back watering
- **Dual operating modes**:
  - **Automatic management**: Master valve controlled by watering tasks (default)
  - **Manual control**: Direct BLE control when auto-management is disabled
- **Complete BLE integration**:
  - Real-time status notifications (channel_id = 0xFF)
  - Remote configuration via System Config characteristic
  - Manual open/close commands via Valve Control characteristic
- **Fail-safe operation**: Master valve closes automatically on system shutdown

### **Advanced Flow Sensor System**
- **Pulse-based flow measurement** using YF-S201 or compatible sensors
- **Hardware interrupt processing** on GPIO P0.6 with 5ms debouncing
- **Atomic pulse counting** with thread-safe operations
- **Dual-mode flow monitoring**:
  - Raw pulse count for calibration
  - Smoothed flow rate (pulses/second) with 2-sample circular buffer
- **Real-time anomaly detection**:
  - No-flow detection when valve is open
  - Unexpected flow detection when all valves are closed
- **Configurable calibration**: Default 750 pulses/liter, adjustable via BLE
- **Ultra-fast response**: <50ms detection time with 500ms averaging windows

### **Precision Real-Time Clock**
- **DS3231 RTC module** with I²C interface (SCL: P0.31, SDA: P0.29)
- **Battery backup** for time retention during power loss
- **Temperature compensation** for maximum accuracy
- **Automatic synchronization** with system timekeeping
- **Sub-second precision** for precise scheduling

### **Bluetooth Low Energy Interface**
- **Nordic nRF52840 SoftDevice s140** for reliable BLE connectivity
- **16 GATT characteristics** providing complete system control
- **Advanced notification system** with 500ms throttling to prevent buffer overflow
- **Automatic error recovery** with reconnection handling
- **MTU optimization**: 20-byte packets with fragmentation support
- **Background update thread** for automatic status monitoring every 2 seconds

---

## 🌱 **SOFTWARE INTELLIGENCE - ADVANCED CONTROL**

### **Plant Database & Configuration**
- **407 predefined plant species** from comprehensive plant database
- **9 plant categories**: Vegetables, Herbs, Flowers, Shrubs, Trees, Fruits, Berries, Grains, Succulents
- **Crop coefficient support**: Initial (Kc_i), Mid-season (Kc_mid), End-season (Kc_end) values
- **Root depth parameters**: Precise root depth measurements for optimal watering
- **RAW depletion factors**: Readily Available Water calculations for each plant type
- **Custom plant support**: Full configuration for user-defined plant types with custom water factors

### **Soil & Environment Intelligence**
- **8 soil types** with detailed characteristics:
  - Clay: 47% field capacity, 170mm/m available water capacity
  - Sandy: 15% field capacity, 60mm/m available water capacity  
  - Loamy: 35% field capacity, 140mm/m available water capacity
  - Silty: 40% field capacity, 160mm/m available water capacity
  - Rocky: 18% field capacity, 75mm/m available water capacity
  - Peaty: 55% field capacity, 200mm/m available water capacity
  - Potting Mix: 60% field capacity, optimized for containers
- **6 irrigation methods**: Drip, Sprinkler, Soaker Hose, Micro Spray, Hand Watering, Flood
- **Coverage flexibility**: Area-based (m²) or plant count measurement
- **Sun exposure configuration**: 0-100% direct sunlight affecting watering schedules

### **Intelligent Scheduling System**
- **Dual scheduling modes**:
  - **Daily scheduling**: Configurable days of week with bitmask (Sunday=bit0, Monday=bit1, etc.)
  - **Periodic scheduling**: Custom interval in days (e.g., every 3 days)
- **Dual watering modes**:
  - **Duration-based**: Specified in minutes and seconds
  - **Volume-based**: Specified in liters with flow sensor feedback
- **Real-time schedule checking**: RTC-based validation every few seconds
- **Automatic task creation**: Background scheduler thread creates tasks based on configuration
- **Task queue management**: FIFO queue with up to 10 pending tasks
- **Priority handling**: Manual tasks override automatic scheduling

---

## 🕒 **TIMEZONE & TIME MANAGEMENT**

### **Comprehensive Timezone Support**
- **Global timezone configuration**: UTC-12 to UTC+14 with minute precision
- **Automatic Daylight Saving Time (DST)**: Smart calculation with configurable rules
- **Local time display**: All timestamps converted from UTC to local time automatically
- **BLE remote configuration**: Full timezone setup via dedicated Bluetooth characteristic
- **Persistent NVS storage**: Timezone settings saved in non-volatile memory
- **Real-time UTC ↔ Local conversion**: Seamless time handling throughout system
- **DST transition management**: Smooth transitions during spring/fall changes

### **Supported DST Rules**
- **European rules**: Last Sunday of March/October (default for Romania: UTC+2/UTC+3)
- **US rules**: 2nd Sunday March, 1st Sunday November
- **Custom rules**: Any month/week/day combination configurable
- **Fixed timezones**: No DST support for regions without time changes
- **Transition handling**: Automatic detection and adjustment during DST changes

---

## 📊 **REAL-TIME MONITORING & HISTORY**

### **Live Task Monitoring**
- **Real-time progress updates**: Automatic notifications every 2 seconds during task execution
- **Comprehensive task data**:
  - Channel ID and watering mode (duration/volume)
  - Task start time and elapsed duration
  - Target vs. current values (seconds or milliliters)
  - Total volume dispensed (calculated from flow sensor)
  - Task status: Idle(0), Running(1), Completed(2), Paused(3)
- **Automatic BLE notifications**: Sent when tasks start, progress, pause, or complete
- **Background monitoring thread**: Dedicated thread for continuous task state tracking
- **Web interface compatible**: JavaScript-ready data format for browser applications

### **Multi-Level History System**
- **Detailed events storage**: 30 events per channel (15 bytes each) for recent activity
- **Daily statistics**: 90 days of aggregated data (16 bytes each)
- **Monthly statistics**: 36 months with Heatshrink compression (12 bytes each)
- **Annual statistics**: 10 years of long-term trends (20 bytes each)
- **Automatic data aggregation**: Background garbage collection maintains storage efficiency
- **Ring buffer management**: Circular buffer with high-watermark (90%) and low-watermark (70%) management
- **Total storage allocation**: 144 KB dedicated NVS partition for history data

### **Statistics & Analytics**
- **Usage tracking**: Total volumes and frequency per channel
- **System efficiency**: Automatic water usage efficiency calculations
- **Leak detection**: Algorithms for identifying abnormal consumption patterns
- **Weekly insights**: Automated analysis with recommendations
- **Flow rate monitoring**: Real-time pulse-per-second calculations with smoothing

---

## 🔔 **SAFETY & ALARM SYSTEM**

### **Advanced Error Detection**
- **No-flow detection**: Automatic detection when valve is open but no water flow occurs
- **Unexpected flow detection**: Alert when flow is detected with all valves closed
- **Hardware monitoring**: Continuous monitoring of GPIO states and device health
- **RTC failure detection**: Automatic detection when real-time clock stops working
- **Low power mode alerts**: Notifications for power supply issues
- **System status codes**:
  - 0: OK - Normal operation
  - 1: No-Flow - Expected flow not detected
  - 2: Unexpected-Flow - Flow detected when all valves closed
  - 3: Fault - Hardware or system fault
  - 4: RTC-Error - Real-time clock malfunction
  - 5: Low-Power - Power supply issues

### **Automatic Recovery System**
- **Graceful error recovery**: Automatic correction attempts for common errors
- **Safety shutdowns**: Automatic valve closure on critical errors
- **Soft reset capability**: Error state reset via BLE commands
- **Comprehensive logging**: All events recorded for troubleshooting
- **Watchdog protection**: System reset on deadlock conditions
- **Flow sensor reset**: Automatic pulse counter reset when all valves close

---

## 📱 **COMPLETE BLUETOOTH INTERFACE**

### **16 GATT Characteristics Implementation**

#### **1. Valve Control (UUID: def1, 4 bytes)**
- **Manual valve operations**: Open, close, pulse commands for channels 0-7
- **🚀 Master valve control**: Direct control via channel_id = 0xFF
  - **Manual open**: task_type = 1 opens master valve (auto-management must be disabled)
  - **Manual close**: task_type = 0 closes master valve (auto-management must be disabled)
  - **Status notifications**: Real-time master valve state updates
- **Task creation**: Duration-based (minutes) or volume-based (liters) watering tasks
- **Real-time status**: Valve active/inactive state notifications for all channels
- **Command structure**: `{channel_id, task_type, value}` where task_type 0=duration/close, 1=volume/open

#### **2. Flow Sensor (UUID: def2, 4 bytes)**
- **Real-time flow monitoring**: Smoothed flow rate in pulses per second
- **High-frequency updates**: Up to 20 Hz during active flow with automatic throttling
- **Hardware processing**: 5ms debouncing, 2-sample circular buffer averaging
- **Rate calculation**: 500ms windows for ultra-fast response
- **Minimum 50ms intervals**: Stability optimization with forced 200ms periodic updates

#### **3. System Status (UUID: def3, 1 byte)**
- **Health monitoring**: Current system status code (0-5)
- **Real-time notifications**: Automatic status change alerts
- **Error reporting**: Fault detection and status propagation

#### **4. Channel Configuration (UUID: def4, 76 bytes, fragmented)**
- **Complete plant setup**: Plant type, soil, irrigation method configuration
- **Coverage settings**: Area-based (m²) or plant count measurement
- **Sun exposure**: 0-100% sunlight configuration
- **Custom naming**: 64-byte channel names with real-time updates
- **Fragmentation protocol**: Automatic handling of large data structures

#### **5. Schedule Configuration (UUID: def5, 9 bytes)**
- **Automatic scheduling**: Daily or periodic watering programs
- **Time settings**: Hour/minute start time with timezone support
- **Days configuration**: Bitmask for daily or interval days
- **Mode selection**: Duration or volume-based automatic watering

#### **6. System Configuration (UUID: def6, 16 bytes)**
- **Power management**: Normal, Energy-Saving, Ultra-Low power modes
- **Flow calibration**: Pulses per liter configuration (default 750)
- **System limits**: Maximum active valves (always 1), channel count (8)
- **🚀 Master valve configuration**: Complete remote control and setup
  - **Enable/disable**: Master valve system on/off control
  - **Pre-start delay**: 0-255 seconds delay before zone valve opens
  - **Post-stop delay**: 0-255 seconds delay after zone valve closes
  - **Overlap grace period**: 0-255 seconds grace for consecutive tasks
  - **Auto-management mode**: Automatic vs manual control selection
  - **Current state**: Real-time master valve status (read-only)
- **Version tracking**: Configuration version for compatibility

#### **7. Task Queue Management (UUID: def7, 9 bytes)**
- **Queue operations**: Create, pause, resume, cancel, clear commands
- **Status monitoring**: Pending count, completed tasks, current active channel
- **Queue state**: Real-time queue status with automatic updates
- **Background processing**: Automatic task execution and monitoring

#### **8. Statistics (UUID: def8, 15 bytes)**
- **Usage tracking**: Total volume, frequency, last watering timestamp
- **Channel analytics**: Per-channel statistics and efficiency metrics
- **Volume calculations**: Flow sensor integration for accurate measurements

#### **9. RTC Configuration (UUID: def9, 16 bytes)**
- **Time synchronization**: Set/get system time with second precision
- **Timezone integration**: Local time display with UTC conversion
- **Date/time structure**: Year, month, day, hour, minute, second, day-of-week

#### **10. Alarm Status (UUID: defa, 7 bytes)**
- **Critical alerts**: Hardware and system fault notifications
- **Alarm history**: Last alarm code and data
- **Alert management**: Clear alarms and reset error states

#### **11. Calibration Management (UUID: defb, 13 bytes)**
- **Interactive calibration**: Start/stop flow sensor calibration process
- **Automatic calculation**: Pulses per liter based on measured volume
- **Calibration status**: Progress tracking and completion notifications
- **Persistence**: Automatic saving of calibration parameters

#### **12. History Management (UUID: defc, 20 bytes, fragmented)**
- **Historical data access**: Detailed events, daily, monthly, annual statistics
- **Time filtering**: Start/end timestamp range selection
- **Data export**: TLV (Type-Length-Value) formatted data streams
- **Fragmentation support**: Large dataset transfer via 20-byte chunks

#### **13. Diagnostics (UUID: defd, 12 bytes)**
- **System health**: Uptime, error count, valve status bitmap
- **Performance metrics**: Memory usage, thread status
- **Debug information**: Last error code and system state

#### **14. Growing Environment (UUID: defe, 52 bytes, fragmented)**
- **Advanced plant configuration**: Detailed plant and soil parameters
- **Environmental settings**: Coverage, irrigation method, sun exposure
- **Custom plant support**: User-defined plant types with custom parameters

#### **15. Current Task Status (UUID: deff, 21 bytes)**
- **Live monitoring**: Real-time active task progress
- **Progress tracking**: Current vs. target values with percentage completion
- **Volume monitoring**: Real-time flow sensor integration
- **Status updates**: Task start, progress, completion with 2-second intervals

#### **16. Timezone Configuration (UUID: 9abc-def123456793, 12 bytes)**
- **DST management**: Complete Daylight Saving Time configuration
- **Custom rules**: Month/week/day DST start/end rules
- **Offset configuration**: UTC offset and DST offset in minutes
- **Regional presets**: European, US, and custom timezone rules

---

## ⚡ **POWER MANAGEMENT**

### **Multiple Power Modes**
- **Normal Mode**: Full performance for active use with complete functionality
- **Energy-Saving Mode**: Reduced monitoring frequency while maintaining core operations
- **Ultra-Low Mode**: Minimal power consumption for long-term battery operation

### **Power Optimizations**
- **Intelligent sleep**: Automatic sleep mode when system is idle
- **Scheduled wake-up**: Automatic wake for scheduled checks and tasks
- **Thread management**: Priority-based thread scheduling and suspension
- **BLE power optimization**: Efficient notification system to reduce radio usage

---

## 💾 **STORAGE & PERSISTENCE**

### **NVS (Non-Volatile Storage) System**
- **144 KB dedicated storage** for configurations and history data
- **Optimized write cycles**: Minimized flash writes for durability
- **Automatic backup**: Critical settings saved on important changes
- **Data integrity**: Validation and error checking on startup
- **Flash partitioning**: Separate 32KB NVS partition and 8KB settings partition

### **Persistent Configurations**
- **Channel settings**: All watering configurations and plant parameters
- **Flow calibration**: Sensor calibration parameters with NVS key 1000
- **Watering history**: Multi-level historical data with compression
- **Timezone settings**: Complete DST configuration
- **System preferences**: Power modes, BLE settings, user preferences

---

## 🌐 **CONNECTIVITY & INTEGRATION**

### **USB & Console Interface**
- **USB CDC-ACM**: Serial interface for configuration and debugging
- **Windows compatibility**: Optimized descriptors for reliable Windows operation
- **Configurable logging**: Multiple verbosity levels for development and debugging
- **Command interface**: Complete system control via console commands

### **Cross-Platform Compatibility**
- **Windows**: Complete support with standard drivers
- **macOS**: Native operation without additional drivers
- **Linux**: Universal support across distributions
- **Web browsers**: Complete JavaScript API for web application development

---

## 🛡️ **SAFETY & RELIABILITY**

### **Hardware Protection**
- **Overcurrent protection**: Current monitoring for valve operations
- **Safe timeouts**: Automatic valve closure after timeout periods (200ms GPIO timeout)
- **GPIO validation**: Hardware configuration verification on startup
- **Device readiness checks**: Automatic verification of all hardware components

### **Software Protection**
- **Thread-safe operations**: Mutex protection and atomic operations for pulse counting
- **Parameter validation**: Comprehensive input validation for all user operations
- **Graceful error handling**: System continues operation despite non-critical errors
- **Watchdog protection**: System reset on deadlock or hang conditions
- **Memory protection**: Stack overflow detection and memory usage monitoring

---

## 🔧 **DEVELOPMENT & CUSTOMIZATION**

### **Modular Architecture**
- **Clean code organization**: Clear separation of responsibilities across modules
- **Public APIs**: Well-defined interfaces for system extension
- **Comprehensive documentation**: Developer guides and API references
- **Reference implementations**: Working examples for all major features

### **Advanced Configurability**
- **Device Tree configuration**: Flexible hardware configuration via DTS files
- **Kconfig options**: Compile-time customization for different use cases
- **Custom plant database**: Complete support for user-defined plant types
- **Adaptive algorithms**: Tunable parameters for different irrigation setups

---

## 📈 **PERFORMANCE & OPTIMIZATIONS**

### **Real-Time Performance**
- **<50ms response time**: For all critical operations including flow detection
- **Parallel processing**: Multiple threads for concurrent operation
- **Intelligent caching**: 100ms cache timeout for responsive channel switching
- **Minimal interrupts**: Efficient design for system stability

### **Memory Optimizations**
- **Efficient RAM usage**: 512KB RAM utilized optimally across all components
- **Dynamic buffer management**: Smart allocation with automatic pool maintenance
- **Data compression**: Heatshrink compression for historical data storage
- **Automatic garbage collection**: Memory maintenance with 90%/70% watermarks

---

## 🎯 **USE CASES & APPLICATIONS**

### **Residential Applications**
- **Home gardens**: Automated watering for small to large residential gardens
- **Container gardening**: Precise control for potted plants and planters
- **Small greenhouses**: Complete automation for hobby greenhouse operations

### **Commercial Applications**
- **Nurseries**: Irrigation management for plant production facilities
- **Urban farming**: Hydroponic and aeroponic system automation
- **Garden centers**: Automated plant care systems for retail environments

### **Educational Applications**
- **STEM projects**: Platform for learning automation and IoT concepts
- **Research**: Data collection for irrigation and plant growth studies
- **Demonstrations**: Real-time monitoring systems for educational presentations

---

## 🏆 **COMPETITIVE ADVANTAGES**

### **vs. Traditional Systems**
- **Superior intelligence**: Automatic plant-based watering decisions vs. simple timers
- **Advanced monitoring**: Real-time feedback vs. blind control systems
- **Complete flexibility**: 8 independent channels vs. simple single-zone systems
- **Modern connectivity**: Full BLE interface vs. no remote control

### **vs. Existing Smart Solutions**
- **Open source**: Transparent, modifiable code vs. proprietary black boxes
- **Dedicated hardware**: Irrigation-optimized design vs. generic IoT platforms
- **Energy efficient**: Purpose-built for continuous operation vs. power-hungry solutions
- **No cloud dependency**: Complete autonomous operation vs. internet-dependent systems

---

## 📋 **FEATURE IMPLEMENTATION STATUS**

### **✅ FULLY IMPLEMENTED**
- 8-channel independent valve control with GPIO safety
- Flow sensor with automatic calibration and anomaly detection
- Complete BLE system with 16 characteristics and notification throttling
- Flexible automatic scheduling with RTC integration
- Multi-level history and statistics with 144KB NVS storage
- Complete timezone and DST support with local time conversion
- Advanced error detection and recovery systems
- Plant database with 407 species and intelligent configuration
- Web interface compatibility with fragmentation protocol
- Persistent NVS configuration storage
- Multiple power modes with intelligent sleep management
- Complete system diagnostics and health monitoring

---

**AutoWatering** represents a complete, modern, and intelligent solution for irrigation automation, providing precision control, advanced monitoring, and maximum flexibility for any type of watering application.
