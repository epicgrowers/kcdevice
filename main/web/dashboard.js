function toggleTheme(){const html=document.documentElement;const body=document.body;const icon=document.querySelector('#themeToggle i');if(body.classList.contains('bg-gray-900')){body.className='bg-gray-50 text-gray-900';html.classList.remove('dark');icon.className='fas fa-sun text-xl';localStorage.setItem('theme','light');}else{body.className='bg-gray-900 text-white';html.classList.add('dark');icon.className='fas fa-moon text-xl';localStorage.setItem('theme','dark');}}
function loadTheme(){const theme=localStorage.getItem('theme')||'dark';const body=document.body;const html=document.documentElement;const icon=document.querySelector('#themeToggle i');if(theme==='light'){body.className='bg-gray-50 text-gray-900';html.classList.remove('dark');icon.className='fas fa-sun text-xl';}else{body.className='bg-gray-900 text-white';html.classList.add('dark');icon.className='fas fa-moon text-xl';}}
const sensorConfig={RTD:{icon:'🌡️',label:'Temperature',unit:'°C',color:'text-red-500'},pH:{icon:'⚗️',label:'pH Level',unit:'',color:'text-purple-500'},EC:{icon:'⚡',label:'Conductivity',unit:'µS/cm',color:'text-cyan-500'},HUM:{icon:'💧',label:'Humidity',unit:'%',color:'text-blue-500'},DO:{icon:'🫧',label:'Dissolved Oxygen',unit:'mg/L',color:'text-teal-500'},ORP:{icon:'🔋',label:'ORP',unit:'mV',color:'text-pink-500'}};
const latestSensorSnapshots={};
const sensorDetailCache={};
const sensorValueHistory={};
let activeModalType=null;
let focusModeActive=false;
let focusSensorAddress=null;
let focusSensorTimer=null;
let focusPauseIssued=false;
const SENSOR_WS_PATH='/ws/sensors';
let sensorSocket=null;
let sensorSocketReady=false;
let sensorSocketReconnectTimer=null;
let focusUsingWebSocket=false;
function displaySensorValues(sensors){const container=document.getElementById('sensor-values');if(!sensors||Object.keys(sensors).length===0){container.innerHTML='<div class="text-gray-500 dark:text-gray-400">No sensor data available</div>';return;}let html='';const nowLabel=new Date().toLocaleTimeString();for(const type in sensors){const value=sensors[type];const cleanType=(type||'').trim();const typeKey=cleanType.toUpperCase();const cfg=sensorConfig[cleanType]||sensorConfig[typeKey]||{icon:'📊',label:cleanType||typeKey,unit:'',color:'text-gray-500'};latestSensorSnapshots[typeKey]={rawType:cleanType||typeKey,value,config:cfg,timestamp:nowLabel};recordSensorHistory(typeKey,value,nowLabel);if(activeModalType===typeKey){refreshSensorModalContent(typeKey);}if(typeof value==='object'&&!Array.isArray(value)){for(const field in value){const fieldLabel=field.replace('_',' ').replace(/\b\w/g,l=>l.toUpperCase());const fieldValue=typeof value[field]==='number'?value[field].toFixed(2):value[field];html+=`<div class='bg-white dark:bg-gray-700 p-4 rounded-lg border border-gray-200 dark:border-gray-600 cursor-pointer transition hover:border-green-400' onclick='openSensorModal("${typeKey}")'>`;html+=`<div class='flex items-center justify-between mb-2'>`;html+=`<span class='text-2xl'>${cfg.icon}</span>`;html+=`<span class='text-xs text-gray-500 dark:text-gray-400'>${cleanType}</span>`;html+=`</div>`;html+=`<div class='text-sm text-gray-600 dark:text-gray-300 mb-1'>${fieldLabel}</div>`;html+=`<div class='text-2xl font-bold ${cfg.color}'>${fieldValue}</div>`;html+=`</div>`;}}else if(typeof value==='number'){html+=`<div class='bg-white dark:bg-gray-700 p-4 rounded-lg border border-gray-200 dark:border-gray-600 cursor-pointer transition hover:border-green-400' onclick='openSensorModal("${typeKey}")'>`;html+=`<div class='flex items-center justify-between mb-2'>`;html+=`<span class='text-2xl'>${cfg.icon}</span>`;html+=`<span class='text-xs text-gray-500 dark:text-gray-400'>${cleanType}</span>`;html+=`</div>`;html+=`<div class='text-sm text-gray-600 dark:text-gray-300 mb-1'>${cfg.label}</div>`;html+=`<div class='text-2xl font-bold ${cfg.color}'>${value.toFixed(2)} ${cfg.unit}</div>`;html+=`</div>`;}}container.innerHTML=html;}

function openSensorModal(typeKey){activeModalType=typeKey;const modal=document.getElementById('sensorModal');if(!modal){return;}refreshSensorModalContent(typeKey);const detailList=sensorDetailCache[typeKey]||[];const sensorDetails=detailList.find(sensor=>((sensor.type||'').trim().toUpperCase())===typeKey)||detailList[0];if(sensorDetails){startSensorFocus(sensorDetails).catch(err=>console.warn('Failed to enter focus mode',err));}modal.classList.remove('hidden');}

function closeSensorModal(){const modal=document.getElementById('sensorModal');if(!modal)return;activeModalType=null;modal.classList.add('hidden');stopSensorFocus();}

function refreshSensorModalContent(typeKey){const modal=document.getElementById('sensorModal');if(!modal||activeModalType!==typeKey){return;}const snapshot=latestSensorSnapshots[typeKey];console.log('Modal refresh for',typeKey,'snapshot:',snapshot);const cfg=(snapshot&&snapshot.config)||{icon:'📊',label:snapshot?.rawType||typeKey,unit:'',color:'text-gray-500'};const title=document.getElementById('sensorModalTitle');if(title){title.innerHTML=`<span class='text-2xl mr-2'>${cfg.icon||'📊'}</span><span>${cfg.label||snapshot?.rawType||typeKey}</span>`;}const currentValue=document.getElementById('sensorModalValue');if(currentValue){currentValue.innerHTML=renderModalValue(snapshot?.value,cfg,snapshot?.timestamp);}const detailList=sensorDetailCache[typeKey]||[];const sensorDetails=detailList.find(sensor=>((sensor.type||'').trim().toUpperCase())===typeKey)||detailList[0];const calibrationBlock=document.getElementById('sensorModalCalibration');if(calibrationBlock){calibrationBlock.innerHTML=renderModalCalibrationSection(typeKey,sensorDetails,snapshot?.timestamp);}}

function recordSensorHistory(typeKey,sample,timestamp){if(!typeKey||sample==null)return;if(!sensorValueHistory[typeKey]){sensorValueHistory[typeKey]=[];}const entry={timestamp:timestamp||new Date().toLocaleTimeString(),display:formatHistoryValue(sample)};sensorValueHistory[typeKey].unshift(entry);sensorValueHistory[typeKey]=sensorValueHistory[typeKey].slice(0,15);} 

function formatHistoryValue(sample){if(typeof sample==='number'){return sample.toFixed(2);}if(Array.isArray(sample)){return sample.map(val=>typeof val==='number'?val.toFixed(2):val).join(', ');}if(sample&&typeof sample==='object'){return Object.keys(sample).map(key=>{const val=sample[key];return `${key}: ${typeof val==='number'?val.toFixed(2):val}`;}).join(', ');}return sample??'—';}

function initSensorSocket(){const scheme=window.location.protocol==='https:'?'wss':'ws';try{sensorSocket=new WebSocket(`${scheme}://${window.location.host}${SENSOR_WS_PATH}`);}catch(err){console.warn('Failed to open sensor socket',err);scheduleSensorSocketReconnect();return;}sensorSocket.addEventListener('open',async()=>{sensorSocketReady=true;if(sensorSocketReconnectTimer){clearTimeout(sensorSocketReconnectTimer);sensorSocketReconnectTimer=null;}await stopHttpFocusFallback();sendSensorSocketMessage({action:'request_snapshot'});if(focusModeActive&&focusSensorAddress!=null){focusUsingWebSocket=true;sendFocusCommand('focus_start',focusSensorAddress);}});sensorSocket.addEventListener('message',handleSensorSocketMessage);sensorSocket.addEventListener('close',()=>{sensorSocketReady=false;sensorSocket=null;if(focusModeActive&&focusSensorAddress!=null){focusUsingWebSocket=false;startHttpFocusFallback(focusSensorAddress);}scheduleSensorSocketReconnect();});sensorSocket.addEventListener('error',()=>{sensorSocket?.close();});}

