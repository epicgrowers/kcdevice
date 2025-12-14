# KC Device REST API Reference

**Version**: 1.0  
**Base URL**: `https://<device-ip>` or `https://kc.local`  
**Protocol**: HTTPS with self-signed certificate  
**Authentication**: None required for local network access

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Device Management](#device-management)
3. [Sensor Operations](#sensor-operations)
4. [Settings & Configuration](#settings--configuration)
5. [Advanced Operations](#advanced-operations)
6. [Rate Limits & Best Practices](#rate-limits--best-practices)
7. [Python Client Examples](#python-client-examples)
8. [Error Handling](#error-handling)

---

## Getting Started

### Quick Start: Discovery & Setup Wizard

This section covers the typical workflow for adding a KC device to your Python dashboard.

#### Step 1: Discover Devices on Network

Use mDNS to discover devices:

```python
import socket
from zeroconf import ServiceBrowser, Zeroconf

def discover_kc_devices():
    """Discover KC devices using mDNS"""
    devices = []
    
    class MyListener:
        def add_service(self, zeroconf, type, name):
            info = zeroconf.get_service_info(type, name)
            if info and 'kc-' in name:
                ip = socket.inet_ntoa(info.addresses[0])
                devices.append({
                    'name': name,
                    'ip': ip,
                    'port': info.port
                })
    
    zeroconf = Zeroconf()
    browser = ServiceBrowser(zeroconf, "_https._tcp.local.", MyListener())
    time.sleep(3)  # Wait for discovery
    zeroconf.close()
    
    return devices

# Usage
devices = discover_kc_devices()
for device in devices:
    print(f"Found: {device['name']} at {device['ip']}")
```

#### Step 2: Get Device Information

**Endpoint**: `GET /api/device/info`

**Request**:
```python
import requests

device_ip = "192.168.1.100"
response = requests.get(
    f"https://{device_ip}/api/device/info",
    verify=False  # Self-signed cert
)

device_info = response.json()
print(device_info)
```

**Response**:
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Greenhouse #1"
}
```

#### Step 3: Get Sensor List

**Endpoint**: `GET /api/sensors`

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/api/sensors",
    verify=False
)

sensors = response.json()
```

**Response**:
```json
{
  "sensors": [
    {
      "address": 99,
      "type": "pH",
      "name": "pH Sensor",
      "value": 6.84,
      "unit": "pH",
      "led": true,
      "connected": true
    },
    {
      "address": 100,
      "type": "EC",
      "name": "Conductivity",
      "value": 1250.5,
      "unit": "µS/cm",
      "conductivity": 1250.5,
      "tds": 625.0,
      "salinity": 0.64,
      "led": true,
      "connected": true
    }
  ],
  "count": 2
}
```

#### Step 4: Add Device to Dashboard

Store the `device_id` in your database and associate sensors:

```python
class KCDevice:
    def __init__(self, device_id, ip_address):
        self.device_id = device_id
        self.ip = ip_address
        self.base_url = f"https://{ip_address}"
        
    def get_status(self):
        """Get current device status"""
        response = requests.get(
            f"{self.base_url}/api/status",
            verify=False
        )
        return response.json()
    
    def get_sensors(self):
        """Get all sensors"""
        response = requests.get(
            f"{self.base_url}/api/sensors",
            verify=False
        )
        return response.json()

# Usage in dashboard
device = KCDevice("kc-3030f973c5cc", "192.168.1.100")
status = device.get_status()
sensors = device.get_sensors()
```

---

## Device Management

### Get Device Status

**Endpoint**: `GET /api/status`

**Description**: Returns comprehensive device status including WiFi, MQTT connection, sensors, and system info.

**Request**:
```python
response = requests.get(f"https://{device_ip}/api/status", verify=False)
```

**Response**:
```json
{
  "device_id": "kc-3030f973c5cc",
  "wifi_ssid": "MyNetwork",
  "wifi_connected": true,
  "ip_address": "192.168.1.100",
  "rssi": -56,
  "mqtt_connected": true,
  "sensor_count": 4,
  "uptime_seconds": 3600,
  "free_heap": 280000,
  "chip": "ESP32-S3"
}
```

---

### Get Device Info

**Endpoint**: `GET /api/device/info`

**Description**: Get device ID and user-configured name.

**Response**:
```json
{
  "device_id": "kc-3030f973c5cc",
  "device_name": "Greenhouse #1"
}
```

---

### Set Device Name

**Endpoint**: `POST /api/device/name`

**Description**: Set a friendly name for the device.

**Request Body**:
```json
{
  "device_name": "Greenhouse #1"
}
```

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/device/name",
    json={"device_name": "Greenhouse #1"},
    verify=False
)
```

**Response**:
```json
{
  "status": "saved",
  "device_name": "Greenhouse #1"
}
```

**Constraints**:
- Name must be 1-64 characters
- Alphanumeric, spaces, hyphens, underscores allowed

---

### Reboot Device

**Endpoint**: `POST /api/reboot`

**Description**: Restart the device (applies after 1 second delay).

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/reboot",
    verify=False
)
```

**Response**:
```json
{
  "status": "rebooting"
}
```

**Note**: Connection will be lost. Wait ~30 seconds for device to reboot.

---

### Clear WiFi Credentials

**Endpoint**: `POST /api/clear-wifi`

**Description**: Clear stored WiFi credentials and restart into provisioning mode.

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/clear-wifi",
    verify=False
)
```

**Response**:
```json
{
  "status": "success"
}
```

**Note**: Device will restart and begin BLE provisioning. Will be unreachable via WiFi until reprovisioned.

---

## Sensor Operations

### List All Sensors

**Endpoint**: `GET /api/sensors`

**Description**: Get all connected sensors with current readings and configuration.

**Response**:
```json
{
  "sensors": [
    {
      "address": 99,
      "type": "pH",
      "name": "pH Sensor",
      "value": 6.84,
      "unit": "pH",
      "led": true,
      "connected": true,
      "firmware": "2.1",
      "capabilities": ["led", "sleep", "temp_comp", "calibration"]
    },
    {
      "address": 100,
      "type": "EC",
      "name": "Conductivity",
      "value": 1250.5,
      "unit": "µS/cm",
      "conductivity": 1250.5,
      "tds": 625.0,
      "salinity": 0.64,
      "sg": 1.001,
      "led": true,
      "connected": true,
      "firmware": "1.5",
      "probe_type": "1.0"
    },
    {
      "address": 102,
      "type": "RTD",
      "name": "Temperature",
      "value": 22.5,
      "unit": "°C",
      "scale": "C",
      "led": true,
      "connected": true,
      "firmware": "2.0"
    },
    {
      "address": 111,
      "type": "HUM",
      "name": "Humidity",
      "humidity": 42.0,
      "air_temp": 21.76,
      "dew_point": 8.33,
      "led": true,
      "connected": true,
      "firmware": "1.0"
    }
  ],
  "count": 4
}
```

**Sensor Types**:
- `pH`: pH sensor (value in pH units)
- `EC`: Electrical Conductivity (conductivity, TDS, salinity, specific gravity)
- `RTD`: Temperature (value in °C or °F)
- `DO`: Dissolved Oxygen (value in mg/L or % saturation)
- `ORP`: Oxidation-Reduction Potential (value in mV)
- `HUM`: Humidity + Temperature + Dew Point

---

### Configure Single Sensor

**Endpoint**: `POST /api/sensors/config`

**Description**: Update sensor configuration (LED, name, scale, etc).

**Request Body**:
```json
{
  "address": 99,
  "led": true,
  "name": "pH Sensor - Tank 1",
  "scale": "C"
}
```

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/sensors/config",
    json={
        "address": 99,
        "led": false,
        "name": "pH Sensor - Tank 1"
    },
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "sensor": {
    "address": 99,
    "type": "pH",
    "name": "pH Sensor - Tank 1",
    "led": false,
    "value": 6.84
  }
}
```

**Configurable Fields**:
- `led` (boolean): Enable/disable LED
- `name` (string, 1-32 chars): Sensor name
- `scale` (string, RTD only): "C" or "F" for temperature scale

---

### Configure Multiple Sensors (Batch)

**Endpoint**: `POST /api/sensors/config/batch`

**Description**: Configure multiple sensors in a single request. Useful for dashboard "Save All" functionality.

**Request Body**:
```json
{
  "sensors": [
    {
      "address": 99,
      "led": false,
      "name": "pH - Tank 1"
    },
    {
      "address": 100,
      "led": false,
      "name": "EC - Tank 1"
    },
    {
      "address": 102,
      "scale": "F",
      "name": "Temp - Tank 1"
    }
  ]
}
```

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/sensors/config/batch",
    json={
        "sensors": [
            {"address": 99, "led": False, "name": "pH - Tank 1"},
            {"address": 100, "led": False, "name": "EC - Tank 1"},
            {"address": 102, "scale": "F", "name": "Temp - Tank 1"}
        ]
    },
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "updated": 3,
  "failed": 0,
  "results": [
    {
      "address": 99,
      "status": "success",
      "sensor": {
        "address": 99,
        "name": "pH - Tank 1",
        "led": false
      }
    },
    {
      "address": 100,
      "status": "success",
      "sensor": {
        "address": 100,
        "name": "EC - Tank 1",
        "led": false
      }
    },
    {
      "address": 102,
      "status": "success",
      "sensor": {
        "address": 102,
        "name": "Temp - Tank 1",
        "scale": "F"
      }
    }
  ]
}
```

**Error Response** (partial failure):
```json
{
  "status": "partial",
  "updated": 2,
  "failed": 1,
  "results": [
    {
      "address": 99,
      "status": "success",
      "sensor": {...}
    },
    {
      "address": 100,
      "status": "success",
      "sensor": {...}
    },
    {
      "address": 102,
      "status": "error",
      "error": "Sensor not found"
    }
  ]
}
```

**Rate Limit**: Max 10 sensors per request

---

### Rescan I2C Bus

**Endpoint**: `POST /api/sensors/rescan`

**Description**: Rescan the I2C bus for new or removed sensors.

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/sensors/rescan",
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "sensors_found": 4,
  "message": "I2C bus rescanned"
}
```

**Note**: Takes ~10-15 seconds. Sensor readings pause during scan.

---

### Get Sensor Status

**Endpoint**: `GET /api/sensors/status/<address>`

**Description**: Get detailed status for a specific sensor (refreshes settings from device).

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/api/sensors/status/99",
    verify=False
)
```

**Response**:
```json
{
  "address": 99,
  "type": "pH",
  "name": "pH Sensor",
  "value": 6.84,
  "unit": "pH",
  "led": true,
  "connected": true,
  "firmware": "2.1",
  "voltage": 3.3,
  "restart_reason": "power_on",
  "capabilities": ["led", "sleep", "temp_comp", "calibration"]
}
```

---

### Sample Single Reading

**Endpoint**: `GET /api/sensors/sample/<address>`

**Description**: Take a single immediate reading from sensor (bypasses cache).

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/api/sensors/sample/99",
    verify=False
)
```

**Response**:
```json
{
  "address": 99,
  "type": "pH",
  "value": 6.84,
  "unit": "pH",
  "timestamp": 1702435200
}
```

**Note**: Takes ~900ms to complete (sensor response time).

---

### Pause Sensor Readings

**Endpoint**: `POST /api/sensors/pause`

**Description**: Pause automatic sensor readings (for calibration or maintenance).

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/sensors/pause",
    verify=False
)
```

**Response**:
```json
{
  "status": "paused"
}
```

---

### Resume Sensor Readings

**Endpoint**: `POST /api/sensors/resume`

**Description**: Resume automatic sensor readings.

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/sensors/resume",
    verify=False
)
```

**Response**:
```json
{
  "status": "resumed"
}
```

---

### Calibrate Sensor

**Endpoint**: `POST /api/sensors/calibrate/<address>`

**Description**: Perform sensor calibration (varies by sensor type).

**pH Sensor** (mid, low, high points):
```python
# Mid-point calibration (pH 7.00)
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/99",
    json={"point": "mid", "value": 7.00},
    verify=False
)

# Low-point calibration (pH 4.00)
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/99",
    json={"point": "low", "value": 4.00},
    verify=False
)

# High-point calibration (pH 10.00)
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/99",
    json={"point": "high", "value": 10.00},
    verify=False
)

# Clear calibration
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/99",
    json={"action": "clear"},
    verify=False
)
```

**EC Sensor** (dry, single, low, high points):
```python
# Dry calibration
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/100",
    json={"point": "dry"},
    verify=False
)

# Single-point calibration (12,880 µS/cm)
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/100",
    json={"point": "single", "value": 12880},
    verify=False
)
```

**RTD Sensor**:
```python
# Calibrate to known temperature (25.0°C)
response = requests.post(
    f"https://{device_ip}/api/sensors/calibrate/102",
    json={"value": 25.0},
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "sensor": {
    "address": 99,
    "type": "pH",
    "value": 7.00,
    "calibrated": true
  }
}
```

---

### Temperature Compensation

**Endpoint**: `POST /api/sensors/compensate/<address>`

**Description**: Set temperature compensation for pH/EC sensors.

**Request**:
```python
# Set temperature compensation to 25.0°C
response = requests.post(
    f"https://{device_ip}/api/sensors/compensate/99",
    json={"temperature": 25.0},
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "sensor": {
    "address": 99,
    "type": "pH",
    "temp_compensation": 25.0
  }
}
```

---

### Set Sensor Mode

**Endpoint**: `POST /api/sensors/mode/<address>`

**Description**: Toggle continuous/polling mode (if supported).

**Request**:
```python
# Enable continuous mode
response = requests.post(
    f"https://{device_ip}/api/sensors/mode/99",
    json={"mode": "continuous"},
    verify=False
)

# Enable polling mode (power saving)
response = requests.post(
    f"https://{device_ip}/api/sensors/mode/99",
    json={"mode": "polling"},
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "sensor": {
    "address": 99,
    "mode": "continuous"
  }
}
```

---

### Sensor Power Management

**Endpoint**: `POST /api/sensors/power/<address>`

**Description**: Put sensor into sleep/wake mode.

**Request**:
```python
# Sleep sensor (power saving)
response = requests.post(
    f"https://{device_ip}/api/sensors/power/99",
    json={"state": "sleep"},
    verify=False
)

# Wake sensor
response = requests.post(
    f"https://{device_ip}/api/sensors/power/99",
    json={"state": "wake"},
    verify=False
)
```

**Response**:
```json
{
  "status": "success",
  "sensor": {
    "address": 99,
    "power_state": "sleep"
  }
}
```

---

## Settings & Configuration

### Get Settings

**Endpoint**: `GET /api/settings`

**Description**: Get current device settings (intervals, thresholds).

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/api/settings",
    verify=False
)
```

**Response**:
```json
{
  "mqtt_interval": 10,
  "sensor_interval": 2
}
```

**Fields**:
- `mqtt_interval`: MQTT publish interval (seconds, 1-3600)
- `sensor_interval`: Sensor reading interval (seconds, 1-3600)

---

### Update Settings

**Endpoint**: `POST /api/settings`

**Description**: Update device settings.

**Request Body**:
```json
{
  "mqtt_interval": 30,
  "sensor_interval": 5
}
```

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/settings",
    json={
        "mqtt_interval": 30,
        "sensor_interval": 5
    },
    verify=False
)
```

