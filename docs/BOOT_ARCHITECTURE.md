# Boot Architecture & Initialization Sequence

## Overview

This document describes the parallel boot architecture for the KannaCloud device firmware. The design prioritizes reliability, graceful degradation, and user experience by initializing sensors and network services in parallel tasks.

## Design Principles

1. **Parallel Initialization**: Sensors and network initialize simultaneously to minimize boot time
2. **Graceful Degradation**: Device operates with any combination of available hardware
3. **No Expected Sensors**: Flexible sensor platform - any EZO sensor combination is valid
4. **Retry Logic**: Comprehensive retry mechanisms for transient I2C issues
5. **User-Facing Priority**: Sensors get higher priority (for future touch display)
6. **Non-Blocking Network**: Network failures don't prevent sensor operation
7. **Proper TLS Ordering**: Certificates provisioned before MQTT/HTTPS services start

---

## Architecture Overview

### Task Structure

```
MAIN TASK (Coordinator)
├─ Basic Hardware Init
├─ Launch SENSOR_TASK (Priority 5 - High)
├─ Launch NETWORK_TASK (Priority 3 - Low)
└─ Wait for SENSORS_READY (network is optional)

SENSOR_TASK (Parallel)
├─ I2C Stabilization (3s)
├─ Sensor Discovery & Initialization (with retries)
├─ Start Sensor Reading Loop
└─ Signal SENSORS_READY

NETWORK_TASK (Parallel)
├─ WiFi Connection (10s timeout)
├─ Cloud Provisioning (get certificates)
├─ Start MQTT Client (requires certs)
├─ Start HTTPS Server + WebSocket (requires certs)
├─ Start mDNS
└─ Signal NETWORK_READY
```

---

## Main Task (Coordinator)

### Responsibilities
- Initialize hardware subsystems
- Launch parallel tasks
- Wait for critical components
- Enter normal operation mode

### Sequence

```c
[0s] Basic Hardware Initialization
├─ Initialize NVS flash
├─ Initialize I2C bus hardware (GPIO config, clock)
├─ Initialize GPIO pins (reset button, status LEDs)
├─ Create event group for task synchronization
├─ Launch SENSOR_TASK (priority 5)
├─ Launch NETWORK_TASK (priority 3)
└─ Wait for SENSORS_READY (timeout 60s)
    └─ NETWORK_READY is optional (device works offline)

[20-60s] Normal Operation
├─ Sensors are operational
├─ Network may still be initializing (that's OK)
└─ Enter main event loop
```

### Failure Handling
- **I2C bus hardware failure**: Restart device (likely hardware glitch)
- **Sensor task timeout**: Log error, continue anyway (device useful for network config)
- **Network task failure**: Continue with sensors only (offline mode)

---

## Sensor Task (Priority 5)

### Responsibilities
- Initialize I2C sensors with comprehensive retry logic
- Handle transient I2C communication issues
- Map sensor types (pH, EC, RTD, HUM, etc.)
- Start sensor reading loop
- Runtime sensor health monitoring

### Detailed Sequence

#### Phase 1: Stabilization (0-3s)
```c
[0-3s] I2C Stabilization
├─ Wait 3 seconds for sensor power-up
├─ EZO sensors need time after power-on to become responsive
└─ Log: "Waiting for I2C sensors to stabilize..."
```

**Why 3 seconds?**
- EZO sensors require initialization time after power-on
- Prevents false "sensor not found" during power-up transient
- Allows I2C bus voltage to stabilize

#### Phase 2: Discovery (3s)
```c
[3s] Initial I2C Scan
├─ Scan all valid I2C addresses (0x08-0x77)
├─ Log all discovered devices with addresses
└─ Helps troubleshooting - shows what hardware is present
```