function scheduleSensorSocketReconnect(){if(sensorSocketReconnectTimer){return;}sensorSocketReconnectTimer=setTimeout(()=>{sensorSocketReconnectTimer=null;initSensorSocket();},3000);}

function sendSensorSocketMessage(payload){if(!sensorSocketReady||!sensorSocket)return;try{sensorSocket.send(JSON.stringify(payload));}catch(err){console.warn('Sensor socket send failed',err);}}

function handleSensorSocketMessage(event){let message=null;try{message=JSON.parse(event.data);}catch(err){console.warn('Invalid WS payload',err);return;}if(message?.type==='status_snapshot'&&message.sensors){displaySensorValues(message.sensors);if(typeof message.rssi==='number'){const rssiEl=document.getElementById('wifi-rssi');if(rssiEl)rssiEl.textContent=`${message.rssi} dBm`;}}else if(message?.type==='focus_sample'){ingestFocusedSample(message);}else if(message?.type==='focus_status'){if(message.status==='stopped'){focusUsingWebSocket=false;}}}

function sendFocusCommand(action,address){if(!action)return;const payload={action};if(typeof address==='number')payload.address=address;sendSensorSocketMessage(payload);}

async function startHttpFocusFallback(target){try{const resp=await fetch('/api/sensors/pause',{method:'POST'});if(resp.ok){focusPauseIssued=true;}}catch(err){console.warn('HTTP pause failed',err);}await pollFocusedSensorHttp();focusSensorTimer=setInterval(pollFocusedSensorHttp,2000);}

async function stopHttpFocusFallback(){if(focusSensorTimer){clearInterval(focusSensorTimer);focusSensorTimer=null;}if(focusPauseIssued){try{const resp=await fetch('/api/sensors/resume',{method:'POST'});if(!resp.ok){console.warn('Resume request returned HTTP error');}}catch(err){console.warn('Resume request failed',err);}focusPauseIssued=false;}}

async function startSensorFocus(sensor){if(!sensor||sensor.address===undefined)return;const target=Number(sensor.address);if(focusModeActive&&focusSensorAddress===target&&focusUsingWebSocket&&sensorSocketReady){return;}await stopSensorFocus();focusModeActive=true;focusSensorAddress=target;if(sensorSocketReady){focusUsingWebSocket=true;sendFocusCommand('focus_start',target);}else{focusUsingWebSocket=false;await startHttpFocusFallback(target);}}

async function stopSensorFocus(){const wasFocused=focusModeActive||focusPauseIssued||focusSensorTimer;if(focusUsingWebSocket&&sensorSocketReady){sendFocusCommand('focus_stop');}await stopHttpFocusFallback();focusModeActive=false;focusSensorAddress=null;focusUsingWebSocket=false;if(wasFocused){await safeLoadStatus();}}

async function pollFocusedSensorHttp(){if(!focusModeActive||focusSensorAddress==null)return;try{const res=await fetch(`/api/sensors/sample/${focusSensorAddress}`,{signal:AbortSignal.timeout(5000)});if(!res.ok)throw new Error('Focused sensor read failed');const payload=await res.json();ingestFocusedSample(payload);}catch(err){console.warn('Focused sensor poll error',err);}}

function ingestFocusedSample(payload){const sensor=payload?.sensor;if(!sensor)return;const typeKey=(sensor.type||'').trim().toUpperCase();if(!typeKey)return;const timestampMs=sensor.timestamp_ms||Date.now();const timestamp=new Date(timestampMs).toLocaleTimeString();const reading=('reading'in sensor)?sensor.reading:(Array.isArray(sensor.raw)&&sensor.raw.length? (sensor.raw.length===1?sensor.raw[0]:sensor.raw):null);const cfg=sensorConfig[sensor.type]||sensorConfig[typeKey]||{icon:'📊',label:sensor.type||typeKey,unit:'',color:'text-gray-500'};latestSensorSnapshots[typeKey]={rawType:sensor.type||typeKey,value:reading,config:cfg,timestamp};recordSensorHistory(typeKey,reading,timestamp);const detailList=sensorDetailCache[typeKey]||[];const existingIndex=detailList.findIndex(item=>item?.address===sensor.address);if(existingIndex>=0){detailList[existingIndex]=sensor;}else{detailList.unshift(sensor);sensorDetailCache[typeKey]=detailList;}if(activeModalType===typeKey){refreshSensorModalContent(typeKey);}}

function renderModalValue(value,cfg,timestamp){const accent=cfg.color||'text-gray-600';if(value==null)return `<p class='text-sm text-gray-500 dark:text-gray-300'>No data available for this sensor.</p>`;const updatedLabel=timestamp?`Updated ${timestamp}`:'Awaiting new data';if(typeof value==='number'){return `<div class='text-center'><div class='text-4xl font-bold ${accent}'>${value.toFixed(2)} ${cfg.unit||''}</div><p class='text-xs text-gray-500 dark:text-gray-400 mt-1'>${updatedLabel}</p></div>`;}if(typeof value==='object'){const rows=Object.keys(value).map(key=>{const label=key.replace('_',' ').replace(/\b\w/g,l=>l.toUpperCase());const val=value[key];const formatted=typeof val==='number'?val.toFixed(2):val;return `<div class='flex items-center justify-between py-1 border-b border-gray-100 dark:border-gray-700 last:border-0'><span class='text-sm text-gray-500 dark:text-gray-300'>${label}</span><span class='font-semibold ${accent}'>${formatted}</span></div>`;}).join('');return `<div class='space-y-1'>${rows}<p class='text-xs text-gray-500 dark:text-gray-400 mt-2 text-right'>${updatedLabel}</p></div>`;}return `<p class='text-sm text-gray-500 dark:text-gray-300'>${value}</p>`;}

function renderModalCalibrationSection(typeKey,sensorDetails,timestamp){const history=sensorValueHistory[typeKey]||[];if(!sensorDetails){return `<div class='mt-4 p-4 border border-gray-200 dark:border-gray-700 rounded-lg'><p class='text-sm text-gray-600 dark:text-gray-300'>Calibration hints become available once this sensor reports through the Sensors tab.</p>${renderHistoryList(history,false)}</div>`;}const supportsCalibration=sensorSupportsCalibration(sensorDetails,typeKey);const statusLabel=sensorDetails.calibration_status||'Unknown';if(!supportsCalibration){return `<div class='mt-4 p-4 border border-gray-200 dark:border-gray-700 rounded-lg'><p class='text-sm text-gray-600 dark:text-gray-300'>This sensor does not expose calibration controls.</p>${renderHistoryList(history,false)}</div>`;}const controls=renderCalibrationControls(sensorDetails);return `<div class='mt-4 border border-gray-200 dark:border-gray-700 rounded-lg bg-gray-50 dark:bg-gray-800/60 p-4 space-y-4'><div class='flex items-center justify-between'><div><p class='text-xs uppercase tracking-wide text-gray-500 dark:text-gray-400'>Calibration Status</p><p class='text-lg font-semibold text-gray-900 dark:text-white'>${statusLabel}</p></div><span class='text-xs text-gray-500 dark:text-gray-400'>${timestamp?`Live as of ${timestamp}`:'Watching for next reading'}</span></div><div>${controls||'<p class="text-sm text-gray-500 dark:text-gray-300">Calibration commands for this sensor are not yet available.</p>'}</div><p class='text-xs text-gray-500 dark:text-gray-400'>Keep this modal open; repeated identical values (e.g., 7.00, 7.00, 7.00) indicate the probe is stable and ready for calibration.</p>${renderHistoryList(history,true)}</div>`;}

function renderCalibrationControls(sensor){const typeKey=(sensor?.type||'').trim().toUpperCase();if(typeKey==='PH')return renderPhCalibrationSection(sensor,'modal');if(typeKey==='ORP')return renderOrpCalibrationSection(sensor,'modal');if(typeKey==='RTD')return renderRtdCalibrationSection(sensor,'modal');if(typeKey==='EC')return renderEcCalibrationSection(sensor,'modal');if(typeKey==='DO')return renderDoCalibrationSection(sensor,'modal');return '';} 