**Response**:
```json
{
  "status": "saved"
}
```

**Validation**:
- `mqtt_interval`: 1-3600 seconds
- `sensor_interval`: 1-3600 seconds

---

### Reset Settings to Defaults

**Endpoint**: `POST /api/settings/reset`

**Description**: Reset all settings to factory defaults.

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/settings/reset",
    verify=False
)
```

**Response**:
```json
{
  "status": "reset",
  "mqtt_interval": 10,
  "sensor_interval": 10
}
```

---

## Advanced Operations

### MQTT Configuration

#### Test MQTT Connection

**Endpoint**: `POST /api/test-mqtt`

**Description**: Test MQTT broker connection.

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/test-mqtt",
    verify=False
)
```

**Response**:
```json
{
  "status": "tested",
  "connected": true
}
```

---

### WebSocket Real-Time Streaming

**Endpoint**: `wss://<device-ip>/ws`

**Description**: Real-time sensor data stream via WebSocket (Secure WebSocket).

#### Connect to WebSocket

```python
import asyncio
import websockets
import ssl
import json

async def stream_sensors(device_ip):
    # Disable SSL verification (self-signed cert)
    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    
    uri = f"wss://{device_ip}/ws"
    
    async with websockets.connect(uri, ssl=ssl_context) as websocket:
        print("Connected to WebSocket")
        
        while True:
            message = await websocket.recv()
            data = json.loads(message)
            print(f"Received: {data}")

# Run
asyncio.run(stream_sensors("192.168.1.100"))
```

