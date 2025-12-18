# Dashboard.js Changes Required

## Changes to make:

### 1. Remove from renderSensorCard() function:
- Remove all `renderPhCalibrationSection(sensor)` calls
- Remove all `renderOrpCalibrationSection(sensor)` calls  
- Remove all `renderRtdCalibrationSection(sensor)` calls
- Remove all `renderEcCalibrationSection(sensor)` calls
- Remove all `renderDoCalibrationSection(sensor)` calls
- Keep only the "Start Calibration Wizard" button

### 2. Add new features to renderSensorCard():

After the "Save Settings" button, add:

```javascript
// Sensor diagnostics and advanced features
html += `<div class='mt-4 border-t border-gray-300 dark:border-gray-600 pt-4'>`;
html += `<h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Diagnostics & Advanced</h4>`;
html += `<div class='flex flex-wrap gap-2'>`;
html += `<button class='px-3 py-2 text-sm bg-blue-600 hover:bg-blue-700 text-white rounded' onclick='findSensor(${sensor.address})'><i class='fas fa-search-location'></i> Find (Blink LED)</button>`;
html += `<button class='px-3 py-2 text-sm bg-purple-600 hover:bg-purple-700 text-white rounded' onclick='getDeviceStatus(${sensor.address})'><i class='fas fa-info-circle'></i> Device Status</button>`;

// Export/Import for sensors that support calibration
if (supportsCalibration) {
  html += `<button class='px-3 py-2 text-sm bg-green-600 hover:bg-green-700 text-white rounded' onclick='exportCalibration(${sensor.address})'><i class='fas fa-download'></i> Export Cal</button>`;
  html += `<button class='px-3 py-2 text-sm bg-green-600 hover:bg-green-700 text-white rounded' onclick='importCalibration(${sensor.address})'><i class='fas fa-upload'></i> Import Cal</button>`;
}

// pH-specific: Slope query
if (isPh) {
  html += `<button class='px-3 py-2 text-sm bg-indigo-600 hover:bg-indigo-700 text-white rounded' onclick='querySlope(${sensor.address})'><i class='fas fa-chart-line'></i> Check Slope</button>`;
}

html += `</div></div>`;

// EC-specific advanced features
if (isEc) {
  html += `<div class='mt-4 border border-gray-300 dark:border-gray-600 rounded-lg p-4'>`;
  html += `<h4 class='text-sm font-semibold text-gray-900 dark:text-white mb-3'>EC Advanced Settings</h4>`;
  
  // EC Temperature Compensation
  html += `<div class='mb-3'><label class='block text-sm text-gray-700 dark:text-gray-300 mb-1'>Temperature Compensation (°C):</label>`;
  html += `<div class='flex gap-2'><input id='ec-temp-${sensor.address}' type='number' step='0.1' value='${sensor.ec_temp_comp || 25}' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded bg-white dark:bg-gray-800 text-gray-900 dark:text-white'>`;
  html += `<button class='px-3 py-1 text-sm bg-blue-600 text-white rounded' onclick='setEcTempComp(${sensor.address})'>Apply</button></div></div>`;
  
  // Output Parameters
  html += `<div class='mb-3'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Output Parameters:</label>`;
  html += `<div class='flex flex-wrap gap-3'>`;
  html += `<label class='flex items-center'><input type='checkbox' id='param-ec-${sensor.address}' class='mr-1'> EC</label>`;
  html += `<label class='flex items-center'><input type='checkbox' id='param-tds-${sensor.address}' class='mr-1'> TDS</label>`;
  html += `<label class='flex items-center'><input type='checkbox' id='param-s-${sensor.address}' class='mr-1'> Salinity</label>`;
  html += `<label class='flex items-center'><input type='checkbox' id='param-sg-${sensor.address}' class='mr-1'> Sp. Gravity</label>`;
  html += `</div>`;
  html += `<button class='mt-2 px-3 py-1 text-sm bg-green-600 text-white rounded' onclick='updateEcOutputParams(${sensor.address})'>Update Outputs</button></div>`;
  
  html += `</div>`;
}
```

### 3. Add new JavaScript functions at end of file:

```javascript
// New diagnostic and advanced feature functions
async function findSensor(address) {
  try {
    const res = await fetch(`/api/sensors/find/${address}`, { method: 'POST' });
    if (!res.ok) throw new Error('Find command failed');
    alert(`Sensor 0x${formatAddress(address)} LED is now blinking rapidly for identification.`);
  } catch (e) {
    alert('Find command failed: ' + e.message);
  }
}