#### Phase 3: Initialization (3-20s)
```c
[3-20s] Sensor Initialization (Sequential with Retries)

For each EZO sensor address:
├─ Addresses checked: 0x16, 0x63, 0x64, 0x66, 0x6F
│
├─ Check if device exists on I2C bus (probe)
│
├─ If found → Initialize sensor:
│   ├─ Attempt 1: ezo_sensor_init()
│   ├─ If fail → Wait 3s, Attempt 2
│   ├─ If fail → Wait 3s, Attempt 3
│   ├─ If fail → Wait 3s, Attempt 4
│   ├─ If fail → Wait 3s, Attempt 5
│   └─ If still fails:
│       ├─ Log: "Sensor at 0xXX failed after 5 attempts"
│       └─ Give up gracefully, continue with other sensors
│
├─ If success:
│   ├─ Query sensor info (type, firmware, name)
│   ├─ Map sensor type to index:
│   │   ├─ RTD → Temperature sensor
│   │   ├─ pH → pH sensor
│   │   ├─ EC → Electrical Conductivity
│   │   ├─ DO → Dissolved Oxygen
│   │   ├─ ORP → Oxidation-Reduction Potential
│   │   └─ HUM → Humidity/Temperature/Dew Point
│   └─ Log: "✓ EZO sensor initialized: Type=pH, FW=2.1"
│
└─ Wait 1.5 seconds before next sensor
    └─ Prevents I2C bus congestion

Additional sensor:
├─ Check for MAX17048 battery monitor at 0x36
├─ If found → Initialize and read initial values
└─ Not required - graceful if missing

Timing:
├─ Best case: 4 sensors × 1.5s = 6 seconds
└─ Worst case: 4 sensors × (5×3s + 1.5s) = 66 seconds
```

**Supported EZO Sensor Addresses:**
| Address | Sensor Type | Default |
|---------|-------------|---------|
| 0x16 | User configurable | No |
| 0x63 | pH | Yes |
| 0x64 | EC (Electrical Conductivity) | Yes |
| 0x66 | RTD (Temperature) | Yes |
| 0x6F | HUM (Humidity) | Yes |

**Note**: 0x66 was missing from original implementation - now added.

#### Phase 4: Verification (20s)
```c
[20s] Final Verification & Inventory
├─ Perform final I2C scan
├─ Log comprehensive sensor inventory:
│   ├─ "Sensor manager initialized: Battery=YES, EZO sensors=3"
│   ├─ "Found: pH (0x63), EC (0x64), HUM (0x6F)"
│   └─ "Not found: RTD (0x66 - no response after 5 attempts)"
│
└─ No errors for missing sensors - this is valid configuration
```

**Important**: There are no "expected" sensors. Any combination (including zero sensors) is valid.

#### Phase 5: Start Reading Loop (20s)
```c
[20s] Start Sensor Reading Task
├─ Load reading interval from NVS (default: 2 seconds)
├─ Begin periodic sensor reading loop
├─ Update sensor cache with latest values
├─ Trigger MQTT publish when data ready (if MQTT available)
└─ WebSocket broadcast to connected clients (if any)
```

#### Phase 6: Signal Ready (20s)
```c
[20s] Signal SENSORS_READY
├─ Set event group bit: SENSORS_READY_BIT
└─ Main task proceeds to normal operation
```

#### Phase 7: Runtime Monitoring (20s+)
```c
[20s+] Runtime Sensor Health Monitoring

For each sensor reading attempt:
├─ If read succeeds:
│   ├─ Reset consecutive failure counter
│   └─ Update sensor cache
│
└─ If read fails:
    ├─ Increment consecutive failure counter
    ├─ If counter >= 5:
    │   ├─ Mark sensor as "degraded"
    │   ├─ Log: "Sensor 0xXX failed 5 times, attempting recovery"
    │   ├─ Attempt re-initialization (same retry logic)
    │   └─ If re-init fails:
    │       ├─ Wait 2 minutes
    │       └─ Retry re-initialization
    │
    └─ Continue with other working sensors (graceful degradation)
```

---

## Network Task (Priority 3)

### Responsibilities
- Establish WiFi connection
- Provision device with cloud (get certificates)
- Start TLS-secured services (MQTT, HTTPS)
- Handle network reconnections

### Critical Dependency Chain
```
WiFi (required)
  └─> Cloud Provisioning (gets TLS certificates)
       ├─> MQTT Client (mqtts:// requires certs)
       └─> HTTPS Server (https:// requires certs)
            ├─> WebSocket (wss:// via HTTPS server)
            └─> mDNS (advertises https/wss services)
```

### Detailed Sequence

#### Phase 1: WiFi Connection (0-10s)
```c
[0-10s] WiFi Connection
├─ Initialize WiFi in STA (station) mode
├─ Load WiFi credentials from NVS
├─ Attempt connection (10 second timeout)
├─ If success:
│   └─ Proceed to cloud provisioning
│
└─ If timeout/failure:
    ├─ Log: "WiFi connection timeout, retrying in background"
    ├─ Start background retry task (every 30 seconds)
    └─ Block here - can't proceed without WiFi
        └─ Cloud provisioning requires internet
```

**WiFi Configuration:**
- Mode: STA (Station - client mode)
- Auth: WPA2-PSK or WPA3-PSK
- Timeout: 10 seconds for initial connection
- Retry: Every 30 seconds if failed