#### WebSocket Message Format

**Sensor Update**:
```json
{
  "type": "sensor_update",
  "timestamp": 1702435200,
  "data": {
    "battery": {
      "voltage": 3.7,
      "percentage": 85
    },
    "sensors": {
      "pH": {
        "value": 6.84,
        "address": 99
      },
      "EC": {
        "value": 1250.5,
        "conductivity": 1250.5,
        "tds": 625.0,
        "address": 100
      },
      "RTD": {
        "value": 22.5,
        "address": 102
      },
      "HUM": {
        "humidity": 42.0,
        "air_temp": 21.76,
        "dew_point": 8.33,
        "address": 111
      }
    }
  }
}
```

#### Focus Mode (Single Sensor Streaming)

Send command to focus on specific sensor (high-frequency updates):

```python
# Focus on pH sensor (address 99)
await websocket.send(json.dumps({
    "action": "focus",
    "address": 99
}))

# Stop focus mode
await websocket.send(json.dumps({
    "action": "unfocus"
}))
```

**Focus Response** (10 Hz updates):
```json
{
  "type": "focus_update",
  "address": 99,
  "value": 6.84,
  "timestamp": 1702435200
}
```

---

### Certificate Management

#### Download CA Certificate

**Endpoint**: `GET /ca.crt`