function renderHistoryList(history,collapsible=false){const items=history.length?history.map(entry=>`<li class='flex items-center justify-between py-1 border-b border-dotted border-gray-200 dark:border-gray-700 last:border-0'><span class='text-xs text-gray-500 dark:text-gray-400'>${entry.timestamp}</span><span class='text-sm font-mono text-gray-900 dark:text-gray-100 truncate ml-3'>${entry.display}</span></li>`).join(''):`<li class='text-xs text-gray-500 dark:text-gray-400'>Waiting for readings...</li>`;if(!collapsible){return `<div class='mt-3'><p class='text-xs text-gray-500 dark:text-gray-400 mb-1'>Live values</p><ul class='text-sm text-gray-700 dark:text-gray-200 space-y-1'>${items}</ul></div>`;}return `<details open class='border border-dashed border-gray-300 dark:border-gray-600 rounded-lg overflow-hidden'>`+
`<summary class='px-4 py-2 flex items-center justify-between cursor-pointer text-sm font-medium text-gray-700 dark:text-gray-200'>`+
`<span>Live Value Stream</span><i class='fas fa-chevron-down text-xs'></i></summary>`+
`<div class='px-4 py-3 bg-white dark:bg-gray-900/70 max-h-48 overflow-y-auto'><ul class='space-y-1'>${items}</ul><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>When this list shows the same value multiple times, the sensor is stable enough to calibrate.</p></div>`+
`</details>`;}

function sensorSupportsCalibration(sensor,typeKey){const caps=Array.isArray(sensor?.capabilities)?sensor.capabilities:[];const inferredTypes=['PH','ORP','RTD','EC','DO'];return caps.includes('calibration')||inferredTypes.includes((sensor?.type||typeKey||'').trim().toUpperCase());}
async function loadStatus(){try{const res=await fetch('/api/status');if(!res.ok)throw new Error('Failed to load');const d=await res.json();document.getElementById('device-id').textContent=d.device_id;document.getElementById('wifi-ssid').textContent=d.wifi_ssid;document.getElementById('ip-addr').textContent=d.ip_address;const upMin=Math.floor(d.uptime/60),upHr=Math.floor(upMin/60);document.getElementById('uptime').textContent=upHr>0?`${upHr}h ${upMin%60}m`:`${upMin}m`;document.getElementById('current-time').textContent=d.current_time;const heapKB=(d.free_heap/1024).toFixed(1);document.getElementById('free-heap').textContent=heapKB+' KB';if(typeof d.rssi==='number'){document.getElementById('wifi-rssi').textContent=d.rssi+' dBm';}if(typeof d.cpu_usage==='number'){document.getElementById('cpu-usage').textContent=d.cpu_usage+'%';}if(d.sensors&&!sensorSocketReady){displaySensorValues(d.sensors);}document.getElementById('status-dot').className='bg-green-500 w-3 h-3 rounded-full status-dot';document.getElementById('status-text').textContent='Device Online';}catch(e){console.error(e);document.getElementById('status-dot').className='bg-red-500 w-3 h-3 rounded-full';document.getElementById('status-text').textContent='Device Offline';}}
async function testMQTT(){alert('Testing MQTT connection...');try{await fetch('/api/test-mqtt',{method:'POST'});alert('MQTT test complete');}catch(e){alert('Test failed');}}
async function rebootDevice(){if(!confirm('Reboot device now?'))return;await fetch('/api/reboot',{method:'POST'});alert('Device rebooting...');setTimeout(()=>location.reload(),10000);}
async function clearWiFi(){if(!confirm('Clear WiFi and reset device?'))return;await fetch('/api/clear-wifi',{method:'POST'});alert('WiFi cleared. Restarting...');setTimeout(()=>location.reload(),10000);}
async function uploadFirmware(){const input=document.getElementById('firmwareFile');const status=document.getElementById('firmwareStatus');if(!input){alert('Uploader not available');return;}if(!input.files||input.files.length===0){alert('Select a firmware .bin file first.');return;}const file=input.files[0];if(!file.name.toLowerCase().endsWith('.bin')){if(!confirm('File does not have a .bin extension. Upload anyway?'))return;}if(!confirm(`Upload ${file.name}? The device will reboot when the transfer completes.`))return;status.textContent='Uploading firmware...';try{const res=await fetch('/api/firmware/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});const payload=await res.text();if(!res.ok)throw new Error(payload||'Upload failed');let message='Firmware uploaded. Device rebooting...';try{const data=JSON.parse(payload);if(data.status)message=`Firmware ${data.status}. Device rebooting...`; }catch(e){}status.textContent=message;alert(message);setTimeout(()=>location.reload(),15000);}catch(err){console.error('Firmware upload failed',err);status.textContent=`Upload failed: ${err.message}`;alert(`Firmware upload failed: ${err.message}`);}}
async function saveSetting(){const interval=document.getElementById('mqtt-interval').value;await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mqtt_interval:parseInt(interval)})});alert('Settings saved!');}
async function loadSensors(){try{const res=await fetch('/api/sensors',{signal:AbortSignal.timeout(10000)});if(!res.ok)throw new Error('Failed to load sensors');const d=await res.json();updateSensorDetailCache(Array.isArray(d.sensors)?d.sensors:[]);const list=document.getElementById('sensor-list');if(!list)return;if(!d.count){list.innerHTML='<p class="text-gray-600 dark:text-gray-400">No sensors detected</p>';return;}list.innerHTML=d.sensors.map(renderSensorCard).join('');}catch(e){console.error('Failed to load sensors:',e);const list=document.getElementById('sensor-list');if(list)list.innerHTML=`<p class='text-red-500'>Failed to load sensors: ${e.message}</p>`;}}

function updateSensorDetailCache(items){const nextCache={};items.forEach(sensor=>{const key=(sensor?.type||'').trim().toUpperCase();if(!key)return;if(!nextCache[key]){nextCache[key]=[];}nextCache[key].push(sensor);});Object.keys(sensorDetailCache).forEach(key=>{if(!nextCache[key]){delete sensorDetailCache[key];}});Object.keys(nextCache).forEach(key=>{sensorDetailCache[key]=nextCache[key];});}