#### Phase 2: Cloud Provisioning (10-15s)
```c
[10-15s] Cloud Provisioning (CRITICAL - Blocks TLS Services)

Check certificate status:
├─ Query NVS for existing certificates
│
├─ If certificates exist and valid:
│   ├─ Load from NVS (fast path)
│   ├─ Time: ~1 second
│   └─ Skip API call
│
└─ If certificates missing or invalid:
    ├─ Call cloud API: POST /api/provision
    ├─ Send: MAC address, device type
    ├─ Receive:
    │   ├─ device_id (e.g., "kc-3030f973c5cc")
    │   ├─ MQTT broker credentials
    │   ├─ MQTT topic structure
    │   ├─ CA certificate for TLS
    │   └─ Device certificate (if mutual TLS)
    ├─ Store all credentials in NVS
    ├─ Time: ~5 seconds (network API call)
    └─ Log: "✓ Device provisioned successfully"

If provisioning fails:
├─ Log error with details
├─ Retry every 60 seconds
├─ Block TLS services (can't start without certs)
└─ Sensors continue operating independently
```

**Certificates Obtained:**
- CA certificate for MQTT broker verification
- Device certificate for mutual TLS (optional)
- HTTPS server certificate for local dashboard

#### Phase 3: MQTT Client (15-18s)
```c
[15-18s] Initialize & Start MQTT Client

Prerequisites:
├─ WiFi connected ✓
└─ Certificates available ✓

Initialize MQTT:
├─ Load broker URI from NVS (default: mqtts://mqtt.kannacloud.com:8883)
├─ Load CA certificate (from cloud provisioning)
├─ Load MQTT credentials (username, password from provisioning)
├─ Configure TLS settings:
│   ├─ Protocol: MQTT 3.1.1 or 5.0
│   ├─ Transport: TLS (mqtts://)
│   ├─ Port: 8883
│   └─ Certificate verification: Required
│
├─ Start MQTT client task
├─ Connect to broker (with automatic retry)
├─ Subscribe to command topic: kannacloud/sensor/{device_id}/cmd
└─ Log: "✓ Connected to MQTT broker"

Connection handling:
├─ Automatic reconnection with exponential backoff
├─ Publishes sensor data when available
└─ Receives commands from cloud
```

**MQTT Topics:**
- Publish: `kannacloud/sensor/{device_id}/data`
- Subscribe: `kannacloud/sensor/{device_id}/cmd`
- Will topic: `kannacloud/sensor/{device_id}/status` (offline)

#### Phase 4: HTTPS Server (18-20s) - S3 Only
```c
[18-20s] Start HTTPS Server + WebSocket

Prerequisites:
├─ WiFi connected ✓
├─ Certificates available ✓
└─ Not ESP32-C6 (C6 is cloud-only)

Initialize HTTPS Server:
├─ Load TLS certificate (from cloud provisioning)
├─ Create HTTPS server instance (port 443)
├─ Register HTTP endpoints:
│   ├─ GET  /                      → Serve index.html
│   ├─ GET  /api/sensors           → Current sensor readings
│   ├─ GET  /api/device/info       → Device ID and name
│   ├─ POST /api/device/name       → Save device name
│   ├─ GET  /api/wifi              → WiFi settings
│   ├─ POST /api/wifi              → Save WiFi credentials
│   ├─ GET  /api/mqtt              → MQTT settings
│   ├─ POST /api/mqtt              → Save MQTT settings
│   └─ ... (all other API endpoints)
│
├─ Register WebSocket endpoint:
│   └─ WS  /ws                     → Real-time sensor updates (wss://)
│
├─ Start server
└─ Log: "✓ HTTPS server started on port 443"

WebSocket features:
├─ Real-time sensor data broadcast (every reading)
├─ Connection management (track connected clients)
├─ Automatic cleanup of dead connections
└─ Same TLS certificate as HTTPS
```

**WebSocket Protocol:**
```json
{
  "type": "sensor_update",
  "timestamp": 1702435200,
  "data": {
    "battery": {"voltage": 3.7, "percentage": 85},
    "sensors": {
      "pH": {"value": 6.8},
      "EC": {"value": 1200},
      "HUM": {"humidity": 42, "air_temp": 21.76, "dew_point": 8.33}
    }
  }
}
```