**Description**: Download device CA certificate (for validating HTTPS).

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/ca.crt",
    verify=False
)

with open('kc_device_ca.crt', 'wb') as f:
    f.write(response.content)

# Use certificate for verified requests
response = requests.get(
    f"https://{device_ip}/api/status",
    verify='kc_device_ca.crt'
)
```

---

### Web File Management

#### List Dashboard Files

**Endpoint**: `GET /api/webfiles/list`

**Description**: List all dashboard files in FATFS partition.

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/api/webfiles/list",
    verify=False
)
```

**Response**:
```json
{
  "files": [
    {
      "name": "index.html",
      "size": 12345
    },
    {
      "name": "dashboard.js",
      "size": 45678
    },
    {
      "name": "dashboard.css",
      "size": 8901
    }
  ],
  "count": 3
}
```

---

#### Get File Content

**Endpoint**: `GET /api/webfiles/<filename>`

**Description**: Get dashboard file content.

**Request**:
```python
response = requests.get(
    f"https://{device_ip}/api/webfiles/index.html",
    verify=False
)

html_content = response.text
```

---

#### Update File Content

**Endpoint**: `PUT /api/webfiles/<filename>`

**Description**: Update dashboard file content.

**Request**:
```python
with open('index.html', 'r') as f:
    content = f.read()

response = requests.put(
    f"https://{device_ip}/api/webfiles/index.html",
    data=content,
    headers={'Content-Type': 'text/html'},
    verify=False
)
```

