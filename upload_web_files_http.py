#!/usr/bin/env python3
"""
Upload web files to ESP32 dashboard via HTTP
This uploads files directly to a running device over WiFi without needing USB connection.
Perfect for remote updates!
"""

import os
import sys
import requests
from urllib3.exceptions import InsecureRequestWarning

# Disable SSL warnings (ESP32 uses self-signed certificate)
requests.packages.urllib3.disable_warnings(category=InsecureRequestWarning)

def main():
    print("=" * 60)
    print("ESP32 Web Files HTTP Upload Script")
    print("=" * 60)
    print()
    
    # Parse arguments
    if len(sys.argv) < 2:
        print("Usage: python upload_web_files_http.py <hostname_or_ip>")
        print("Examples:")
        print("  python upload_web_files_http.py kc.local")
        print("  python upload_web_files_http.py 192.168.0.215")
        print()
        sys.exit(1)
    
    host = sys.argv[1]
    
    # Build URL (use https since dashboard uses HTTPS)
    base_url = f"https://{host}"
    
    # Check if web files exist
    web_dir = os.path.join("main", "web")
    if not os.path.exists(web_dir):
        print(f"❌ Error: Web directory not found: {web_dir}")
        sys.exit(1)
    
    # List web files
    web_files = []
    for filename in os.listdir(web_dir):
        if filename.endswith(('.html', '.js', '.css')):
            filepath = os.path.join(web_dir, filename)
            size = os.path.getsize(filepath)
            web_files.append((filename, filepath, size))
    
    if not web_files:
        print("❌ No web files found in main/web/")
        sys.exit(1)
    
    print("Found web files:")
    total_size = 0
    for filename, filepath, size in web_files:
        print(f"  • {filename:20s} {size:8d} bytes ({size/1024:.1f} KB)")
        total_size += size
    print(f"\nTotal size: {total_size} bytes ({total_size/1024:.1f} KB)")
    print()
    
    print("=" * 60)
    print(f"Uploading to {base_url}...")
    print("=" * 60)
    print()
    
    # Test connection first
    print("Testing connection...")
    try:
        response = requests.get(f"{base_url}/api/status", verify=False, timeout=5)
        if response.status_code == 200:
            print("✓ Connection successful")
        else:
            print(f"⚠️  Warning: Got status code {response.status_code}")
    except Exception as e:
        print(f"❌ Error: Cannot connect to {base_url}")
        print(f"    {str(e)}")
        print()
        print("Make sure:")
        print("  1. ESP32 is powered on and connected to WiFi")
        print("  2. Hostname resolves (try IP address instead)")
        print("  3. Dashboard server is running")
        sys.exit(1)
    print()
    
    # Upload each file
    success_count = 0
    fail_count = 0
    
    for filename, filepath, size in web_files:
        print(f"Uploading {filename}...", end=" ")
        
        try:
            # Read file content
            with open(filepath, 'r', encoding='utf-8') as f:
                content = f.read()
            
            # Upload via PUT (not POST) to web editor API
            upload_url = f"{base_url}/api/webfiles/{filename}"
            response = requests.put(
                upload_url,
                data=content.encode('utf-8'),
                headers={
                    'Content-Type': 'text/plain; charset=utf-8',
                    'Connection': 'close'
                },
                verify=False,
                timeout=10
            )
            
            if response.status_code == 200:
                print("✓")
                success_count += 1
            else:
                print(f"❌ (HTTP {response.status_code})")
                print(f"    Response: {response.text}")
                fail_count += 1
                
        except Exception as e:
            print(f"❌ Error: {str(e)}")
            fail_count += 1
    
    print()
    print("=" * 60)
    
    if fail_count == 0:
        print("✓ Upload complete! All files uploaded successfully.")
    else:
        print(f"⚠️  Upload completed with errors:")
        print(f"    Success: {success_count}")
        print(f"    Failed:  {fail_count}")
    
    print("=" * 60)
    print()
    print("Dashboard updated! Changes are live immediately.")
    print(f"Visit {base_url} to see the updates.")
    print()
    print("No reboot required - files are served directly from FATFS.")
    print()

if __name__ == "__main__":
    main()