async function getDeviceStatus(address) {
  try {
    const res = await fetch(`/api/sensors/device-status/${address}`);
    if (!res.ok) throw new Error('Status query failed');
    const data = await res.json();
    alert(`Device Status for 0x${formatAddress(address)}:\n\n${data.device_status}`);
  } catch (e) {
    alert('Status query failed: ' + e.message);
  }
}

async function exportCalibration(address) {
  try {
    const res = await fetch(`/api/sensors/export/${address}`);
    if (!res.ok) throw new Error('Export failed');
    const data = await res.json();
    
    // Show calibration data in a copyable format
    const textarea = document.createElement('textarea');
    textarea.value = data.calibration_data;
    textarea.style.position = 'fixed';
    textarea.style.top = '50%';
    textarea.style.left = '50%';
    textarea.style.transform = 'translate(-50%, -50%)';
    textarea.style.width = '80%';
    textarea.style.height = '200px';
    textarea.style.zIndex = '10000';
    textarea.style.padding = '20px';
    textarea.style.border = '2px solid #4CAF50';
    textarea.style.borderRadius = '8px';
    textarea.style.fontSize = '14px';
    document.body.appendChild(textarea);
    textarea.select();
    document.execCommand('copy');
    
    alert(`✓ Calibration data exported and copied to clipboard!\n\nData: ${data.calibration_data}\n\nSave this string to restore calibration later.`);
    document.body.removeChild(textarea);
  } catch (e) {
    alert('Export failed: ' + e.message);
  }
}

async function importCalibration(address) {
  const calibData = prompt('Paste the calibration data string:');
  if (!calibData || calibData.trim().length === 0) {
    return;
  }
  
  try {
    const res = await fetch(`/api/sensors/import/${address}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ calibration_data: calibData.trim() })
    });
    
    if (!res.ok) throw new Error('Import failed');
    alert('✓ Calibration data imported successfully!');
    await loadSensors();
  } catch (e) {
    alert('Import failed: ' + e.message);
  }
}

async function querySlope(address) {
  try {
    const res = await fetch(`/api/sensors/slope/${address}`);
    if (!res.ok) throw new Error('Slope query failed');
    const data = await res.json();
    alert(`pH Slope Values for 0x${formatAddress(address)}:\n\n${data.slope}\n\nThese values indicate calibration quality. Standard slopes are typically 99.7-100.3% for good calibration.`);
  } catch (e) {
    alert('Slope query failed: ' + e.message);
  }
}

async function setEcTempComp(address) {
  const input = document.getElementById(`ec-temp-${address}`);
  if (!input) return;
  
  const temp = parseFloat(input.value);
  if (isNaN(temp)) {
    alert('Enter a valid temperature value');
    return;
  }
  
  try {
    const res = await fetch(`/api/sensors/ec-temp-comp/${address}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ temp_c: temp })
    });
    
    if (!res.ok) throw new Error('Failed to set temperature compensation');
    alert('✓ EC temperature compensation updated');
    await loadSensors();
  } catch (e) {
    alert('Failed: ' + e.message);
  }
}

async function updateEcOutputParams(address) {
  const ec = document.getElementById(`param-ec-${address}`)?.checked || false;
  const tds = document.getElementById(`param-tds-${address}`)?.checked || false;
  const s = document.getElementById(`param-s-${address}`)?.checked || false;
  const sg = document.getElementById(`param-sg-${address}`)?.checked || false;
  
  try {
    const res = await fetch(`/api/sensors/ec-output-params/${address}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ec, tds, s, sg })
    });
    
    if (!res.ok) throw new Error('Failed to update output parameters');
    alert('✓ EC output parameters updated');
    await loadSensors();
  } catch (e) {
    alert('Failed: ' + e.message);
  }
}
```

## Manual Edit Instructions:

Since dashboard.js is minified, you need to:

1. Search for the section where calibration sections are added (line ~74)
2. Remove all calls to renderPhCalibrationSection, renderOrpCalibrationSection, etc. EXCEPT for the modal context
3. Add the new diagnostic button sections after the Save Settings button
4. Add the new JavaScript functions at the very end of the file

The key is to keep calibration IN the modal/wizard, but REMOVE it from the main sensor tab cards.