**Response**:
```json
{
  "status": "saved",
  "filename": "index.html",
  "size": 12345
}
```

**File Size Limit**: 200KB per file

---

#### Reset Dashboard to Defaults

**Endpoint**: `POST /api/webfiles/reset`

**Description**: Format FATFS partition and restore default dashboard files.

**Request**:
```python
response = requests.post(
    f"https://{device_ip}/api/webfiles/reset",
    verify=False
)
```

**Response**:
```json
{
  "success": true,
  "message": "filesystem reset"
}
```

**Warning**: This erases all custom dashboard modifications.

---

### Firmware Updates (OTA)

#### Upload Firmware

**Endpoint**: `POST /api/firmware/upload`

**Description**: Upload new firmware for Over-The-Air (OTA) update.

**Request**:
```python
with open('kc_device.bin', 'rb') as f:
    firmware = f.read()

response = requests.post(
    f"https://{device_ip}/api/firmware/upload",
    data=firmware,
    headers={'Content-Type': 'application/octet-stream'},
    verify=False,
    timeout=60  # Firmware upload takes time
)
```

**Response**:
```json
{
  "status": "uploaded",
  "reboot": true
}
```

**Process**:
1. Firmware uploaded to OTA partition
2. Device validates firmware
3. Device reboots in 1.5 seconds
4. Boots from new partition
5. If successful, makes it permanent