#### Phase 5: mDNS (20s) - S3 Only
```c
[20s] Start mDNS Service

Prerequisites:
├─ WiFi connected ✓
├─ HTTPS server running ✓
└─ Not ESP32-C6

Initialize mDNS:
├─ Set hostname: "kc"
├─ Advertise services:
│   ├─ _https._tcp  → Port 443 (HTTPS dashboard)
│   └─ _ws._tcp     → Port 443 (Secure WebSocket)
│
└─ Log: "✓ mDNS started, accessible at https://kc.local"

Users can now access:
├─ https://kc.local          → Dashboard
└─ wss://kc.local/ws         → WebSocket
```

#### Phase 6: Signal Ready (20s)
```c
[20s] Signal NETWORK_READY
├─ Set event group bit: NETWORK_READY_BIT
└─ All network services operational
```

#### Phase 7: Reconnection Handling (20s+)
```c
[20s+] Handle Network Reconnections

WiFi Reconnection:
├─ Monitor WiFi events (DISCONNECTED, CONNECTED)
├─ On WiFi disconnect:
│   ├─ MQTT client detects loss, waits for WiFi
│   ├─ HTTPS server continues (local connections may still work)
│   └─ Start WiFi reconnection attempts
│
└─ On WiFi reconnect:
    ├─ MQTT client automatically reconnects to broker
    ├─ HTTPS server continues normally
    └─ Resume cloud services

MQTT Reconnection:
├─ Built-in automatic retry with exponential backoff
├─ Initial retry: 1 second
├─ Max retry: 60 seconds
├─ Maintains connection state across WiFi drops
└─ Resubscribes to topics on reconnect

WebSocket Reconnection:
├─ Clients handle their own reconnection logic
├─ Server maintains client list
├─ On client disconnect:
│   ├─ Clean up connection resources
│   └─ Remove from broadcast list
│
└─ On new connection:
    ├─ Add to client list
    └─ Send initial sensor snapshot
```

---

## Configuration Constants

```c
// Timing Configuration
#define I2C_STABILIZATION_DELAY_MS     3000   // Initial wait for sensor power-up
#define SENSOR_INIT_RETRY_COUNT        5      // Attempts per sensor
#define SENSOR_INIT_RETRY_DELAY_MS     3000   // Between retry attempts (3 seconds)
#define SENSOR_INTER_INIT_DELAY_MS     1500   // Between different sensors (1.5 seconds)
#define WIFI_CONNECT_TIMEOUT_MS        10000  // WiFi connection timeout (10 seconds)
#define WIFI_RETRY_INTERVAL_MS         30000  // WiFi retry interval (30 seconds)
#define CLOUD_PROVISION_RETRY_MS       60000  // Cloud provision retry (60 seconds)
#define SENSOR_CONSECUTIVE_FAIL_LIMIT  5      // Before attempting re-init
#define SENSOR_RECOVERY_RETRY_MS       120000 // 2 minutes between recovery attempts

// Task Configuration
#define SENSOR_TASK_STACK_SIZE         4096   // 4KB stack
#define SENSOR_TASK_PRIORITY           5      // Higher priority - user facing
#define NETWORK_TASK_STACK_SIZE        8192   // 8KB stack - TLS needs more
#define NETWORK_TASK_PRIORITY          3      // Lower priority - background

// Event Group Bits for Task Synchronization
#define SENSORS_READY_BIT              BIT0   // Sensor task completed
#define NETWORK_READY_BIT              BIT1   // Network task completed

// EZO Sensor I2C Addresses
static const uint8_t EZO_ADDRESSES[] = {
    0x16,  // Custom address (user configurable)
    0x63,  // pH (default)
    0x64,  // EC - Electrical Conductivity (default)
    0x66,  // RTD - Temperature (default) ⚠️ ADDED - was missing!
    0x6F   // HUM - Humidity (default)
};

#define BATTERY_MONITOR_ADDRESS        0x36   // MAX17048
```

---

## Boot Timeline Examples

### Example 1: Best Case Scenario
**Conditions**: All sensors found on first try, WiFi fast, certificates cached

```
Time  | Sensor Task                          | Network Task
------|--------------------------------------|----------------------------------------
0s    | Launch task                          | Launch task
0-3s  | Wait for I2C stabilization          | Connect to WiFi (fast - 2s)
3s    | Initial I2C scan                     | Check NVS for certificates
3-4s  | Init pH sensor (0x63) - success     | Load certificates from NVS
4-5s  | Wait 1.5s, init EC (0x64) - success | Initialize MQTT client
5-6s  | Wait 1.5s, init RTD (0x66) - success| Connect to MQTT broker
6-7s  | Wait 1.5s, init HUM (0x6F) - success| Initialize HTTPS server
7-8s  | Init MAX17048 battery monitor        | Start WebSocket endpoint
8-9s  | Final I2C scan, log inventory        | Start mDNS service
9s    | Start sensor reading task            | Signal NETWORK_READY ✓
9s    | Signal SENSORS_READY ✓               |
------|--------------------------------------|----------------------------------------
Total boot time: ~9 seconds
```

