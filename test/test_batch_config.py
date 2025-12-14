#!/usr/bin/env python3
"""
Test script for batch sensor configuration endpoint
"""

import requests
import urllib3
import json

# Disable SSL warnings for self-signed certificate
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Device configuration
DEVICE_IP = "192.168.1.100"  # Update with your device IP
BASE_URL = f"https://{DEVICE_IP}"

def test_batch_config():
    """Test batch sensor configuration"""
    
    print("=" * 70)
    print("KC Device - Batch Sensor Configuration Test")
    print("=" * 70)
    
    # Get current sensors
    print("\n1. Getting current sensor list...")
    try:
        response = requests.get(
            f"{BASE_URL}/api/sensors",
            verify=False,
            timeout=5
        )
        response.raise_for_status()
        sensors_data = response.json()
        
        print(f"   Found {sensors_data['count']} sensors:")
        for sensor in sensors_data['sensors']:
            print(f"   - Address {sensor['address']}: {sensor['name']} ({sensor['type']})")
            print(f"     LED: {sensor['led']}, Connected: {sensor['connected']}")
        
    except Exception as e:
        print(f"   ERROR: {e}")
        return
    
    # Prepare batch configuration
    print("\n2. Preparing batch configuration...")
    
    # Build batch config based on discovered sensors
    batch_config = {
        "sensors": []
    }
    
    for sensor in sensors_data['sensors']:
        config = {
            "address": sensor['address'],
            "led": False,  # Turn off all LEDs
            "name": f"{sensor['type']}_Test"
        }
        
        # Add scale for RTD sensors
        if sensor['type'] == 'RTD':
            config['scale'] = 'F'
        
        batch_config['sensors'].append(config)
        print(f"   - Address {sensor['address']}: LED=False, Name={config['name']}")
    
    # Send batch configuration
    print("\n3. Sending batch configuration...")
    try:
        response = requests.post(
            f"{BASE_URL}/api/sensors/config/batch",
            json=batch_config,
            verify=False,
            timeout=10
        )
        response.raise_for_status()
        result = response.json()
        
        print(f"\n   Status: {result['status']}")
        print(f"   Updated: {result['updated']}")
        print(f"   Failed: {result['failed']}")
        
        print("\n   Results:")
        for item in result['results']:
            if item['status'] == 'success':
                print(f"   ✓ Address {item['address']}: SUCCESS")
                if 'sensor' in item:
                    s = item['sensor']
                    print(f"     Name: {s['name']}, LED: {s['led']}")
            else:
                print(f"   ✗ Address {item['address']}: FAILED - {item['error']}")
        
    except Exception as e:
        print(f"   ERROR: {e}")
        return
    
    # Verify changes
    print("\n4. Verifying changes...")
    try:
        response = requests.get(
            f"{BASE_URL}/api/sensors",
            verify=False,
            timeout=5
        )
        response.raise_for_status()
        sensors_data = response.json()
        
        print(f"   Current sensor states:")
        for sensor in sensors_data['sensors']:
            print(f"   - Address {sensor['address']}: {sensor['name']}")
            print(f"     LED: {sensor['led']}, Value: {sensor['value']} {sensor['unit']}")
        
    except Exception as e:
        print(f"   ERROR: {e}")
        return
    
    print("\n" + "=" * 70)
    print("Test completed successfully!")
    print("=" * 70)


def test_batch_error_handling():
    """Test batch configuration error handling"""
    
    print("\n" + "=" * 70)
    print("Testing Error Handling")
    print("=" * 70)
    
    # Test 1: Invalid sensor address
    print("\n1. Testing invalid sensor address...")
    try:
        response = requests.post(
            f"{BASE_URL}/api/sensors/config/batch",
            json={
                "sensors": [
                    {"address": 99, "led": False},
                    {"address": 255, "led": True},  # Invalid address
                    {"address": 100, "name": "EC_Test"}
                ]
            },
            verify=False,
            timeout=10
        )
        result = response.json()
        
        print(f"   Status: {result['status']}")
        print(f"   Updated: {result['updated']}, Failed: {result['failed']}")
        
        for item in result['results']:
            status_icon = "✓" if item['status'] == 'success' else "✗"
            print(f"   {status_icon} Address {item['address']}: {item['status'].upper()}")
            if item['status'] == 'error':
                print(f"      Error: {item['error']}")
        
    except Exception as e:
        print(f"   ERROR: {e}")
    
    # Test 2: Invalid name (too long)
    print("\n2. Testing invalid sensor name...")
    try:
        response = requests.post(
            f"{BASE_URL}/api/sensors/config/batch",
            json={
                "sensors": [
                    {"address": 99, "name": "This_Name_Is_Way_Too_Long_For_Sensor"}
                ]
            },
            verify=False,
            timeout=10
        )
        result = response.json()
        
        print(f"   Status: {result['status']}")
        if result['failed'] > 0:
            print(f"   Failed as expected: {result['results'][0]['error']}")
        
    except Exception as e:
        print(f"   ERROR: {e}")
    
    # Test 3: Exceeding batch size limit
    print("\n3. Testing batch size limit (>10 sensors)...")
    try:
        large_batch = {
            "sensors": [{"address": i, "led": False} for i in range(99, 120)]
        }
        
        response = requests.post(
            f"{BASE_URL}/api/sensors/config/batch",
            json=large_batch,
            verify=False,
            timeout=10
        )
        
        if response.status_code == 400:
            print(f"   Correctly rejected: {response.text}")
        else:
            print(f"   Unexpected response: {response.status_code}")
        
    except Exception as e:
        print(f"   ERROR: {e}")


if __name__ == "__main__":
    # Update device IP
    print("\nNote: Update DEVICE_IP in this script if not using 192.168.1.100")
    print(f"Current target: {BASE_URL}\n")
    
    try:
        # Run main test
        test_batch_config()
        
        # Run error handling tests
        test_batch_error_handling()
        
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
    except Exception as e:
        print(f"\n\nUnexpected error: {e}")