**Notes**:
- Upload takes ~30 seconds for 1.6MB firmware
- Device unreachable during update (~60 seconds total)
- Automatic rollback if new firmware fails to boot

---

## Rate Limits & Best Practices

### Rate Limits

| Operation | Limit | Notes |
|-----------|-------|-------|
| Sensor readings | 1 request/sec per sensor | Use WebSocket for faster updates |
| Batch operations | 10 sensors max | For `/api/sensors/config/batch` |
| I2C operations | 1 concurrent | Rescan, calibration, mode changes |
| WebSocket messages | 100/sec | Focused sensor updates: 10 Hz |
| File uploads | 200KB max | Dashboard files |
| Firmware uploads | 5MB max | OTA updates |

### Best Practices

#### 1. Use WebSocket for Real-Time Data

**Don't** poll REST API rapidly:
```python
# ❌ Bad - hammers device with requests
while True:
    response = requests.get(f"{base_url}/api/sensors")
    time.sleep(1)
```

**Do** use WebSocket:
```python
# ✅ Good - efficient real-time updates
async with websockets.connect(ws_url) as ws:
    async for message in ws:
        data = json.loads(message)
        process_sensor_data(data)
```

#### 2. Batch Sensor Configuration

**Don't** configure sensors one-by-one:
```python
# ❌ Bad - 3 separate requests
for sensor in [99, 100, 102]:
    requests.post(f"{base_url}/api/sensors/config", 
                  json={"address": sensor, "led": False})
```

**Do** use batch endpoint:
```python
# ✅ Good - single request
requests.post(f"{base_url}/api/sensors/config/batch",
              json={"sensors": [
                  {"address": 99, "led": False},
                  {"address": 100, "led": False},
                  {"address": 102, "led": False}
              ]})
```

#### 3. Cache Device Status

**Don't** fetch status on every page load:
```python
# ❌ Bad - unnecessary requests
def get_dashboard_data():
    status = requests.get(f"{base_url}/api/status").json()
    sensors = requests.get(f"{base_url}/api/sensors").json()
    return status, sensors
```

**Do** cache and refresh periodically:
```python
# ✅ Good - cache with TTL
from cachetools import TTLCache, cached

cache = TTLCache(maxsize=100, ttl=10)  # 10 second cache

@cached(cache)
def get_device_status(device_ip):
    return requests.get(f"https://{device_ip}/api/status").json()
```

#### 4. Handle Self-Signed Certificates

Create a session with disabled verification:
```python
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

session = requests.Session()
session.verify = False

# Reuse session for all requests
response = session.get(f"{base_url}/api/sensors")
```

#### 5. Implement Timeouts

Always set timeouts to prevent hanging:
```python
# ✅ Good - with timeout
response = requests.get(
    f"{base_url}/api/sensors",
    verify=False,
    timeout=5  # 5 second timeout
)
```

#### 6. Connection Pooling

Use session for multiple requests:
```python
with requests.Session() as session:
    session.verify = False
    
    # Multiple requests reuse connection
    status = session.get(f"{base_url}/api/status").json()
    sensors = session.get(f"{base_url}/api/sensors").json()
    settings = session.get(f"{base_url}/api/settings").json()
```

---

## Python Client Examples

### Complete Client Library