**Result**: Device fully operational in 9 seconds, all sensors working, all services running.

---

### Example 2: Worst Case Scenario
**Conditions**: Sensors need retries, slow WiFi, first boot (no cached certs)

```
Time  | Sensor Task                          | Network Task
------|--------------------------------------|----------------------------------------
0s    | Launch task                          | Launch task
0-3s  | Wait for I2C stabilization          | Connecting to WiFi...
3s    | Initial I2C scan                     | Still connecting...
3-6s  | Init pH (0x63) - fail                | Still connecting...
6-9s  | Retry pH - fail                      | Still connecting...
9-10s | Retry pH - fail                      | WiFi connected (took 10s)
10-13s| Retry pH - success!                  | Cloud provision API call
13-15s| Wait 1.5s, init EC (0x64) - success | Still provisioning...
15-18s| Wait 1.5s, init RTD (0x66) - success| Received certs, init MQTT
18-19s| Wait 1.5s, init HUM (0x6F) - success| Connect to MQTT broker
19-20s| Init battery, final scan             | Start HTTPS server
20s   | Start reading task                   | Start mDNS
20s   | Signal SENSORS_READY ✓               | Signal NETWORK_READY ✓
------|--------------------------------------|----------------------------------------
Total boot time: ~20 seconds
```

**Result**: Device operational in 20 seconds, pH sensor needed retries, network took full time.

---

### Example 3: Degraded Mode (Missing Sensors)
**Conditions**: Only 2 of 4 sensors respond, network fully operational

```
Time  | Sensor Task                          | Network Task
------|--------------------------------------|----------------------------------------
0s    | Launch task                          | Launch task
0-3s  | Wait for I2C stabilization          | WiFi connects (fast)
3s    | I2C scan: Found 0x64, 0x6F          | Load certs from NVS
      | NOT FOUND: 0x63, 0x66               |
3-18s | Try 0x63 (pH) - 5 attempts × 3s     | Initialize MQTT (done at 5s)
      | All fail, give up gracefully        | Initialize HTTPS (done at 7s)
18-20s| Init 0x64 (EC) - success            | Start mDNS (done at 8s)
20-22s| Init 0x6F (HUM) - success           | Signal NETWORK_READY ✓ (at 8s)
22s   | Final scan, log:                     | Waiting for sensors...
      | "Found: EC, HUM"                    |
      | "Missing: pH, RTD"                  |
22s   | Start reading with 2 sensors         |
22s   | Signal SENSORS_READY ✓               |
------|--------------------------------------|----------------------------------------
Total boot time: ~22 seconds
```

**Result**: Device works with 2 sensors, MQTT publishes EC+HUM only, dashboard shows available data.

**MQTT Payload Example** (degraded mode):
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Greenhouse #1",
  "sensors": {
    "EC": {"conductivity": 1200},
    "HUM": {"humidity": 42, "air_temp": 21.76, "dew_point": 8.33}
  },
  "battery": {"voltage": 3.7, "percentage": 85},
  "rssi": -56
}
```

---

### Example 4: Offline Mode (No Network)
**Conditions**: All sensors work, WiFi unavailable

```
Time  | Sensor Task                          | Network Task
------|--------------------------------------|----------------------------------------
0s    | Launch task                          | Launch task
0-3s  | Wait for I2C stabilization          | Attempting WiFi connection...
3s    | Initial I2C scan                     | Still trying...
3-9s  | Initialize all 4 sensors (success)   | Still trying...
9s    | Start sensor reading task            | WiFi timeout (10s)
9s    | Signal SENSORS_READY ✓               | Log: "WiFi timeout, retrying..."
      |                                      | Start background retry (every 30s)
      |                                      | Block - can't start MQTT/HTTPS