function renderSensorCard(sensor){const caps=Array.isArray(sensor.capabilities)?sensor.capabilities:[];const addrHex=formatAddress(sensor.address);const type=(sensor.type||'').trim();const typeKey=type.toUpperCase();const isPh=typeKey==='PH';const isOrp=typeKey==='ORP';const isRtd=typeKey==='RTD';const isEc=typeKey==='EC';const isDo=typeKey==='DO';const supportsCalibration=caps.includes('calibration')||isPh||isOrp||isRtd||isEc||isDo;const supportsTempComp=(caps.includes('temp_comp')&&isPh)||isPh;let html=`<div class='bg-gray-50 dark:bg-gray-700 p-4 rounded-lg mb-4' id='sensor-card-${sensor.address}' data-sensor-address='${sensor.address}' data-sensor-type='${type}'>`;html+=`<h3 class='text-lg font-bold text-green-600 dark:text-green-400 mb-2'>${type||'Sensor'} @ 0x${addrHex}</h3>`;if(sensor.firmware)html+=`<p class='text-sm text-gray-600 dark:text-gray-400 mb-3'>Firmware: ${sensor.firmware}</p>`;if(type!=='MAX17048'){html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Name:</label><input class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' id='name-${sensor.address}' value='${sensor.name||''}' placeholder='Pool_pH' maxlength='16' pattern='[A-Za-z0-9_]+'><p class='text-xs text-gray-500 dark:text-gray-400 mt-1'>1-16 characters: letters, numbers, underscore only</p></div>`;html+=`<div class='mb-3 flex gap-4 flex-wrap'><label class='flex items-center text-gray-700 dark:text-gray-300'><input type='checkbox' class='mr-2' id='led-${sensor.address}' ${sensor.led?'checked':''}> LED On</label><label class='flex items-center text-gray-700 dark:text-gray-300'><input type='checkbox' class='mr-2' id='plock-${sensor.address}' ${sensor.plock?'checked':''}> Protocol Lock</label></div>`;if(isRtd){html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Scale:</label><select class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' id='scale-${sensor.address}'><option ${sensor.scale==='C'?'selected':''}>C</option><option ${sensor.scale==='F'?'selected':''}>F</option><option ${sensor.scale==='K'?'selected':''}>K</option></select></div>`;}if(isPh){html+=`<div class='mb-3'><label class='flex items-center text-gray-700 dark:text-gray-300'><input type='checkbox' class='mr-2' id='extscale-${sensor.address}' ${sensor.extended_scale?'checked':''}> Extended pH Scale</label></div>`;}if(isEc){html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Probe K Value:</label><input class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' type='number' step='0.1' id='probe-${sensor.address}' value='${sensor.probe_type??1.0}'></div>`;html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>TDS Factor:</label><input class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' type='number' step='0.01' id='tds-${sensor.address}' value='${sensor.tds_factor??0.5}'></div>`;}html+=`<button class='bg-green-600 dark:bg-green-400 hover:bg-green-700 dark:hover:bg-green-500 text-white px-4 py-2 rounded-md mt-2' onclick='saveSensorConfig(${sensor.address})'><i class='fas fa-save'></i> Save ${type||'Sensor'} Settings</button>`;}if(caps.length)html+=renderCapabilityBadges(caps);if(supportsCalibration){html+=`<div class='mt-4'><button class='w-full bg-gradient-to-r from-green-600 to-green-500 hover:from-green-700 hover:to-green-600 text-white px-6 py-3 rounded-lg font-semibold shadow-lg hover:shadow-xl transition-all flex items-center justify-center gap-2' onclick='openCalibrationWizard({address:${sensor.address},type:"${type}",name:"${sensor.name||''}"});'><i class='fas fa-magic'></i> Start Calibration Wizard</button></div>`;if(isPh)html+=renderPhCalibrationSection(sensor);else if(isOrp)html+=renderOrpCalibrationSection(sensor);else if(isRtd)html+=renderRtdCalibrationSection(sensor);else if(isEc)html+=renderEcCalibrationSection(sensor);else if(isDo)html+=renderDoCalibrationSection(sensor);}if(supportsTempComp)html+=renderPhTempCompSection(sensor);if(caps.includes('mode'))html+=renderModeSection(sensor);if(caps.includes('sleep'))html+=renderSleepSection(sensor);html+=`</div>`;return html;}

function formatAddress(address){return Number(address).toString(16).toUpperCase().padStart(2,'0');}

function renderCapabilityBadges(caps){const map={calibration:'Calibration',temp_comp:'Temp Compensation',mode:'Measurement Mode',sleep:'Power State',offset:'Offset'};return `<div class='flex flex-wrap gap-2 mt-3'>${caps.map(cap=>`<span class='px-2 py-1 text-xs rounded-full bg-green-100 text-green-700 dark:bg-green-900 dark:text-green-200'>${map[cap]||cap}</span>`).join('')}</div>`;}

function renderPhCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';const points=[{key:'mid',label:'Mid (7.00)'},{key:'low',label:'Low (4.00)'},{key:'high',label:'High (10.00)'}];return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-4 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-4'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>pH Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><div class='space-y-3'>${points.map(p=>`<div class='flex items-center gap-3'><label class='text-xs font-medium text-gray-600 dark:text-gray-300 w-24 flex-shrink-0'>${p.label}</label><input id='${idPrefix}cal-${p.key}-${sensor.address}' type='number' step='0.01' value='${p.key==='mid'?7.00:p.key==='low'?4.00:10.00}' class='w-20 px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white text-center'><button class='px-4 py-2 text-xs font-medium rounded-md bg-green-600 hover:bg-green-700 text-white transition-colors' onclick='calibratePhSensor(${sensor.address},"${p.key}")'>Calibrate</button></div>`).join('')}</div><button class='mt-4 w-full px-4 py-2 text-sm font-medium rounded-md bg-red-600 hover:bg-red-700 text-white transition-colors' onclick='calibratePhSensor(${sensor.address},"clear")'>Clear Calibration</button></div>`;}

function renderOrpCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>ORP Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><label class='block text-xs font-medium text-gray-600 dark:text-gray-300 mb-1'>Solution mV</label><div class='flex flex-col gap-2 md:flex-row md:items-center'><input id='${idPrefix}orp-cal-${sensor.address}' type='number' step='1' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white' placeholder='e.g. 470'><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateOrpSensor(${sensor.address})'>Calibrate</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='clearOrpCalibration(${sensor.address})'>Clear</button></div></div></div>`;}

function renderRtdCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';const tempValue=Number.isFinite(sensor.last_rtd_cal)?sensor.last_rtd_cal:(sensor.temperature_comp??25);return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>RTD Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><label class='block text-xs font-medium text-gray-600 dark:text-gray-300 mb-1'>Reference Temperature (°C)</label><div class='flex flex-col gap-2 md:flex-row md:items-center'><input id='${idPrefix}rtd-cal-${sensor.address}' type='number' step='0.1' value='${Number(tempValue||25).toFixed(2)}' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white'><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateRtdSensor(${sensor.address})'>Calibrate</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='calibrateRtdSensor(${sensor.address},true)'>Clear</button></div></div><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Place the probe in a trusted reference solution before calibrating.</p></div>`;}

function renderEcCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>EC Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><label class='block text-xs font-medium text-gray-600 dark:text-gray-300 mb-1'>Solution Conductivity (µS/cm)</label><input id='${idPrefix}ec-cal-value-${sensor.address}' type='number' step='1' class='w-full px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white mb-3' placeholder='e.g. 12880'><div class='flex flex-wrap gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-indigo-600 text-white' onclick='calibrateEcSensor(${sensor.address},"dry")'>Dry</button><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateEcSensor(${sensor.address},"low")'>Low</button><button class='px-3 py-1.5 text-xs rounded-md bg-blue-600 text-white' onclick='calibrateEcSensor(${sensor.address},"high")'>High</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='calibrateEcSensor(${sensor.address},"clear")'>Clear</button></div><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Low/high points require a conductivity value in µS/cm.</p></div>`;}

function renderDoCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>DO Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><p class='text-xs text-gray-500 dark:text-gray-400 mb-2'>Use atmospheric (air) or zero-oxygen solution for calibration.</p><div class='flex flex-wrap gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateDoSensor(${sensor.address},"atm")'>Atmospheric</button><button class='px-3 py-1.5 text-xs rounded-md bg-blue-600 text-white' onclick='calibrateDoSensor(${sensor.address},"0")'>Zero</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='calibrateDoSensor(${sensor.address},"clear")'>Clear</button></div></div>`;}

function renderPhTempCompSection(sensor){const tempValue=Number.isFinite(sensor.temperature_comp)?sensor.temperature_comp:(sensor.temperature_comp??sensor.temp_compensation??25);return `<div class='mt-4 border border-dashed border-gray-300 dark:border-gray-600 rounded-lg p-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white mb-2'>Temperature Compensation</h4><div class='flex flex-col gap-2 md:flex-row md:items-center'><input id='ph-temp-${sensor.address}' type='number' step='0.1' value='${Number(tempValue||25).toFixed(2)}' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white' placeholder='°C'><button class='px-3 py-1.5 text-xs rounded-md bg-blue-600 text-white' onclick='setPhTempComp(${sensor.address})'>Apply</button></div></div>`;}

function renderModeSection(sensor){const active=!!sensor.continuous_mode;return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3'><div class='flex items-center justify-between mb-2'><div><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>Measurement Mode</h4><p class='text-xs text-gray-500 dark:text-gray-300'>Currently ${active?'Continuous stream (C command)':'Single-shot (R command)'}</p></div><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md ${active?'bg-gray-400 text-white cursor-not-allowed':'bg-green-600 text-white'}' ${active?'disabled':''} onclick='setSensorMode(${sensor.address},true)'>Continuous</button><button class='px-3 py-1.5 text-xs rounded-md ${!active?'bg-gray-400 text-white cursor-not-allowed':'bg-gray-200 dark:bg-gray-700 dark:text-white'}' ${!active?'disabled':''} onclick='setSensorMode(${sensor.address},false)'>Single</button></div></div></div>`;}

function renderSleepSection(sensor){const sleeping=!!sensor.sleeping;return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-yellow-50 dark:bg-yellow-900/40'><div class='flex items-center justify-between'><div><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>Power State</h4><p class='text-xs text-gray-600 dark:text-gray-300'>${sleeping?'Sensor is in Sleep mode':'Sensor is awake'}</p></div><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-yellow-600 text-white ${sleeping?'cursor-not-allowed opacity-70':''}' ${sleeping?'disabled':''} onclick='setSensorSleep(${sensor.address},true)'>Sleep</button><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white ${sleeping?'':'cursor-not-allowed opacity-70'}' ${sleeping?'':'disabled'} onclick='setSensorSleep(${sensor.address},false)'>Wake</button></div></div></div>`;}
async function rescanSensors(){alert('Rescanning I2C bus...');await fetch('/api/sensors/rescan',{method:'POST'});await loadSensors();alert('Rescan complete!');}
async function pauseSensors(){try{await fetch('/api/sensors/pause',{method:'POST'});alert('Sensor readings paused');}catch(e){alert('Failed to pause sensors');}}
async function resumeSensors(){try{await fetch('/api/sensors/resume',{method:'POST'});alert('Sensor readings resumed');}catch(e){alert('Failed to resume sensors');}}
async function saveSensorConfig(addr){const cfg={address:addr};const name=document.getElementById(`name-${addr}`)?.value;if(name){const trimmed=name.trim();if(trimmed.length>0){if(trimmed.length>16){alert('Sensor name must be 16 characters or less');return;}if(!/^[A-Za-z0-9_]+$/.test(trimmed)){alert('Sensor name can only contain letters, numbers, and underscores (no spaces or special characters)');return;}cfg.name=trimmed;}}const led=document.getElementById(`led-${addr}`)?.checked;if(led!==undefined)cfg.led=led;const plock=document.getElementById(`plock-${addr}`)?.checked;if(plock!==undefined)cfg.plock=plock;const scale=document.getElementById(`scale-${addr}`)?.value;if(scale)cfg.scale=scale;const extscale=document.getElementById(`extscale-${addr}`)?.checked;if(extscale!==undefined)cfg.extended_scale=extscale;const probe=document.getElementById(`probe-${addr}`)?.value;if(probe)cfg.probe_type=parseFloat(probe);const tds=document.getElementById(`tds-${addr}`)?.value;if(tds)cfg.tds_factor=parseFloat(tds);try{const res=await fetch('/api/sensors/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});if(!res.ok){const err=await res.text();alert('Failed to save: '+err);return;}alert('Sensor configuration saved!');await loadSensors();}catch(e){alert('Save failed: '+e.message);}}
async function saveAllSensors(){try{const sensorCards=document.querySelectorAll('[data-sensor-address]');if(sensorCards.length===0){alert('No sensors found on page. Please load the sensors tab first.');return;}const batchConfig={sensors:[]};for(const card of sensorCards){const addr=parseInt(card.getAttribute('data-sensor-address'));if(isNaN(addr))continue;const sensorType=card.getAttribute('data-sensor-type');if(sensorType==='MAX17048')continue;const cfg={address:addr};const nameInput=document.getElementById(`name-${addr}`);if(nameInput&&nameInput.value){const trimmed=nameInput.value.trim();if(trimmed.length>0){if(trimmed.length>16){alert(`Sensor at address 0x${addr.toString(16)}: Name must be 16 characters or less`);return;}if(!/^[A-Za-z0-9_]+$/.test(trimmed)){alert(`Sensor at address 0x${addr.toString(16)}: Name can only contain letters, numbers, and underscores`);return;}cfg.name=trimmed;}}const ledCheckbox=document.getElementById(`led-${addr}`);if(ledCheckbox!==null){cfg.led=ledCheckbox.checked;}const plockCheckbox=document.getElementById(`plock-${addr}`);if(plockCheckbox!==null){cfg.plock=plockCheckbox.checked;}const scaleSelect=document.getElementById(`scale-${addr}`);if(scaleSelect&&scaleSelect.value){cfg.scale=scaleSelect.value;}const extscaleCheckbox=document.getElementById(`extscale-${addr}`);if(extscaleCheckbox!==null){cfg.extended_scale=extscaleCheckbox.checked;}const probeSelect=document.getElementById(`probe-${addr}`);if(probeSelect&&probeSelect.value){cfg.probe_type=parseFloat(probeSelect.value);}const tdsInput=document.getElementById(`tds-${addr}`);if(tdsInput&&tdsInput.value){cfg.tds_factor=parseFloat(tdsInput.value);}if(Object.keys(cfg).length>1){batchConfig.sensors.push(cfg);}}if(batchConfig.sensors.length===0){alert('No sensor configurations to save');return;}if(!confirm(`Save configuration for ${batchConfig.sensors.length} sensor(s)?`))return;const saveRes=await fetch('/api/sensors/config/batch',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(batchConfig)});if(!saveRes.ok){const err=await saveRes.text();alert('Batch save failed: '+err);return;}const result=await saveRes.json();let message=`✓ Updated: ${result.updated}\n✗ Failed: ${result.failed}`;if(result.failed>0){message+='\n\nFailed sensors:\n';result.results.forEach(r=>{if(r.status==='error'){message+=`- Address 0x${r.address.toString(16)}: ${r.error}\n`;}});}alert(message);}catch(e){alert('Batch save failed: '+e.message);}}
async function sensorActionRequest(url,payload,successMessage){try{const res=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const text=await res.text();if(!res.ok)throw new Error(text||'Request failed');if(successMessage)alert(successMessage);await loadSensors();return true;}catch(err){console.error('Sensor action failed:',err);alert(err.message||'Sensor action failed');return false;}}
async function calibratePhSensor(address,point){const addrHex=formatAddress(address);if(point!=='clear'){const input=document.getElementById(`cal-${point}-${address}`)||document.getElementById(`modal-cal-${point}-${address}`);const value=parseFloat(input?.value||'');if(Number.isNaN(value)){alert('Enter a valid calibration value.');return;}if(!confirm(`Calibrate 0x${addrHex.toUpperCase()} (${point.toUpperCase()})?`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{point,value},'Calibration command sent.');}else{if(!confirm(`Clear calibration for sensor 0x${addrHex}?`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{point:'clear'},'Calibration cleared.');}}
async function calibrateOrpSensor(address){const addrHex=formatAddress(address);const input=document.getElementById(`orp-cal-${address}`)||document.getElementById(`modal-orp-cal-${address}`);const value=parseFloat(input?.value||'');if(Number.isNaN(value)){alert('Enter a valid mV value.');return;}if(!confirm(`Calibrate ORP sensor 0x${addrHex} at ${value.toFixed(0)} mV?`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{value},'ORP calibration command sent.');}
async function clearOrpCalibration(address){const addrHex=formatAddress(address);if(!confirm(`Clear ORP calibration for sensor 0x${addrHex}?`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{point:'clear'},'ORP calibration cleared.');}
async function calibrateRtdSensor(address,clear=false){const addrHex=formatAddress(address);if(clear){if(!confirm(`Clear RTD calibration for sensor 0x${addrHex}?`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{point:'clear'},'RTD calibration cleared.');return;}const input=document.getElementById(`rtd-cal-${address}`)||document.getElementById(`modal-rtd-cal-${address}`);const value=parseFloat(input?.value||'');if(Number.isNaN(value)){alert('Enter a valid reference temperature.');return;}if(!confirm(`Calibrate RTD 0x${addrHex} at ${value.toFixed(2)} °C?`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{temperature:value},'RTD calibration command sent.');}
async function calibrateEcSensor(address,point){const addrHex=formatAddress(address);const payload={point};if(point==='low'||point==='high'){const input=document.getElementById(`ec-cal-value-${address}`)||document.getElementById(`modal-ec-cal-value-${address}`);const value=parseFloat(input?.value||'');if(Number.isNaN(value)||value<=0){alert('Enter a valid solution conductivity in µS/cm.');return;}payload.value=Math.round(value);}let actionLabel='Calibration command sent.';if(point==='dry')actionLabel='EC dry calibration command sent.';else if(point==='low')actionLabel='EC low-point calibration sent.';else if(point==='high')actionLabel='EC high-point calibration sent.';else if(point==='clear')actionLabel='EC calibration cleared.';if(point==='clear'){if(!confirm(`Clear EC calibration for sensor 0x${addrHex}?`))return;}else{if(!confirm(`Send EC ${point.toUpperCase()} calibration to 0x${addrHex}?`))return;}await sensorActionRequest(`/api/sensors/calibrate/${address}`,payload,actionLabel);}
async function calibrateDoSensor(address,point){const addrHex=formatAddress(address);let message='Send DO calibration command?';if(point==='atm')message='Run atmospheric DO calibration?';else if(point==='0')message='Run zero-oxygen DO calibration?';else if(point==='clear')message='Clear DO calibration?';if(!confirm(`${message} (Sensor 0x${addrHex})`))return;await sensorActionRequest(`/api/sensors/calibrate/${address}`,{point},'DO calibration command sent.');}
async function setPhTempComp(address){const input=document.getElementById(`ph-temp-${address}`);const value=parseFloat(input?.value||'');if(Number.isNaN(value)){alert('Enter a valid temperature value.');return;}await sensorActionRequest(`/api/sensors/compensate/${address}`,{temp_c:value},'Compensation temperature updated.');}
async function setSensorMode(address,enable){await sensorActionRequest(`/api/sensors/mode/${address}`,{continuous:!!enable},enable?'Continuous mode enabled.':'Switched to single-shot mode.');}
async function setSensorSleep(address,sleep){const addrHex=formatAddress(address);const action=sleep?'put to sleep':'wake';if(!confirm(`Do you want to ${action} sensor 0x${addrHex}?`))return;await sensorActionRequest(`/api/sensors/power/${address}`,{sleep:!!sleep},sleep?'Sleep command sent.':'Wake command sent.');}
function showTab(n){document.querySelectorAll('.tab').forEach((t,i)=>{if(i===n){t.className='px-6 py-3 text-green-600 dark:text-green-400 border-b-2 border-green-600 dark:border-green-400 font-semibold tab active';}else{t.className='px-6 py-3 text-gray-600 dark:text-gray-400 border-b-2 border-transparent hover:text-gray-900 dark:hover:text-white tab';}});document.querySelectorAll('.tab-content').forEach((c,i)=>{c.style.display=(i===n)?'block':'none';});if(n===1)loadSensors();if(n===2){loadSettings();loadDeviceInfo();}}
async function loadDeviceInfo(){try{const res=await fetch('/api/device/info');if(!res.ok)throw new Error('Failed to load device info');const data=await res.json();document.getElementById('device-id').value=data.device_id||'';document.getElementById('device-name').value=data.device_name||'';}catch(e){console.error('Failed to load device info:',e);}}
async function saveDeviceName(){try{const deviceName=document.getElementById('device-name').value.trim();if(deviceName&&deviceName.length>64){alert('Device name must be 64 characters or less');return;}const res=await fetch('/api/device/name',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_name:deviceName})});if(!res.ok){const text=await res.text();throw new Error(text||'Failed to save device name');}const data=await res.json();document.getElementById('device-name').value=data.device_name||'';alert('Device name saved successfully!');}catch(e){console.error('Failed to save device name:',e);alert('Error: '+e.message);}}
async function clearDeviceName(){try{const res=await fetch('/api/device/name',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_name:''})});if(!res.ok)throw new Error('Failed to clear device name');document.getElementById('device-name').value='';alert('Device name cleared!');}catch(e){console.error('Failed to clear device name:',e);alert('Error: '+e.message);}}
async function loadSettings(){try{const res=await fetch('/api/settings');if(!res.ok)throw new Error('Failed to load settings');const data=await res.json();document.getElementById('mqtt-interval').value=data.mqtt_interval!==undefined?data.mqtt_interval:10;document.getElementById('sensor-interval').value=data.sensor_interval!==undefined?data.sensor_interval:10;}catch(e){console.error('Failed to load settings:',e);}}
async function saveSettings(){try{const mqttInterval=parseInt(document.getElementById('mqtt-interval').value);const sensorInterval=parseInt(document.getElementById('sensor-interval').value);if(isNaN(mqttInterval)||mqttInterval<0){alert('MQTT interval must be >= 0');return;}if(isNaN(sensorInterval)||sensorInterval<1){alert('Sensor interval must be >= 1');return;}const res=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mqtt_interval:mqttInterval,sensor_interval:sensorInterval})});if(!res.ok)throw new Error('Failed to save settings');alert(mqttInterval===0?'Settings saved! Periodic MQTT publishing disabled (will publish on sensor read).':'Settings saved successfully!');}catch(e){alert('Failed to save settings: '+e.message);}}
async function resetSettings(){try{if(!confirm('Reset both intervals to 10 seconds (default)?'))return;const res=await fetch('/api/settings/reset',{method:'POST'});if(!res.ok)throw new Error('Failed to reset settings');const data=await res.json();document.getElementById('mqtt-interval').value=data.mqtt_interval;document.getElementById('sensor-interval').value=data.sensor_interval;alert('Settings reset to defaults (10 seconds)!');}catch(e){alert('Failed to reset settings: '+e.message);}}
let isLoadingStatus=false;
async function safeLoadStatus(){if(isLoadingStatus||(focusModeActive&&!focusUsingWebSocket))return;isLoadingStatus=true;try{await loadStatus();}catch(e){console.error('Status load failed:',e);}finally{isLoadingStatus=false;}}
async function initializeDashboard(){loadTheme();await ensureSensorsResumed();await safeLoadStatus();await loadSensors();initSensorSocket();const modal=document.getElementById('sensorModal');if(modal){modal.addEventListener('click',e=>{if(e.target===modal){closeSensorModal();}});}document.addEventListener('keydown',e=>{if(e.key==='Escape')closeSensorModal();});setInterval(safeLoadStatus,10000);}
async function ensureSensorsResumed(){try{await fetch('/api/sensors/resume',{method:'POST'});}catch(e){console.warn('Resume on init failed:',e);}}