```python
import requests
import urllib3
import asyncio
import websockets
import ssl
import json
from typing import Dict, List, Optional

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

class KCDeviceClient:
    """Python client for KC Device REST API"""
    
    def __init__(self, ip_address: str, timeout: int = 5):
        self.ip = ip_address
        self.base_url = f"https://{ip_address}"
        self.ws_url = f"wss://{ip_address}/ws"
        self.timeout = timeout
        self.session = requests.Session()
        self.session.verify = False
    
    # Device Management
    
    def get_status(self) -> Dict:
        """Get device status"""
        response = self.session.get(
            f"{self.base_url}/api/status",
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def get_device_info(self) -> Dict:
        """Get device ID and name"""
        response = self.session.get(
            f"{self.base_url}/api/device/info",
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def set_device_name(self, name: str) -> Dict:
        """Set device name"""
        response = self.session.post(
            f"{self.base_url}/api/device/name",
            json={"device_name": name},
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def reboot(self):
        """Reboot device"""
        self.session.post(
            f"{self.base_url}/api/reboot",
            timeout=self.timeout
        )
    
    def clear_wifi(self):
        """Clear WiFi credentials"""
        self.session.post(
            f"{self.base_url}/api/clear-wifi",
            timeout=self.timeout
        )
    
    # Sensor Operations
    
    def get_sensors(self) -> Dict:
        """Get all sensors"""
        response = self.session.get(
            f"{self.base_url}/api/sensors",
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def configure_sensor(self, address: int, **config) -> Dict:
        """Configure single sensor"""
        payload = {"address": address, **config}
        response = self.session.post(
            f"{self.base_url}/api/sensors/config",
            json=payload,
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def configure_sensors_batch(self, sensors: List[Dict]) -> Dict:
        """Configure multiple sensors at once"""
        response = self.session.post(
            f"{self.base_url}/api/sensors/config/batch",
            json={"sensors": sensors},
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def rescan_sensors(self) -> Dict:
        """Rescan I2C bus for sensors"""
        response = self.session.post(
            f"{self.base_url}/api/sensors/rescan",
            timeout=self.timeout + 10  # Longer timeout
        )
        response.raise_for_status()
        return response.json()
    
    def get_sensor_status(self, address: int) -> Dict:
        """Get detailed sensor status"""
        response = self.session.get(
            f"{self.base_url}/api/sensors/status/{address}",
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def sample_sensor(self, address: int) -> Dict:
        """Take immediate sensor reading"""
        response = self.session.get(
            f"{self.base_url}/api/sensors/sample/{address}",
            timeout=self.timeout + 1  # Sensor response time
        )
        response.raise_for_status()
        return response.json()
    
    def pause_sensors(self):
        """Pause sensor readings"""
        self.session.post(
            f"{self.base_url}/api/sensors/pause",
            timeout=self.timeout
        )
    
    def resume_sensors(self):
        """Resume sensor readings"""
        self.session.post(
            f"{self.base_url}/api/sensors/resume",
            timeout=self.timeout
        )
    
    def calibrate_sensor(self, address: int, **params) -> Dict:
        """Calibrate sensor"""
        response = self.session.post(
            f"{self.base_url}/api/sensors/calibrate/{address}",
            json=params,
            timeout=self.timeout + 2
        )
        response.raise_for_status()
        return response.json()
    
    def set_temperature_compensation(self, address: int, temperature: float) -> Dict:
        """Set temperature compensation"""
        response = self.session.post(
            f"{self.base_url}/api/sensors/compensate/{address}",
            json={"temperature": temperature},
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    # Settings
    
    def get_settings(self) -> Dict:
        """Get device settings"""
        response = self.session.get(
            f"{self.base_url}/api/settings",
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def update_settings(self, **settings) -> Dict:
        """Update device settings"""
        response = self.session.post(
            f"{self.base_url}/api/settings",
            json=settings,
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    def reset_settings(self) -> Dict:
        """Reset settings to defaults"""
        response = self.session.post(
            f"{self.base_url}/api/settings/reset",
            timeout=self.timeout
        )
        response.raise_for_status()
        return response.json()
    
    # WebSocket
    
    async def stream_sensors(self, callback):
        """Stream sensor data via WebSocket"""
        ssl_context = ssl.create_default_context()
        ssl_context.check_hostname = False
        ssl_context.verify_mode = ssl.CERT_NONE
        
        async with websockets.connect(self.ws_url, ssl=ssl_context) as ws:
            async for message in ws:
                data = json.loads(message)
                await callback(data)
    
    async def focus_sensor(self, websocket, address: int):
        """Focus on specific sensor for high-frequency updates"""
        await websocket.send(json.dumps({
            "action": "focus",
            "address": address
        }))
    
    async def unfocus_sensor(self, websocket):
        """Stop focused sensor updates"""
        await websocket.send(json.dumps({
            "action": "unfocus"
        }))
    
    # Cleanup
    
    def close(self):
        """Close session"""
        self.session.close()
    
    def __enter__(self):
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


# Usage Examples

# Context manager
with KCDeviceClient("192.168.1.100") as device:
    status = device.get_status()
    sensors = device.get_sensors()
    print(f"Device: {status['device_id']}")
    print(f"Sensors: {sensors['count']}")

# Configure multiple sensors
device = KCDeviceClient("192.168.1.100")
result = device.configure_sensors_batch([
    {"address": 99, "led": False, "name": "pH - Tank 1"},
    {"address": 100, "led": False, "name": "EC - Tank 1"},
    {"address": 102, "scale": "F", "name": "Temp - Tank 1"}
])
print(f"Updated: {result['updated']}, Failed: {result['failed']}")

# WebSocket streaming
async def handle_sensor_data(data):
    if data['type'] == 'sensor_update':
        print(f"Sensors: {data['data']['sensors']}")

device = KCDeviceClient("192.168.1.100")
asyncio.run(device.stream_sensors(handle_sensor_data))
```