------|--------------------------------------|----------------------------------------
Result: Sensors operational, network degraded
```

**User Experience**:
- ✓ Sensors read every 2 seconds
- ✓ Touch display shows live data
- ✗ No MQTT publishing (no internet)
- ✗ No HTTPS dashboard (no WiFi)
- ⏳ WiFi retries every 30 seconds in background
- ⏳ When WiFi comes back, network services start automatically

---

## Failure Scenarios & Handling

### 1. I2C Bus Hardware Failure
**Symptom**: `i2c_new_master_bus()` returns error  
**Cause**: SDA/SCL pins shorted, hardware malfunction  
**Behavior**: Restart device (ESP_ERROR_CHECK)  
**Rationale**: Hardware issue likely transient, restart may resolve

---

### 2. Individual Sensor Initialization Failure
**Symptom**: `ezo_sensor_init()` fails after 5 attempts  
**Cause**: Sensor not connected, wrong address, sensor malfunction  
**Behavior**: 
- Log: "Sensor at 0xXX failed after 5 attempts"
- Give up gracefully
- Continue with other sensors
- MQTT publishes with available sensors only

**Example Log**:
```
W (15234) SENSOR_MGR: pH sensor at 0x63 failed after 5 attempts
W (15234) SENSOR_MGR: Check wiring and power
I (15244) SENSOR_MGR: Continuing with remaining sensors...
```

---

### 3. All Sensors Fail
**Symptom**: No sensors initialize successfully  
**Cause**: Power issue, wrong I2C pins, all sensors disconnected  
**Behavior**:
- Signal SENSORS_READY anyway (empty sensor list)
- Device continues operating
- Network services start normally
- MQTT publishes empty sensor object
- Dashboard shows "No sensors detected"

**MQTT Payload** (no sensors):
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Test Device",
  "sensors": {},
  "rssi": -56
}
```

**Use Cases**: Configuration mode, network testing, firmware updates

---