// Code Editor Functions
let currentFile='';
let originalContent='';
function showDashboard(){document.querySelector('.container').style.display='block';document.getElementById('editorSection').style.display='none';}
function showEditor(){document.querySelector('.container').style.display='none';document.getElementById('editorSection').style.display='block';loadFileList();}
async function loadFileList(){try{const res=await fetch('/api/webfiles/list');const data=await res.json();const selector=document.getElementById('fileSelector');selector.innerHTML='<option value="">-- Select a file --</option>';data.files.forEach(f=>{const opt=document.createElement('option');opt.value=f.name;opt.textContent=`${f.name} (${(f.size/1024).toFixed(1)} KB)`;selector.appendChild(opt);});}catch(e){alert('Failed to load file list: '+e.message);}}
async function loadFile(){const filename=document.getElementById('fileSelector').value;if(!filename)return;try{const res=await fetch('/api/webfiles/'+filename);if(!res.ok)throw new Error('Failed to load file');currentFile=filename;originalContent=await res.text();document.getElementById('codeEditor').value=originalContent;}catch(e){alert('Failed to load file: '+e.message);}}
async function saveFile(){if(!currentFile){alert('No file selected');return;}const content=document.getElementById('codeEditor').value;if(!confirm(`Save changes to ${currentFile}? This will update the file immediately.`))return;try{const res=await fetch('/api/webfiles/'+currentFile,{method:'PUT',headers:{'Content-Type':'text/plain'},body:content});if(!res.ok)throw new Error('Failed to save file');alert('File saved successfully! Refresh the page to see changes.');originalContent=content;}catch(e){alert('Failed to save file: '+e.message);}}
async function uploadAssetFile(){const input=document.getElementById('assetFileInput');const status=document.getElementById('assetUploadStatus');if(!input||!input.files||input.files.length===0){alert('Select an HTML, CSS, or JS file first.');return;}const file=input.files[0];const name=file.name||'';if(!name.toLowerCase().match(/\.(html|css|js)$/)){alert('Only .html, .css, or .js files are allowed.');return;}if(file.size>200*1024){alert('File exceeds the 200 KB limit.');return;}if(!confirm(`Upload ${name} to the device? This will overwrite the existing file immediately.`))return;status.textContent='Uploading...';try{const res=await fetch('/api/webfiles/'+encodeURIComponent(name),{method:'PUT',headers:{'Content-Type':'text/plain'},body:file});if(!res.ok){const errText=await res.text();throw new Error(errText||'Upload failed');}status.textContent='Upload successful!';input.value='';await loadFileList();alert(`${name} uploaded successfully.`);}catch(err){console.error('Asset upload failed',err);status.textContent=`Upload failed: ${err.message}`;}}
function refreshPreview(){if(!currentFile){alert('No file selected');return;}if(currentFile==='index.html'){if(confirm('Reload the dashboard to preview changes?')){window.location.reload();}}else{alert('Preview is only available for index.html. Save and refresh manually for other files.');}}
function resetFile(){if(!currentFile){alert('No file selected');return;}if(confirm('Reset to last saved version?')){document.getElementById('codeEditor').value=originalContent;}}