---

## Error Handling

### HTTP Status Codes

| Code | Meaning | When It Occurs |
|------|---------|----------------|
| 200 | Success | Request completed successfully |
| 400 | Bad Request | Invalid JSON, missing parameters, validation failure |
| 404 | Not Found | Sensor address doesn't exist, file not found |
| 500 | Internal Server Error | Device error, I2C failure, NVS error |

### Error Response Format

```json
{
  "error": "Error message here",
  "details": "Additional context"
}
```

### Common Errors

#### Sensor Not Found
```json
{
  "error": "Sensor not found",
  "address": 99
}
```

#### Invalid JSON
```json
{
  "error": "Invalid JSON"
}
```

#### Calibration Not Supported
```json
{
  "error": "Calibration not supported for this sensor"
}
```

#### I2C Communication Error
```json
{
  "error": "I2C communication failed",
  "address": 99
}
```

### Python Error Handling

```python
from requests.exceptions import HTTPError, Timeout, ConnectionError

try:
    response = requests.get(
        f"{base_url}/api/sensors",
        verify=False,
        timeout=5
    )
    response.raise_for_status()
    sensors = response.json()
    
except HTTPError as e:
    if e.response.status_code == 404:
        print("Sensor not found")
    elif e.response.status_code == 500:
        print("Device error")
    else:
        print(f"HTTP error: {e}")
        
except Timeout:
    print("Request timed out - device may be offline")
    
except ConnectionError:
    print("Cannot connect to device - check IP address")
    
except Exception as e:
    print(f"Unexpected error: {e}")
```

---

## Appendix

### Complete Endpoint List

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Serve dashboard (index.html) |
| GET | `/favicon.ico` | Favicon |
| GET | `/ca.crt` | Download CA certificate |
| GET | `/api/status` | Device status |
| GET | `/api/device/info` | Device ID & name |
| POST | `/api/device/name` | Set device name |
| POST | `/api/reboot` | Reboot device |
| POST | `/api/clear-wifi` | Clear WiFi credentials |
| POST | `/api/test-mqtt` | Test MQTT connection |
| GET | `/api/settings` | Get settings |
| POST | `/api/settings` | Update settings |
| POST | `/api/settings/reset` | Reset settings |
| GET | `/api/sensors` | List all sensors |
| POST | `/api/sensors/config` | Configure single sensor |
| POST | `/api/sensors/config/batch` | Configure multiple sensors |
| POST | `/api/sensors/rescan` | Rescan I2C bus |
| POST | `/api/sensors/pause` | Pause readings |
| POST | `/api/sensors/resume` | Resume readings |
| GET | `/api/sensors/status/<address>` | Get sensor status |
| GET | `/api/sensors/sample/<address>` | Sample sensor reading |
| POST | `/api/sensors/calibrate/<address>` | Calibrate sensor |
| POST | `/api/sensors/compensate/<address>` | Set temp compensation |
| POST | `/api/sensors/mode/<address>` | Set sensor mode |
| POST | `/api/sensors/power/<address>` | Sleep/wake sensor |
| GET | `/api/webfiles/list` | List dashboard files |
| GET | `/api/webfiles/<filename>` | Get file content |
| PUT | `/api/webfiles/<filename>` | Update file |
| POST | `/api/webfiles/reset` | Reset dashboard |
| POST | `/api/firmware/upload` | Upload firmware (OTA) |
| WS | `/ws` | WebSocket real-time stream |

---

**Document Version**: 1.0  
**Last Updated**: 2025-12-13  
**Device Firmware**: v1.0+  
**Compatible With**: ESP32-S3, ESP32-C6