### 4. WiFi Connection Timeout
**Symptom**: WiFi doesn't connect within 10 seconds  
**Cause**: Wrong credentials, out of range, router offline  
**Behavior**:
- Log: "WiFi connection timeout"
- Start background retry task (every 30 seconds)
- Block Network Task (can't proceed without WiFi)
- Sensor Task continues independently
- Device operates in offline mode

---

### 5. Cloud Provisioning Failure
**Symptom**: API call to provision endpoint fails  
**Cause**: No internet, cloud service down, device not registered  
**Behavior**:
- Log error with details
- Retry every 60 seconds
- Block MQTT and HTTPS (need certificates)
- Sensors continue operating
- Touch display shows sensor data

**Retry Strategy**:
```c
while (!provisioned) {
    esp_err_t ret = cloud_provision();
    if (ret == ESP_OK) {
        provisioned = true;
        break;
    }
    ESP_LOGW(TAG, "Cloud provisioning failed, retry in 60s");
    vTaskDelay(pdMS_TO_TICKS(60000));
}
```

---

### 6. MQTT Broker Unreachable
**Symptom**: Can't connect to `mqtts://mqtt.kannacloud.com:8883`  
**Cause**: Broker offline, firewall blocking, certificate issue  
**Behavior**:
- MQTT client has built-in automatic reconnection
- Exponential backoff (1s, 2s, 4s, 8s... up to 60s)
- Sensors continue reading
- HTTPS server continues operating
- When broker comes back, client reconnects automatically

---

### 7. Runtime Sensor Failure
**Symptom**: Working sensor stops responding during operation  
**Cause**: Loose connection, sensor power issue, I2C bus glitch  
**Behavior**:

```c
Consecutive failures tracking:
├─ Reading fails: counter++
├─ If counter >= 5:
│   ├─ Log: "Sensor 0xXX failed 5 consecutive times"
│   ├─ Mark sensor as "degraded"
│   ├─ Attempt re-initialization (same 5×3s retry logic)
│   └─ If re-init fails:
│       ├─ Wait 2 minutes
│       └─ Retry re-initialization
│
└─ Continue with other working sensors
```

**Example Timeline**:
```
T+0s:   pH sensor reads successfully (7.2)
T+2s:   pH read fails (timeout)
T+4s:   pH read fails (timeout)
T+6s:   pH read fails (timeout)
T+8s:   pH read fails (timeout)
T+10s:  pH read fails (timeout) - 5 consecutive failures!
T+10s:  Log: "pH sensor degraded, attempting recovery"
T+10s:  Attempt re-init (5 tries × 3s = 15s)
T+25s:  Re-init successful! Resume normal readings
```

---

### 8. WiFi Drops During Operation
**Symptom**: Connected WiFi suddenly disconnects  
**Cause**: Router reboot, signal loss, DHCP lease expired  
**Behavior**:
- WiFi event handler triggered
- MQTT client detects disconnect, pauses publishing
- HTTPS server continues (local connections may work)
- WiFi auto-reconnect (built-in ESP-IDF feature)
- When reconnected:
  - MQTT client automatically reconnects to broker
  - Resume publishing sensor data
  - HTTPS server continues normally

---

## Key Design Features

### 1. Graceful Degradation
- Device works with **any combination of hardware**
- No "required" sensors - any subset is valid
- Missing sensors don't prevent operation
- Network failures don't stop sensor readings
- Partial connectivity is acceptable

### 2. Comprehensive Retry Logic
- **Sensors**: 5 attempts with 3-second delays
- **WiFi**: Continuous retry every 30 seconds
- **Cloud provision**: Retry every 60 seconds
- **MQTT**: Automatic reconnection with exponential backoff
- **Runtime recovery**: Re-init failed sensors every 2 minutes

### 3. Parallel Initialization
- Sensors and network initialize simultaneously
- Total boot time = MAX(sensor_time, network_time)
- Not SUM(sensor_time + network_time)
- User sees data faster (sensors don't wait for network)

### 4. Non-Blocking Network
- WiFi timeout doesn't block sensors
- Cloud provision failure doesn't stop device
- MQTT issues don't affect sensor readings
- Device useful even fully offline (touch display)

### 5. Proper TLS Ordering
- Cloud provisioning **always before** MQTT/HTTPS
- Certificates stored in NVS (persist across reboots)
- Fast path: Load certs from NVS (~1 second)
- Slow path: Fetch from cloud API (~5 seconds)

### 6. User-Facing Priority
- Sensor task: Priority 5 (high)
- Network task: Priority 3 (low)
- Ensures sensors initialize first
- Critical for touch display responsiveness

### 7. Runtime Monitoring
- Detect sensor failures (5 consecutive timeouts)
- Automatic recovery attempts
- Re-initialization without full device restart
- Monitors WiFi, MQTT, sensor health

### 8. Flexible Architecture
- No hardcoded sensor expectations
- Works with 0 to 5+ EZO sensors
- Battery monitor optional
- Cloud services optional (offline mode)
- Future-proof for touch display, additional sensors

---

## MQTT Payload Structure

### Complete Payload (All Features)
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Greenhouse #1",
  "sensors": {
    "pH": {
      "value": 6.84
    },
    "EC": {
      "conductivity": 1250,
      "tds": 625,
      "salinity": 0.64
    },
    "RTD": {
      "temperature": 22.5
    },
    "HUM": {
      "humidity": 42.0,
      "air_temp": 21.76,
      "dew_point": 8.33
    }
  },
  "battery": {
    "voltage": 3.72,
    "percentage": 85
  },
  "rssi": -56
}
```

### Minimal Payload (One Sensor, No Battery)
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Test Device",
  "sensors": {
    "HUM": {
      "humidity": 42.0,
      "air_temp": 21.76,
      "dew_point": 8.33
    }
  },
  "rssi": -56
}
```

### Empty Payload (No Sensors)
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Config Device",
  "sensors": {},
  "rssi": -56
}
```

**Notes**:
- `device_id`: Always present (immutable, MAC-based)
- `device_name`: Optional (user-configurable, 1-64 chars)
- `sensors`: Object with detected sensors only
- `battery`: Only present if MAX17048 found
- `rssi`: WiFi signal strength (always present when connected)

---

## Implementation Checklist

### Phase 1: Core Infrastructure
- [ ] Create event group for task synchronization
- [ ] Define configuration constants
- [ ] Update EZO address array (add 0x66 for RTD)
- [ ] Add task function declarations

### Phase 2: Sensor Task
- [ ] Implement 3-second I2C stabilization delay
- [ ] Add retry logic (5 attempts × 3 seconds)
- [ ] Add inter-sensor delay (1.5 seconds)
- [ ] Implement final verification scan
- [ ] Add comprehensive logging
- [ ] Signal SENSORS_READY event bit

### Phase 3: Network Task
- [ ] WiFi connection with timeout
- [ ] Cloud provisioning (check NVS first)
- [ ] MQTT initialization (after certs)
- [ ] HTTPS server startup (after certs)
- [ ] mDNS startup
- [ ] Signal NETWORK_READY event bit

### Phase 4: Runtime Monitoring
- [ ] Track consecutive sensor failures
- [ ] Implement sensor re-initialization
- [ ] Add 2-minute retry timer
- [ ] WiFi reconnection handling
- [ ] MQTT reconnection handling

### Phase 5: Main Task Updates
- [ ] Launch sensor task
- [ ] Launch network task
- [ ] Wait for SENSORS_READY (required)
- [ ] Wait for NETWORK_READY (optional, with timeout)
- [ ] Enter normal operation mode

### Phase 6: Testing
- [ ] Test all sensors found (best case)
- [ ] Test missing sensors (degraded mode)
- [ ] Test sensor retry logic (disconnect during init)
- [ ] Test WiFi timeout (no network)
- [ ] Test cloud provision failure
- [ ] Test runtime sensor recovery
- [ ] Test MQTT reconnection
- [ ] Test parallel boot timing

---

## Logging Standards

### Startup Logs (INFO level)
```
I (0) MAIN: ========================================
I (0) MAIN: KannaCloud Device Firmware v1.2.0
I (0) MAIN: Chip: ESP32-S3, Flash: 4MB
I (0) MAIN: ========================================
I (100) MAIN: Initializing NVS...
I (150) MAIN: Initializing I2C bus...
I (200) MAIN: Launching sensor task (priority 5)
I (210) MAIN: Launching network task (priority 3)
I (220) SENSOR_MGR: Waiting 3s for I2C stabilization...
I (230) NETWORK: Connecting to WiFi...
```

### Sensor Detection Logs
```
I (3220) SENSOR_MGR: Starting I2C scan...
I (3500) SENSOR_MGR: I2C scan complete: 4 devices found
I (3510) SENSOR_MGR: Initializing sensors...
I (4200) SENSOR_MGR: ✓ pH sensor initialized (0x63, FW=2.1)
I (5800) SENSOR_MGR: ✓ EC sensor initialized (0x64, FW=1.5)
I (7400) SENSOR_MGR: ✓ RTD sensor initialized (0x66, FW=2.0)
I (9000) SENSOR_MGR: ✓ HUM sensor initialized (0x6F, FW=1.0)
I (9100) SENSOR_MGR: ✓ Battery monitor detected (0x36)
I (9200) SENSOR_MGR: Sensor manager ready: 4 EZO sensors, Battery=YES
```

### Network Logs
```
I (2300) NETWORK: WiFi connected (SSID: MyNetwork, IP: 192.168.1.100)
I (3400) NETWORK: Cloud provisioning complete
I (4500) NETWORK: ✓ MQTT connected to broker
I (5600) NETWORK: ✓ HTTPS server started on port 443
I (5700) NETWORK: ✓ mDNS started: https://kc.local
I (5800) NETWORK: Network services ready
```

### Error/Warning Logs
```
W (15000) SENSOR_MGR: pH sensor at 0x63 not responding
W (18000) SENSOR_MGR: pH sensor failed after 5 attempts
W (18010) SENSOR_MGR: Continuing with remaining sensors...
E (10000) NETWORK: WiFi connection timeout
W (10010) NETWORK: Retrying WiFi in 30 seconds...
E (15000) NETWORK: Cloud provisioning failed: HTTP 500
W (15010) NETWORK: Retrying in 60 seconds...
```

### Runtime Monitoring Logs
```
W (120000) SENSOR_MGR: EC sensor failed 5 consecutive times
I (120010) SENSOR_MGR: Attempting EC sensor recovery...
I (135020) SENSOR_MGR: ✓ EC sensor recovered successfully
W (180000) NETWORK: WiFi disconnected
I (185000) NETWORK: WiFi reconnected
I (186000) NETWORK: MQTT reconnected to broker
```

---

## Future Enhancements

### Touch Display Integration
- Display task: Priority 6 (highest)
- Shows sensor data even when offline
- Local UI for WiFi configuration
- Visual sensor status indicators

### Advanced Sensor Management
- User-configurable sensor addresses via dashboard
- Sensor calibration via local UI
- Historical data logging to SD card
- Sensor health graphs on display

### Enhanced Recovery
- Automatic I2C bus reset on complete failure
- Sensor power cycle via GPIO control
- Configurable retry parameters via NVS
- OTA firmware updates for bug fixes

### Cloud Features
- Remote sensor configuration
- Firmware updates via MQTT
- Remote diagnostics and logs
- Cloud-triggered sensor calibration

---

## References

- ESP-IDF I2C Master Driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2c.html
- ESP-IDF WiFi Driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html
- ESP-IDF MQTT Client: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html
- FreeRTOS Task Management: https://www.freertos.org/a00019.html
- Atlas Scientific EZO Devices: https://atlas-scientific.com/embedded-solutions/ezo-embedded-circuits/

---

**Document Version**: 1.0  
**Last Updated**: 2025-12-13  
**Status**: Design Complete - Ready for Implementation