// Calibration Wizard System
const CalibrationWizard = {
  config: null,
  sensor: null,
  currentStep: 0,
  steps: [],
  readingsHistory: [],
  isStable: false,
  stableCount: 0,
  lastReading: null,
  focusInterval: null,
  
  // Sensor-specific configurations
  configs: {
    pH: {
      name: 'pH Sensor',
      icon: '⚗️',
      steps: [
        {id: 'intro', title: 'Introduction', skippable: false},
        {id: 'mid', title: 'Mid Point (7.00)', skippable: false, solution: '7.00 pH', value: 7.00},
        {id: 'low', title: 'Low Point (4.00)', skippable: true, solution: '4.00 pH', value: 4.00},
        {id: 'high', title: 'High Point (10.00)', skippable: true, solution: '10.00 pH', value: 10.00},
        {id: 'complete', title: 'Complete', skippable: false}
      ]
    },
    EC: {
      name: 'EC Sensor',
      icon: '⚡',
      steps: [
        {id: 'intro', title: 'Introduction', skippable: false},
        {id: 'dry', title: 'Dry Calibration', skippable: false, solution: 'Dry probe', isDry: true},
        {id: 'low', title: 'Low Point', skippable: false, solution: 'Low EC', needsValue: true},
        {id: 'high', title: 'High Point', skippable: true, solution: 'High EC', needsValue: true},
        {id: 'complete', title: 'Complete', skippable: false}
      ]
    },
    ORP: {
      name: 'ORP Sensor',
      icon: '🔋',
      steps: [
        {id: 'intro', title: 'Introduction', skippable: false},
        {id: 'calibrate', title: 'Calibration', skippable: false, solution: 'ORP Solution', needsValue: true},
        {id: 'complete', title: 'Complete', skippable: false}
      ]
    },
    RTD: {
      name: 'Temperature Sensor',
      icon: '🌡️',
      steps: [
        {id: 'intro', title: 'Introduction', skippable: false},
        {id: 'calibrate', title: 'Calibration', skippable: false, solution: 'Reference Temperature', needsValue: true},
        {id: 'complete', title: 'Complete', skippable: false}
      ]
    },
    DO: {
      name: 'Dissolved Oxygen',
      icon: '🫧',
      steps: [
        {id: 'intro', title: 'Introduction', skippable: false},
        {id: 'atmospheric', title: 'Atmospheric', skippable: false, solution: 'Air'},
        {id: 'zero', title: 'Zero Oxygen', skippable: true, solution: 'Zero O₂'},
        {id: 'complete', title: 'Complete', skippable: false}
      ]
    }
  },
  
  open(sensor) {
    if (!sensor || !sensor.type) return;
    
    const sensorType = sensor.type.toUpperCase();
    this.config = this.configs[sensorType];
    
    if (!this.config) {
      alert(`Calibration wizard not available for ${sensorType} sensors`);
      return;
    }
    
    this.sensor = sensor;
    this.currentStep = 0;
    this.readingsHistory = [];
    this.isStable = false;
    this.stableCount = 0;
    this.lastReading = null;
    
    this.steps = this.config.steps;
    
    const modal = document.getElementById('calibrationWizard');
    const wizardTitle = document.getElementById('wizardTitle');
    const wizardSubtitle = document.getElementById('wizardSubtitle');
    
    wizardTitle.innerHTML = `<i class='fas fa-magic'></i> ${this.config.icon} ${this.config.name} Calibration`;
    wizardSubtitle.textContent = 'Follow the guided steps to calibrate your sensor accurately';
    
    this.renderSteps();
    this.renderContent();
    this.updateButtons();
    
    modal.classList.remove('hidden');
    
    // Start focus mode for live readings
    if (this.sensor.address !== undefined) {
      startSensorFocus(this.sensor).then(() => {
        this.startReadingMonitor();
      });
    }
  },
  
  close() {
    const modal = document.getElementById('calibrationWizard');
    modal.classList.add('hidden');
    
    // Stop focus mode
    if (this.focusInterval) {
      clearInterval(this.focusInterval);
      this.focusInterval = null;
    }
    stopSensorFocus();
    
    // Refresh sensor list
    loadSensors();
  },
  
  renderSteps() {
    const container = document.getElementById('wizardSteps');
    const totalSteps = this.steps.length;
    
    let html = '<div class="flex items-center justify-between w-full">';
    
    this.steps.forEach((step, index) => {
      const isActive = index === this.currentStep;
      const isCompleted = index < this.currentStep;
      const isPending = index > this.currentStep;
      
      const status = isActive ? 'active' : (isCompleted ? 'completed' : 'pending');
      
      html += `<div class="wizard-step ${status} flex-1 relative">`;
      html += `<div class="wizard-step-circle">`;
      
      if (isCompleted) {
        html += `<i class="fas fa-check"></i>`;
      } else {
        html += `${index + 1}`;
      }
      
      html += `</div>`;
      html += `<div class="wizard-step-label">${step.title}</div>`;
      
      if (index < totalSteps - 1) {
        html += `<div class="wizard-step-line"></div>`;
      }
      
      html += `</div>`;
    });
    
    html += '</div>';
    container.innerHTML = html;
    
    // Update progress bar
    const progress = (this.currentStep / (totalSteps - 1)) * 100;
    document.getElementById('wizardProgress').style.width = `${progress}%`;
  },
  
  renderContent() {
    const step = this.steps[this.currentStep];
    const content = document.getElementById('wizardContent');
    
    if (step.id === 'intro') {
      content.innerHTML = this.renderIntroStep();
    } else if (step.id === 'complete') {
      content.innerHTML = this.renderCompleteStep();
    } else {
      content.innerHTML = this.renderCalibrationStep(step);
    }
  },
  
  renderIntroStep() {
    return `
      <div class="text-center space-y-6">
        <div class="text-6xl">${this.config.icon}</div>
        <h3 class="text-2xl font-bold text-gray-900 dark:text-white">Welcome to ${this.config.name} Calibration</h3>
        <div class="text-left max-w-md mx-auto space-y-4 text-gray-700 dark:text-gray-300">
          <p>This wizard will guide you through the calibration process step by step.</p>
          <div class="bg-blue-50 dark:bg-blue-900/30 border-l-4 border-blue-500 p-4 rounded">
            <p class="font-semibold text-blue-900 dark:text-blue-200">
              <i class="fas fa-info-circle mr-2"></i>Before you begin:
            </p>
            <ul class="mt-2 space-y-2 text-sm">
              <li>• Prepare your calibration solutions</li>
              <li>• Ensure the probe is clean and dry</li>
              <li>• Have a timer ready</li>
              <li>• Follow each step carefully</li>
            </ul>
          </div>
          <p class="text-sm text-gray-600 dark:text-gray-400">
            Calibration typically takes 5-10 minutes depending on probe response time.
          </p>
        </div>
      </div>
    `;
  },
  
  renderCompleteStep() {
    return `
      <div class="text-center space-y-6">
        <div class="text-6xl">✅</div>
        <h3 class="text-2xl font-bold text-green-600 dark:text-green-400">Calibration Complete!</h3>
        <div class="max-w-md mx-auto space-y-4 text-gray-700 dark:text-gray-300">
          <p>Your ${this.config.name} has been successfully calibrated.</p>
          <div class="bg-green-50 dark:bg-green-900/30 border-l-4 border-green-500 p-4 rounded text-left">
            <p class="font-semibold text-green-900 dark:text-green-200">
              <i class="fas fa-check-circle mr-2"></i>Next steps:
            </p>
            <ul class="mt-2 space-y-2 text-sm">
              <li>• Rinse the probe with distilled water</li>
              <li>• Store the probe properly</li>
              <li>• Verify readings with known samples</li>
              <li>• Re-calibrate if readings drift</li>
            </ul>
          </div>
        </div>
      </div>
    `;
  },
  
  renderCalibrationStep(step) {
    let html = `<div class="space-y-6">`;
    
    // Step header
    html += `
      <div class="text-center">
        <span class="solution-badge text-lg">${step.solution}</span>
        <h3 class="text-xl font-bold text-gray-900 dark:text-white mt-4">
          ${step.title}
        </h3>
      </div>
    `;
    
    // For dry calibration, skip live reading and stability
    if (!step.isDry) {
      // Live reading display
      html += `
        <div class="reading-display text-gray-900 dark:text-white">
          <div id="liveReading" class="text-5xl font-bold">--</div>
          <div class="text-sm mt-2 text-gray-600 dark:text-gray-400">Current Reading</div>
        </div>
      `;
      
      // Stability indicator
      html += `
        <div id="stabilityIndicator" class="text-center">
          <div class="stability-indicator unstable">
            <div class="stability-dots">
              <div class="stability-dot"></div>
              <div class="stability-dot"></div>
              <div class="stability-dot"></div>
            </div>
            <span>Stabilizing...</span>
          </div>
        </div>
      `;
    }
    
    // Instructions
    if (step.isDry) {
      html += `
        <div class="bg-yellow-50 dark:bg-yellow-900/30 border-l-4 border-yellow-500 p-4 rounded">
          <p class="font-semibold text-yellow-900 dark:text-yellow-200">
            <i class="fas fa-lightbulb mr-2"></i>Instructions:
          </p>
          <ol class="mt-2 space-y-2 text-sm text-yellow-900 dark:text-yellow-100">
            <li>1. Remove the probe from any solution</li>
            <li>2. Thoroughly dry the probe with a clean cloth or paper towel</li>
            <li>3. Ensure no liquid remains on the probe surface</li>
            <li>4. Click "Calibrate" to perform dry calibration</li>
          </ol>
        </div>
      `;
    } else {
      html += `
        <div class="bg-yellow-50 dark:bg-yellow-900/30 border-l-4 border-yellow-500 p-4 rounded">
          <p class="font-semibold text-yellow-900 dark:text-yellow-200">
            <i class="fas fa-lightbulb mr-2"></i>Instructions:
          </p>
          <ol class="mt-2 space-y-2 text-sm text-yellow-900 dark:text-yellow-100">
            <li>1. Immerse the probe in ${step.solution} solution</li>
            <li>2. Wait for the reading to stabilize (same value 3+ times)</li>
            <li>3. When stable, click "Calibrate" to capture this point</li>
          </ol>
        </div>
      `;
    }
    
    // Value input if needed
    if (step.needsValue) {
      html += `
        <div>
          <label class="block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2">
            Solution Value:
          </label>
          <input 
            type="number" 
            step="0.01" 
            id="calibrationValue" 
            class="w-full px-4 py-2 border border-gray-300 dark:border-gray-600 rounded-lg bg-white dark:bg-gray-800 text-gray-900 dark:text-white"
            placeholder="Enter value..."
          />
        </div>
      `;
    }
    
    // Calibrate button
    html += `
      <button 
        id="calibrateBtn"
        onclick="CalibrationWizard.calibratePoint('${step.id}')"
        class="w-full py-3 bg-green-600 hover:bg-green-700 disabled:bg-gray-400 disabled:cursor-not-allowed text-white rounded-lg font-semibold transition"
        ${step.isDry ? '' : 'disabled'}
      >
        <i class="fas fa-check-circle mr-2"></i>
        Calibrate This Point
      </button>
    `;
    
    html += `</div>`;
    return html;
  },
  
  startReadingMonitor() {
    // Clear any existing interval
    if (this.focusInterval) {
      clearInterval(this.focusInterval);
    }
    
    // Monitor readings every 500ms
    this.focusInterval = setInterval(() => {
      this.updateLiveReading();
    }, 500);
  },
  
  updateLiveReading() {
    const step = this.steps[this.currentStep];
    if (step.id === 'intro' || step.id === 'complete') return;
    
    const sensorType = this.sensor.type.toUpperCase();
    const snapshot = latestSensorSnapshots[sensorType];
    
    if (!snapshot || snapshot.value == null) return;
    
    let currentReading = snapshot.value;
    
    // Extract numeric value for stability check
    let numericValue = null;
    if (typeof currentReading === 'number') {
      numericValue = currentReading;
    } else if (typeof currentReading === 'object' && !Array.isArray(currentReading)) {
      // For HUM sensor, use first value
      const keys = Object.keys(currentReading);
      if (keys.length > 0) {
        numericValue = currentReading[keys[0]];
      }
    }
    
    if (numericValue === null) return;
    
    // Update display
    const display = document.getElementById('liveReading');
    if (display) {
      display.textContent = numericValue.toFixed(2);
    }
    
    // Check stability
    this.checkStability(numericValue);
  },
  
  checkStability(value) {
    const tolerance = 0.02; // Consider stable if within 2% of last reading
    
    if (this.lastReading !== null) {
      const diff = Math.abs(value - this.lastReading);
      const percentDiff = (diff / Math.abs(this.lastReading)) * 100;
      
      if (percentDiff <= tolerance) {
        this.stableCount++;
      } else {
        this.stableCount = 0;
      }
    }
    
    this.lastReading = value;
    this.readingsHistory.push(value);
    if (this.readingsHistory.length > 10) {
      this.readingsHistory.shift();
    }
    
    // Need 3 consecutive stable readings
    const wasStable = this.isStable;
    this.isStable = this.stableCount >= 3;
    
    // Update UI if stability changed
    if (wasStable !== this.isStable) {
      this.updateStabilityUI();
    }
  },
  
  updateStabilityUI() {
    const indicator = document.getElementById('stabilityIndicator');
    const calibrateBtn = document.getElementById('calibrateBtn');
    
    if (!indicator) return;
    
    if (this.isStable) {
      indicator.innerHTML = `
        <div class="stability-indicator stable">
          <i class="fas fa-check-circle text-xl"></i>
          <span>Reading Stable - Ready to Calibrate</span>
        </div>
      `;
      if (calibrateBtn) calibrateBtn.disabled = false;
    } else {
      indicator.innerHTML = `
        <div class="stability-indicator unstable">
          <div class="stability-dots">
            <div class="stability-dot"></div>
            <div class="stability-dot"></div>
            <div class="stability-dot"></div>
          </div>
          <span>Stabilizing... (${this.stableCount}/3)</span>
        </div>
      `;
      if (calibrateBtn) calibrateBtn.disabled = true;
    }
  },
  
  async calibratePoint(pointId) {
    const step = this.steps.find(s => s.id === pointId);
    if (!step) return;
    
    let value = step.value;
    
    // Get value from input if needed
    if (step.needsValue) {
      const input = document.getElementById('calibrationValue');
      if (!input || !input.value) {
        alert('Please enter the solution value');
        return;
      }
      value = parseFloat(input.value);
    }
    
    const payload = {point: pointId};
    if (value !== undefined) {
      payload.value = value;
    }
    
    try {
      const res = await fetch(`/api/sensors/calibrate/${this.sensor.address}`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
      });
      
      if (!res.ok) throw new Error('Calibration failed');
      
      // Move to next step
      this.nextStep();
      
    } catch (err) {
      alert(`Calibration failed: ${err.message}`);
    }
  },
  
  nextStep() {
    if (this.currentStep < this.steps.length - 1) {
      this.currentStep++;
      this.isStable = false;
      this.stableCount = 0;
      this.lastReading = null;
      this.readingsHistory = [];
      
      this.renderSteps();
      this.renderContent();
      this.updateButtons();
    } else {
      this.close();
    }
  },
  
  previousStep() {
    if (this.currentStep > 0) {
      this.currentStep--;
      this.isStable = false;
      this.stableCount = 0;
      this.lastReading = null;
      this.readingsHistory = [];
      
      this.renderSteps();
      this.renderContent();
      this.updateButtons();
    }
  },
  
  skipStep() {
    const step = this.steps[this.currentStep];
    if (step.skippable) {
      this.nextStep();
    }
  },
  
  updateButtons() {
    const backBtn = document.getElementById('wizardBack');
    const nextBtn = document.getElementById('wizardNext');
    const skipBtn = document.getElementById('wizardSkip');
    const step = this.steps[this.currentStep];
    
    // Back button
    backBtn.disabled = this.currentStep === 0;
    
    // Next button text
    if (step.id === 'intro') {
      nextBtn.innerHTML = `Start <i class="fas fa-arrow-right ml-2"></i>`;
      nextBtn.onclick = () => this.nextStep();
      nextBtn.disabled = false;
    } else if (step.id === 'complete') {
      nextBtn.innerHTML = `<i class="fas fa-check mr-2"></i> Finish`;
      nextBtn.onclick = () => this.close();
      nextBtn.disabled = false;
    } else {
      nextBtn.classList.add('hidden');
    }
    
    // Skip button
    if (step.skippable && step.id !== 'intro' && step.id !== 'complete') {
      skipBtn.classList.remove('hidden');
    } else {
      skipBtn.classList.add('hidden');
    }
  }
};

function openCalibrationWizard(sensor) {
  CalibrationWizard.open(sensor);
}

function closeCalibrationWizard() {
  CalibrationWizard.close();
}

window.onload=()=>{initializeDashboard();};
