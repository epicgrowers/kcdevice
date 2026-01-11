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
let currentTab=0;
const LOG_TAB_INDEX=5;
const LOG_POLL_INTERVAL_MS=5000;
const LOG_MAX_BUFFER=2000;
const LOG_COLOR_MAP={
  EZO_SENSOR:'#48d597',
  SENSOR_MGR:'#5fe3c2',
  MQTT_TELEM:'#7bb7ff',
  MQTT_CONN:'#4f8bff',
  PROV_RUN:'#f4c361',
  WIFI_MGR:'#f28f5c',
  BOOT_HANDLERS:'#f1556c',
  SECURITY:'#d78bff',
  NETWORK_BOOT:'#ffaaae',
  DEFAULT:'#c5c7ca'
};
const LOG_DEFAULT_GROUPS=[
  {key:'ALL',label:'All',matchers:[]},
  {key:'MQTT',label:'MQTT',matchers:['MQTT_']},
  {key:'SENSORS',label:'Sensors',matchers:['EZO_SENSOR','SENSOR_']},
  {key:'WIFI',label:'WiFi/Provisioning',matchers:['WIFI','PROV_']},
  {key:'SECURITY',label:'Security',matchers:['SECURITY']},
  {key:'BOOT',label:'Boot/Network',matchers:['BOOT','NETWORK_']},
  {key:'OTHER',label:'Other',matchers:[]}
];
const LOG_INLINE_ACTIONS=[
  {matchers:['WIFI','PROV_'],label:'Open Wi-Fi Controls',handler:()=>showTab(3)},
  {matchers:['MQTT_'],label:'Test MQTT',handler:()=>testMQTT()},
  {matchers:['EZO_SENSOR','SENSOR_'],label:'Sensors Tab',handler:()=>showTab(1)}
];
let logBuffer=[];
let logPollingTimer=null;
let logActiveGroup='ALL';
let logAutoScroll=true;
let logTerminalEl=null;
let logJumpButton=null;
let logInitialized=false;
let logSearchTerm='';
let logSearchMatches=[];
let logSearchIndex=-1;
let customLogGroups=[];
let pendingCustomGroups=[];
let logTagVisibility={};
let logPinnedIds=new Set();
let logBookmarks=[];
let logBaselineTimestamp=null;
let logTimeWindow=null;
let logAlertRules=[];
let logSequence=0;
let logIngressHistory=[];
let logContextDrawer=null;
let logLastRenderedIds=new Set();
let logSource='live';
let logArchiveDate='';
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
async function loadSensors(){const list=document.getElementById('sensor-list');const statusEl=document.createElement('div');statusEl.id='load-status';statusEl.className='flex flex-col items-center justify-center py-8';const updateStatus=(msg)=>{statusEl.innerHTML=`<div class="w-full max-w-md mb-3"><div class="h-2 bg-gray-200 dark:bg-gray-600 rounded-full overflow-hidden"><div class="h-full bg-gradient-to-r from-green-500 to-blue-500 animate-pulse rounded-full" style="width: 100%"></div></div></div><p class="text-gray-600 dark:text-gray-400 text-sm font-mono">${msg}</p>`;};try{if(list){list.innerHTML='';list.appendChild(statusEl);}updateStatus('⚡ Initializing I2C bus communication...');await new Promise(r=>setTimeout(r,100));updateStatus('🔍 Scanning I2C bus for EZO sensors...');const res=await fetch('/api/sensors',{signal:AbortSignal.timeout(30000)});if(!res.ok)throw new Error('Failed to load sensors');updateStatus('📥 Receiving sensor data from ESP32...');const d=await res.json();updateStatus('⚙️ Parsing sensor configurations...');await new Promise(r=>setTimeout(r,150));updateSensorDetailCache(Array.isArray(d.sensors)?d.sensors:[]);if(!list)return;if(!d.count){list.innerHTML='<p class="text-gray-600 dark:text-gray-400">No sensors detected</p>';return;}const sensorTypes=d.sensors.map(s=>s.type).filter(Boolean);updateStatus(`✓ Loaded ${d.count} sensor(s): ${sensorTypes.join(', ')}`);await new Promise(r=>setTimeout(r,200));list.innerHTML=d.sensors.map(renderSensorCard).join('');}catch(e){console.error('Failed to load sensors:',e);if(list){const isTimeout=e.name==='TimeoutError'||e.message.includes('timed out');if(isTimeout){list.innerHTML=`<p class='text-orange-500'>⚠️ Sensor loading timed out. <button onclick='loadSensors()' class='underline'>Retry</button></p>`;}else{list.innerHTML=`<p class='text-red-500'>Failed to load sensors: ${e.message}. <button onclick='loadSensors()' class='underline'>Retry</button></p>`;}}}}

function updateSensorDetailCache(items){const nextCache={};items.forEach(sensor=>{const key=(sensor?.type||'').trim().toUpperCase();if(!key)return;if(!nextCache[key]){nextCache[key]=[];}nextCache[key].push(sensor);});Object.keys(sensorDetailCache).forEach(key=>{if(!nextCache[key]){delete sensorDetailCache[key];}});Object.keys(nextCache).forEach(key=>{sensorDetailCache[key]=nextCache[key];});}


function renderSensorCard(sensor){
  const caps=Array.isArray(sensor.capabilities)?sensor.capabilities:[];
  const addrHex=formatAddress(sensor.address);
  const type=(sensor.type||'').trim();
  const typeKey=type.toUpperCase();
  const isPh=typeKey==='PH';
  const isOrp=typeKey==='ORP';
  const isRtd=typeKey==='RTD';
  const isEc=typeKey==='EC';
  const isDo=typeKey==='DO';
  const isHum=typeKey==='HUM';
  const supportsCalibration=caps.includes('calibration')||isPh||isOrp||isRtd||isEc||isDo;
  const supportsTempComp=(caps.includes('temp_comp')&&isPh)||isPh;
  const ecTempValue=Number.isFinite(sensor.ec_temp_comp)?sensor.ec_temp_comp:(sensor.temperature_comp??25);
  const tdsFactorValue=sensor.tds_factor??0.5;
  let html=`<div class='bg-gray-50 dark:bg-gray-700 p-4 rounded-lg mb-4 border-2 border-gray-200 dark:border-gray-600' id='sensor-card-${sensor.address}' data-sensor-address='${sensor.address}' data-sensor-type='${type}'>`;
  html+=`<h3 class='text-lg font-bold text-green-600 dark:text-green-400 mb-1'>${type||'Sensor'} @ 0x${addrHex}</h3>`;
  if(sensor.firmware){
    html+=`<p class='text-sm text-gray-600 dark:text-gray-400 mb-4'>Firmware: ${sensor.firmware}</p>`;
  }
  if(type!=='MAX17048'){
    html+=`<div class='border-t border-gray-300 dark:border-gray-600 pt-4 mt-2'><h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Basic Configuration</h4>`;
    html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Sensor Name</label><input class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' id='name-${sensor.address}' value='${sensor.name||''}' placeholder='Pool_pH' maxlength='16' pattern='[A-Za-z0-9_]+'><p class='text-xs text-gray-500 dark:text-gray-400 mt-1'>1-16 characters: letters, numbers, underscore only</p></div>`;
    html+=`<div class='mb-4'><label class='flex items-center text-gray-700 dark:text-gray-300'><input type='checkbox' class='mr-2' id='led-${sensor.address}' ${sensor.led?'checked':''}> LED On</label></div>`;
    if(isRtd){
      html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Temperature Scale</label><select class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' id='scale-${sensor.address}'><option ${sensor.scale==='C'?'selected':''}>C</option><option ${sensor.scale==='F'?'selected':''}>F</option><option ${sensor.scale==='K'?'selected':''}>K</option></select></div>`;
    }
    if(isEc){
      html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Probe K Value</label><input class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' type='number' step='0.1' id='probe-${sensor.address}' value='${sensor.probe_type??1.0}'></div>`;
    }
    if(isHum){
      html+=`<div class='mb-4'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Output Parameters</label><div class='flex flex-wrap gap-3 mb-2'><label class='flex items-center'><input type='checkbox' id='param-hum-${sensor.address}' ${sensor.param_hum?'checked':''} class='mr-2'> Humidity</label><label class='flex items-center'><input type='checkbox' id='param-t-${sensor.address}' ${sensor.param_t?'checked':''} class='mr-2'> Temperature</label><label class='flex items-center'><input type='checkbox' id='param-dew-${sensor.address}' ${sensor.param_dew?'checked':''} class='mr-2'> Dew Point</label></div><button class='w-full px-3 py-1.5 text-sm bg-blue-600 hover:bg-blue-700 text-white rounded' onclick='updateHumOutputParams(${sensor.address})'>Update Output Parameters</button></div>`;
    }
    html+=`<button class='w-full bg-green-600 dark:bg-green-400 hover:bg-green-700 dark:hover:bg-green-500 text-white px-4 py-2 rounded-md font-semibold' onclick='saveSensorConfig(${sensor.address})'><i class='fas fa-save'></i> Save ${type||'Sensor'} Settings</button>`;
    if(supportsCalibration){
      html+=`<div class='mt-4 border-t border-gray-300 dark:border-gray-600 pt-4'><h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Calibration</h4><button class='w-full bg-gradient-to-r from-green-600 to-green-500 hover:from-green-700 hover:to-green-600 text-white px-6 py-3 rounded-lg font-semibold shadow-lg hover:shadow-xl transition-all flex items-center justify-center gap-2' onclick='openCalibrationWizard({address:${sensor.address},type:"${type}",name:"${sensor.name||''}"});'><i class='fas fa-magic'></i> Start Calibration Wizard</button></div>`;
    }
    html+=`<div class='mt-4 border-t border-gray-300 dark:border-gray-600 pt-4'>`;
    html+=`<h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Diagnostics & Advanced</h4>`;
    html+=`<div class='flex flex-wrap gap-2'>`;
    html+=`<button class='px-3 py-2 text-sm bg-blue-600 hover:bg-blue-700 text-white rounded' onclick='findSensor(${sensor.address})'><i class='fas fa-search-location'></i> Find (Blink LED)</button>`;
    html+=`<button class='px-3 py-2 text-sm bg-purple-600 hover:bg-purple-700 text-white rounded' onclick='getDeviceStatus(${sensor.address})'><i class='fas fa-info-circle'></i> Device Status</button>`;
    if(supportsCalibration){
      html+=`<button class='px-3 py-2 text-sm bg-green-600 hover:bg-green-700 text-white rounded' onclick='exportCalibration(${sensor.address})'><i class='fas fa-download'></i> Export Cal</button>`;
      html+=`<button class='px-3 py-2 text-sm bg-green-600 hover:bg-green-700 text-white rounded' onclick='importCalibration(${sensor.address})'><i class='fas fa-upload'></i> Import Cal</button>`;
    }
    if(isPh){
      html+=`<button class='px-3 py-2 text-sm bg-indigo-600 hover:bg-indigo-700 text-white rounded' onclick='querySlope(${sensor.address})'><i class='fas fa-chart-line'></i> Check Slope</button>`;
    }
    html+=`</div></div>`;
    if(isEc){
      html+=`<div class='mt-4 border border-gray-300 dark:border-gray-600 rounded-lg p-4'>`;
      html+=`<h4 class='text-sm font-semibold text-gray-900 dark:text-white mb-3'>EC Advanced Settings</h4>`;
      html+=`<div class='mb-3'><label class='block text-sm text-gray-700 dark:text-gray-300 mb-1'>Temperature Compensation (°C)</label><div class='flex gap-2'><input id='ec-temp-${sensor.address}' type='number' step='0.1' value='${Number(ecTempValue??25).toFixed(1)}' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded bg-white dark:bg-gray-800 text-gray-900 dark:text-white'><button class='px-3 py-1 text-sm bg-blue-600 text-white rounded' onclick='setEcTempComp(${sensor.address})'>Apply</button></div></div>`;
      html+=`<div class='mb-3'><label class='block text-sm font-medium text-gray-700 dark:text-gray-300 mb-2'>Output Parameters</label><div class='flex flex-wrap gap-3'><label class='flex items-center'><input type='checkbox' id='param-ec-${sensor.address}' class='mr-1' ${sensor.param_ec===true?'checked':''}> EC</label><label class='flex items-center'><input type='checkbox' id='param-tds-${sensor.address}' class='mr-1' ${sensor.param_tds===true?'checked':''}> TDS</label><label class='flex items-center'><input type='checkbox' id='param-s-${sensor.address}' class='mr-1' ${sensor.param_s===true?'checked':''}> Salinity</label><label class='flex items-center'><input type='checkbox' id='param-sg-${sensor.address}' class='mr-1' ${sensor.param_sg===true?'checked':''}> Sp. Gravity</label></div><button class='mt-2 px-3 py-1 text-sm bg-green-600 text-white rounded' onclick='updateEcOutputParams(${sensor.address})'>Update Outputs</button></div>`;
      html+=`<div class='mt-3 border-t border-gray-200 dark:border-gray-700 pt-3'><label class='block text-sm text-gray-700 dark:text-gray-300 mb-2'>TDS Conversion Factor</label><input class='w-full px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' type='number' step='0.01' id='tds-${sensor.address}' value='${tdsFactorValue}'><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Multiplier for converting EC to TDS (typically 0.5-0.7)</p></div>`;
      html+=`</div>`;
    }
    html+=`</div>`;
  }
  if(type!=='MAX17048'){
    html+=`<div class='border-t border-gray-300 dark:border-gray-600 pt-4 mt-4'><h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Manual Command Mode</h4><div class='bg-yellow-50 dark:bg-yellow-900/20 border border-yellow-200 dark:border-yellow-800 rounded-lg p-3'><p class='text-xs text-gray-600 dark:text-gray-400 mb-3'>Send raw I2C commands directly to the sensor for debugging.</p><div class='flex gap-2 mb-2'><input id='manual-cmd-${sensor.address}' type='text' placeholder='e.g. i, R, O,?' class='flex-1 px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white font-mono text-sm'><button class='px-4 py-2 text-sm bg-indigo-600 hover:bg-indigo-700 text-white rounded font-semibold' onclick='sendManualCommand(${sensor.address})'><i class='fas fa-paper-plane'></i> Send</button></div><div id='manual-response-${sensor.address}' class='text-xs font-mono bg-gray-900 dark:bg-black text-green-400 p-2 rounded min-h-[60px] overflow-auto' style='display:none;'></div></div></div>`;
  }
  if(type!=='MAX17048'){
    html+=`<div class='border-t border-gray-300 dark:border-gray-600 pt-4 mt-4'><h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Advanced Settings</h4>`;
    if(supportsTempComp){
      html+=`<div class='bg-white dark:bg-gray-800 border border-gray-200 dark:border-gray-600 rounded-lg p-3 mb-3'>`;
      html+=renderPhTempCompSection(sensor);
      html+=`</div>`;
    }
    if(isPh){
      html+=`<div class='bg-white dark:bg-gray-800 border border-gray-200 dark:border-gray-600 rounded-lg p-3 mb-3'><label class='flex items-center text-gray-700 dark:text-gray-300'><input type='checkbox' class='mr-2' id='extscale-${sensor.address}' ${sensor.extended_scale?'checked':''}> Extended pH Scale (-1 to 15)</label><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Extends the readable pH range from 0-14 to -1 to 15</p></div>`;
    }
    html+=`<div class='bg-white dark:bg-gray-800 border border-gray-200 dark:border-gray-600 rounded-lg p-3'><label class='flex items-center text-gray-700 dark:text-gray-300'><input type='checkbox' class='mr-2' id='plock-${sensor.address}' ${sensor.plock?'checked':''}> Protocol Lock (I2C Only)</label><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Prevents switching between I2C and UART protocols</p></div></div>`;
  }
  if(caps.includes('sleep')){
    html+=`<div class='border-t border-gray-300 dark:border-gray-600 pt-4 mt-4'>`;
    html+=renderSleepSection(sensor);
    html+=`</div>`;
  }
  html+=`</div>`;
  return html;
}



function formatAddress(address){return Number(address).toString(16).toUpperCase().padStart(2,'0');}

function renderCapabilityBadges(caps){const map={calibration:'Calibration',temp_comp:'Temp Compensation',mode:'Measurement Mode',sleep:'Power State',offset:'Offset'};return `<div class='flex flex-wrap gap-2 mt-3'>${caps.map(cap=>`<span class='px-2 py-1 text-xs rounded-full bg-green-100 text-green-700 dark:bg-green-900 dark:text-green-200'>${map[cap]||cap}</span>`).join('')}</div>`;}

function renderPhCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';const points=[{key:'mid',label:'Mid (7.00)'},{key:'low',label:'Low (4.00)'},{key:'high',label:'High (10.00)'}];return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-4 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-4'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>pH Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><div class='space-y-3'>${points.map(p=>`<div class='flex items-center gap-3'><label class='text-xs font-medium text-gray-600 dark:text-gray-300 w-24 flex-shrink-0'>${p.label}</label><input id='${idPrefix}cal-${p.key}-${sensor.address}' type='number' step='0.01' value='${p.key==='mid'?7.00:p.key==='low'?4.00:10.00}' class='w-20 px-3 py-2 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white text-center'><button class='px-4 py-2 text-xs font-medium rounded-md bg-green-600 hover:bg-green-700 text-white transition-colors' onclick='calibratePhSensor(${sensor.address},"${p.key}")'>Calibrate</button></div>`).join('')}</div><button class='mt-4 w-full px-4 py-2 text-sm font-medium rounded-md bg-red-600 hover:bg-red-700 text-white transition-colors' onclick='calibratePhSensor(${sensor.address},"clear")'>Clear Calibration</button></div>`;}

function renderOrpCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>ORP Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><label class='block text-xs font-medium text-gray-600 dark:text-gray-300 mb-1'>Solution mV</label><div class='flex flex-col gap-2 md:flex-row md:items-center'><input id='${idPrefix}orp-cal-${sensor.address}' type='number' step='1' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white' placeholder='e.g. 470'><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateOrpSensor(${sensor.address})'>Calibrate</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='clearOrpCalibration(${sensor.address})'>Clear</button></div></div></div>`;}

function renderRtdCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';const tempValue=Number.isFinite(sensor.last_rtd_cal)?sensor.last_rtd_cal:(sensor.temperature_comp??25);return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>RTD Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><label class='block text-xs font-medium text-gray-600 dark:text-gray-300 mb-1'>Reference Temperature (°C)</label><div class='flex flex-col gap-2 md:flex-row md:items-center'><input id='${idPrefix}rtd-cal-${sensor.address}' type='number' step='0.1' value='${Number(tempValue||25).toFixed(2)}' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white'><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateRtdSensor(${sensor.address})'>Calibrate</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='calibrateRtdSensor(${sensor.address},true)'>Clear</button></div></div><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Place the probe in a trusted reference solution before calibrating.</p></div>`;}

function renderEcCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';const idPrefix=context==='modal'?'modal-':'';return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>EC Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><label class='block text-xs font-medium text-gray-600 dark:text-gray-300 mb-1'>Solution Conductivity (µS/cm)</label><input id='${idPrefix}ec-cal-value-${sensor.address}' type='number' step='1' class='w-full px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-900 text-gray-900 dark:text-white mb-3' placeholder='e.g. 12880'><div class='flex flex-wrap gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-indigo-600 text-white' onclick='calibrateEcSensor(${sensor.address},"dry")'>Dry</button><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateEcSensor(${sensor.address},"low")'>Low</button><button class='px-3 py-1.5 text-xs rounded-md bg-blue-600 text-white' onclick='calibrateEcSensor(${sensor.address},"high")'>High</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='calibrateEcSensor(${sensor.address},"clear")'>Clear</button></div><p class='text-xs text-gray-500 dark:text-gray-400 mt-2'>Low/high points require a conductivity value in µS/cm.</p></div>`;}

function renderDoCalibrationSection(sensor,context='card'){const status=sensor.calibration_status||'Unknown';return `<div class='mt-4 border border-gray-200 dark:border-gray-600 rounded-lg p-3 bg-white/40 dark:bg-gray-800/40'><div class='flex items-center justify-between mb-3'><h4 class='text-sm font-semibold text-gray-900 dark:text-white'>DO Calibration</h4><span class='text-xs text-gray-500 dark:text-gray-300'>Status: ${status}</span></div><p class='text-xs text-gray-500 dark:text-gray-400 mb-2'>Use atmospheric (air) or zero-oxygen solution for calibration.</p><div class='flex flex-wrap gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white' onclick='calibrateDoSensor(${sensor.address},"atm")'>Atmospheric</button><button class='px-3 py-1.5 text-xs rounded-md bg-blue-600 text-white' onclick='calibrateDoSensor(${sensor.address},"0")'>Zero</button><button class='px-3 py-1.5 text-xs rounded-md bg-red-600 text-white' onclick='calibrateDoSensor(${sensor.address},"clear")'>Clear</button></div></div>`;}

function renderPhTempCompSection(sensor){const tempValue=Number.isFinite(sensor.temperature_comp)?sensor.temperature_comp:(sensor.temperature_comp??sensor.temp_compensation??25);return `<h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>pH Temperature Compensation</h4><div class='bg-white dark:bg-gray-800 border border-gray-200 dark:border-gray-600 rounded-lg p-3'><label class='block text-sm text-gray-700 dark:text-gray-300 mb-2'>Temperature (°C)</label><div class='flex gap-2'><input id='ph-temp-${sensor.address}' type='number' step='0.1' value='${Number(tempValue||25).toFixed(2)}' class='flex-1 px-2 py-1 border border-gray-300 dark:border-gray-600 rounded-md bg-white dark:bg-gray-800 text-gray-900 dark:text-white' placeholder='25.0'><button class='px-3 py-1.5 text-sm bg-blue-600 text-white rounded' onclick='setPhTempComp(${sensor.address})'>Apply</button></div></div>`;}

function renderSleepSection(sensor){const sleeping=!!sensor.sleeping;return `<h4 class='text-sm font-semibold text-gray-700 dark:text-gray-300 mb-3'>Power Management</h4><div class='bg-yellow-50 dark:bg-yellow-900/20 border border-yellow-200 dark:border-yellow-800 rounded-lg p-3'><div class='flex items-center justify-between'><div><p class='text-sm font-medium text-gray-900 dark:text-white'>${sleeping?'Sensor is sleeping (low power)':'Sensor is awake and active'}</p><p class='text-xs text-gray-600 dark:text-gray-400 mt-1'>${sleeping?'Wake to take readings':'Sleep to conserve power'}</p></div><div class='flex gap-2'><button class='px-3 py-1.5 text-xs rounded-md bg-yellow-600 text-white ${sleeping?'opacity-50 cursor-not-allowed':'hover:bg-yellow-700'}' ${sleeping?'disabled':''} onclick='setSensorSleep(${sensor.address},true)'>Sleep</button><button class='px-3 py-1.5 text-xs rounded-md bg-green-600 text-white ${sleeping?'hover:bg-green-700':'opacity-50 cursor-not-allowed'}' ${sleeping?'':'disabled'} onclick='setSensorSleep(${sensor.address},false)'>Wake</button></div></div></div>`;}
async function rescanSensors(){alert('Rescanning I2C bus...');await fetch('/api/sensors/rescan',{method:'POST'});await loadSensors();alert('Rescan complete!');}
async function pauseSensors(silent=false){try{await fetch('/api/sensors/pause',{method:'POST'});if(!silent)alert('Sensor readings paused');}catch(e){if(!silent)alert('Failed to pause sensors');}}
async function resumeSensors(silent=false){try{await fetch('/api/sensors/resume',{method:'POST'});if(!silent)alert('Sensor readings resumed');}catch(e){if(!silent)alert('Failed to resume sensors');}}
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
async function setSensorSleep(address,sleep){const addrHex=formatAddress(address);const action=sleep?'put to sleep':'wake';if(!confirm(`Do you want to ${action} sensor 0x${addrHex}?`))return;await sensorActionRequest(`/api/sensors/power/${address}`,{sleep:!!sleep},sleep?'Sleep command sent.':'Wake command sent.');}
async function sendManualCommand(address){const input=document.getElementById(`manual-cmd-${address}`);const responseDiv=document.getElementById(`manual-response-${address}`);const command=(input?.value||'').trim();if(!command){alert('Enter a command');return;}responseDiv.style.display='block';responseDiv.textContent='Sending...';try{const res=await fetch(`/api/sensors/command/${address}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command})});if(!res.ok){const err=await res.text();throw new Error(err||'Command failed');}const data=await res.json();const timestamp=new Date().toLocaleTimeString();responseDiv.textContent=`[${timestamp}] > ${data.command}\n< ${data.response||'(no response)'}`;input.value='';}catch(e){responseDiv.textContent=`Error: ${e.message}`;console.error('Manual command failed:',e);}}
async function showTab(n){const previousTab=currentTab;currentTab=n;
// Tabs that require sensors to be paused
const pauseRequiredTabs=[1,4]; // Tab 1: Sensors, Tab 4: Manual Mode
const wasPauseRequired=pauseRequiredTabs.includes(previousTab);
const isPauseRequired=pauseRequiredTabs.includes(n);

// Only toggle sensor state if the pause requirement changed
if(wasPauseRequired&&!isPauseRequired){
  await resumeSensors(true);
} else if(!wasPauseRequired&&isPauseRequired){
  await pauseSensors(true);
}

document.querySelectorAll('.tab').forEach((t,i)=>{if(i===n){t.className='px-6 py-3 text-green-600 dark:text-green-400 border-b-2 border-green-600 dark:border-green-400 font-semibold tab active';}else{t.className='px-6 py-3 text-gray-600 dark:text-gray-400 border-b-2 border-transparent hover:text-gray-900 dark:hover:text-white tab';}});document.querySelectorAll('.tab-content').forEach((c,i)=>{c.style.display=(i===n)?'block':'none';});if(n===1)loadSensors();if(n===2){loadSettings();loadDeviceInfo();}if(n===LOG_TAB_INDEX){await loadLogs(true);startLogPolling();}else if(previousTab===LOG_TAB_INDEX){stopLogPolling();}}
async function loadDeviceInfo(){try{const res=await fetch('/api/device/info');if(!res.ok)throw new Error('Failed to load device info');const data=await res.json();document.getElementById('device-id').value=data.device_id||'';document.getElementById('device-name').value=data.device_name||'';}catch(e){console.error('Failed to load device info:',e);}}
async function saveDeviceName(){try{const deviceName=document.getElementById('device-name').value.trim();if(deviceName&&deviceName.length>64){alert('Device name must be 64 characters or less');return;}const res=await fetch('/api/device/name',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_name:deviceName})});if(!res.ok){const text=await res.text();throw new Error(text||'Failed to save device name');}const data=await res.json();document.getElementById('device-name').value=data.device_name||'';alert('Device name saved successfully!');}catch(e){console.error('Failed to save device name:',e);alert('Error: '+e.message);}}
async function clearDeviceName(){try{const res=await fetch('/api/device/name',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({device_name:''})});if(!res.ok)throw new Error('Failed to clear device name');document.getElementById('device-name').value='';alert('Device name cleared!');}catch(e){console.error('Failed to clear device name:',e);alert('Error: '+e.message);}}
async function loadSettings(){try{const res=await fetch('/api/settings');if(!res.ok)throw new Error('Failed to load settings');const data=await res.json();document.getElementById('mqtt-interval').value=data.mqtt_interval!==undefined?data.mqtt_interval:10;document.getElementById('sensor-interval').value=data.sensor_interval!==undefined?data.sensor_interval:10;}catch(e){console.error('Failed to load settings:',e);}}
async function saveSettings(){try{const mqttInterval=parseInt(document.getElementById('mqtt-interval').value);const sensorInterval=parseInt(document.getElementById('sensor-interval').value);if(isNaN(mqttInterval)||mqttInterval<0){alert('MQTT interval must be >= 0');return;}if(isNaN(sensorInterval)||sensorInterval<1){alert('Sensor interval must be >= 1');return;}const res=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mqtt_interval:mqttInterval,sensor_interval:sensorInterval})});if(!res.ok)throw new Error('Failed to save settings');alert(mqttInterval===0?'Settings saved! Periodic MQTT publishing disabled (will publish on sensor read).':'Settings saved successfully!');}catch(e){alert('Failed to save settings: '+e.message);}}
async function resetSettings(){try{if(!confirm('Reset both intervals to 10 seconds (default)?'))return;const res=await fetch('/api/settings/reset',{method:'POST'});if(!res.ok)throw new Error('Failed to reset settings');const data=await res.json();document.getElementById('mqtt-interval').value=data.mqtt_interval;document.getElementById('sensor-interval').value=data.sensor_interval;alert('Settings reset to defaults (10 seconds)!');}catch(e){alert('Failed to reset settings: '+e.message);}}
let isLoadingStatus=false;
async function safeLoadStatus(){if(isLoadingStatus||(focusModeActive&&!focusUsingWebSocket))return;isLoadingStatus=true;try{await loadStatus();}catch(e){console.error('Status load failed:',e);}finally{isLoadingStatus=false;}}
async function initializeDashboard(){loadTheme();await ensureSensorsResumed();await safeLoadStatus();initSensorSocket();const modal=document.getElementById('sensorModal');if(modal){modal.addEventListener('click',e=>{if(e.target===modal){closeSensorModal();}});}document.addEventListener('keydown',e=>{if(e.key==='Escape')closeSensorModal();});ensureLogConsoleReady();setInterval(safeLoadStatus,10000);}
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
    PH: {
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

async function findSensor(address){try{const res=await fetch(`/api/sensors/find/${address}`,{method:'POST'});if(!res.ok)throw new Error('Find command failed');alert(`Sensor 0x${formatAddress(address)} LED is now blinking rapidly for identification.`);}catch(e){alert('Find command failed: '+e.message);}}

async function getDeviceStatus(address){try{const res=await fetch(`/api/sensors/device-status/${address}`);if(!res.ok)throw new Error('Status query failed');const data=await res.json();alert(`Device Status for 0x${formatAddress(address)}:\n\n${data.device_status}`);}catch(e){alert('Status query failed: '+e.message);}}

async function exportCalibration(address){try{const res=await fetch(`/api/sensors/export/${address}`);if(!res.ok)throw new Error('Export failed');const data=await res.json();const textarea=document.createElement('textarea');textarea.value=data.calibration_data;textarea.style.position='fixed';textarea.style.top='50%';textarea.style.left='50%';textarea.style.transform='translate(-50%, -50%)';textarea.style.width='80%';textarea.style.height='200px';textarea.style.zIndex='10000';textarea.style.padding='20px';textarea.style.border='2px solid #4CAF50';textarea.style.borderRadius='8px';textarea.style.fontSize='14px';document.body.appendChild(textarea);textarea.select();document.execCommand('copy');alert(`✓ Calibration data exported and copied to clipboard!\n\nData: ${data.calibration_data}\n\nSave this string to restore calibration later.`);document.body.removeChild(textarea);}catch(e){alert('Export failed: '+e.message);}}

async function importCalibration(address){const calibData=prompt('Paste the calibration data string:');if(!calibData||calibData.trim().length===0){return;}try{const res=await fetch(`/api/sensors/import/${address}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({calibration_data:calibData.trim()})});if(!res.ok)throw new Error('Import failed');alert('✓ Calibration data imported successfully!');await loadSensors();}catch(e){alert('Import failed: '+e.message);}}

async function querySlope(address){try{const res=await fetch(`/api/sensors/slope/${address}`);if(!res.ok)throw new Error('Slope query failed');const data=await res.json();alert(`pH Slope Values for 0x${formatAddress(address)}:\n\n${data.slope}\n\nThese values indicate calibration quality. Standard slopes are typically 99.7-100.3% for good calibration.`);}catch(e){alert('Slope query failed: '+e.message);}}

async function setEcTempComp(address){const input=document.getElementById(`ec-temp-${address}`);if(!input)return;const temp=parseFloat(input.value);if(isNaN(temp)){alert('Enter a valid temperature value');return;}try{const res=await fetch(`/api/sensors/ec-temp-comp/${address}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({temp_c:temp})});if(!res.ok)throw new Error('Failed to set temperature compensation');alert('✓ EC temperature compensation updated');await loadSensors();}catch(e){alert('Failed: '+e.message);}}

async function updateEcOutputParams(address){const ec=document.getElementById(`param-ec-${address}`)?.checked||false;const tds=document.getElementById(`param-tds-${address}`)?.checked||false;const s=document.getElementById(`param-s-${address}`)?.checked||false;const sg=document.getElementById(`param-sg-${address}`)?.checked||false;try{const res=await fetch(`/api/sensors/ec-output-params/${address}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ec,tds,s,sg})});if(!res.ok)throw new Error('Failed to update output parameters');alert('✓ EC output parameters updated');await loadSensors();}catch(e){alert('Failed: '+e.message);}}

async function updateHumOutputParams(address){const hum=document.getElementById(`param-hum-${address}`)?.checked||false;const t=document.getElementById(`param-t-${address}`)?.checked||false;const dew=document.getElementById(`param-dew-${address}`)?.checked||false;try{const res=await fetch(`/api/sensors/hum-output-params/${address}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hum,t,dew})});if(!res.ok)throw new Error('Failed to update output parameters');alert('✓ HUM output parameters updated');await loadSensors();}catch(e){alert('Failed: '+e.message);}}

// Log Viewer
function normalizeLogArchiveDate(value){if(!value)return'';const match=value.trim().match(/^(\d{4})-(\d{2})-(\d{2})$/);if(!match)return'';const year=Number(match[1]);const month=Number(match[2]);const day=Number(match[3]);if(year<2000||year>2099||month<1||month>12||day<1||day>31)return'';const utcDate=new Date(Date.UTC(year,month-1,day));if(utcDate.getUTCFullYear()!==year||utcDate.getUTCMonth()+1!==month||utcDate.getUTCDate()!==day)return'';return`${match[1]}-${match[2]}-${match[3]}`;}
function initializeLogConsole(){if(logInitialized)return;logTerminalEl=document.getElementById('logTerminal');logJumpButton=document.getElementById('logJumpToLatest');logContextDrawer=document.getElementById('logContextDrawer');if(!logTerminalEl)return;loadCustomLogGroups();loadLogAlertRules();buildLogGroupFilters();renderLogLegend();renderLogBookmarks();renderLogStats();attachLogEventHandlers();const autoToggle=document.getElementById('logAutoScrollToggle');if(autoToggle)autoToggle.checked=logAutoScroll;updateLogSourceControls();logInitialized=true;}

function ensureLogConsoleReady(){if(!logInitialized){initializeLogConsole();}}

function handleLogSourceChange(value){const next=value==='sd'?'sd':'live';if(logSource===next)return;logSource=next;updateLogSourceControls();if(logSource==='live'){startLogPolling();loadLogs(true);}else{stopLogPolling();if(!logArchiveDate){const status=document.getElementById('logStatus');if(status){status.textContent='Select a date to load archive logs';}}else{loadLogs(true);}}}

function handleLogArchiveDate(value){const normalized=normalizeLogArchiveDate(value);const dateInput=document.getElementById('logArchiveDate');if(!normalized){if(value){showLogToast('Enter a valid date (YYYY-MM-DD, 2000-2099)');}logArchiveDate='';if(dateInput&&dateInput.value){dateInput.value='';}return;}logArchiveDate=normalized;if(dateInput&&dateInput.value!==normalized){dateInput.value=normalized;}if(logSource==='sd'){loadLogs(true);}}

function loadArchiveLogs(){if(logSource!=='sd')return;if(!logArchiveDate){showLogToast('Select a date first');return;}loadLogs(true);}

function updateLogSourceControls(){const dateInput=document.getElementById('logArchiveDate');const loadButton=document.getElementById('logArchiveLoad');if(dateInput){if(logSource==='sd'){dateInput.style.display='inline-flex';}else{dateInput.style.display='none';dateInput.value='';logArchiveDate='';}}if(loadButton){loadButton.style.display=logSource==='sd'?'inline-flex':'none';}}

function attachLogEventHandlers(){if(!logTerminalEl)return;logTerminalEl.addEventListener('scroll',handleLogScrollEvent);logTerminalEl.addEventListener('click',handleLogTerminalClick);logTerminalEl.addEventListener('mouseover',handleLogHover);logTerminalEl.addEventListener('mouseleave',clearCorrelationHighlight);}

function loadCustomLogGroups(){try{const stored=localStorage.getItem('logCustomGroups');customLogGroups=stored?JSON.parse(stored):[];customLogGroups=Array.isArray(customLogGroups)?customLogGroups:[];customLogGroups=customLogGroups.map((group,index)=>({...group,key:group.key||`custom-${index}`}));}catch(e){console.warn('Failed to parse custom groups',e);customLogGroups=[];}}

function loadLogAlertRules(){try{const raw=localStorage.getItem('logAlertRules');logAlertRules=raw?JSON.parse(raw):[];if(!Array.isArray(logAlertRules))logAlertRules=[];}catch(e){console.warn('Failed to parse alert rules',e);logAlertRules=[];}}

function buildLogGroupFilters(){const defaultContainer=document.getElementById('logGroupFilters');const customContainer=document.getElementById('logCustomFilters');if(!defaultContainer||!customContainer)return;const defaultHtml=LOG_DEFAULT_GROUPS.map(group=>`<button class="log-chip ${group.key===logActiveGroup?'active':''}" data-log-group='${group.key}'>${group.label}</button>`).join('');defaultContainer.innerHTML=defaultHtml;const customHtml=customLogGroups.map(group=>`<button class="log-chip ${group.key===logActiveGroup?'active':''}" data-log-group='${group.key}'><i class='fas fa-star mr-1'></i>${group.label}</button>`).join('');customContainer.innerHTML=customHtml||'<span class="text-xs text-gray-500">No custom groups yet.</span>';document.querySelectorAll('[data-log-group]').forEach(btn=>{btn.addEventListener('click',()=>setActiveLogGroup(btn.getAttribute('data-log-group')));});}

function setActiveLogGroup(key){if(!key)return;logActiveGroup=key;document.querySelectorAll('[data-log-group]').forEach(btn=>{const isActive=btn.getAttribute('data-log-group')===key;btn.classList.toggle('active',isActive);});renderLogEntries();}

async function loadLogs(manual=false){ensureLogConsoleReady();const status=document.getElementById('logStatus');if(status&&manual){status.textContent=logSource==='sd'?'Loading archive...':'Refreshing...';}const params=new URLSearchParams();if(logSource==='sd'){const normalized=normalizeLogArchiveDate(logArchiveDate);if(!normalized){if(status){status.textContent='Select a valid archive date';}if(manual){showLogToast('Select a valid archive date (YYYY-MM-DD, 2000-2099)');}return;}logArchiveDate=normalized;params.set('source','sd');params.set('date',normalized);}const url=params.toString()?`/api/logs?${params.toString()}`:'/api/logs';try{const res=await fetch(url,{signal:AbortSignal.timeout(5000)});if(!res.ok){const errText=await res.text();throw new Error(errText||'Failed to load logs');}const data=await res.json();const entries=Array.isArray(data.entries)?data.entries:[];ingestLogEntries(entries,{reset:logSource==='sd',trackRate:logSource==='live'});if(status){const label=logSource==='sd'?`Archive ${logArchiveDate}`:'Live log';status.textContent=`${label} updated ${new Date().toLocaleTimeString()}`;}}catch(err){console.error('Failed to load logs:',err);if(status){status.textContent='Unable to load logs';}showLogToast(`Log fetch failed: ${err.message}`);}}

function ingestLogEntries(entries,options={}){const{reset=false,trackRate=true}=options;if(reset){logBuffer=[];logIngressHistory=[];logPinnedIds.clear();logBookmarks=[];logBaselineTimestamp=null;logLastRenderedIds=new Set();}if(!entries||entries.length===0){if(reset){renderLogEntries();}return;}let appended=false;entries.forEach(raw=>{const normalized=normalizeLogEntry(raw);if(!normalized)return;logBuffer.push(normalized);if(trackRate){logIngressHistory.push(normalized.ts);if(logIngressHistory.length>500){logIngressHistory.shift();}}appended=true;evaluateAlertRules(normalized);});if(!appended)return;if(logBuffer.length>LOG_MAX_BUFFER){logBuffer=logBuffer.slice(-LOG_MAX_BUFFER);}if(!trackRate){logIngressHistory=[];}renderLogEntries();}

function normalizeLogEntry(raw){if(!raw)return null;const now=Date.now();const ageMs=Number(raw.age_sec)*1000;let ts=raw.timestamp?Date.parse(raw.timestamp):Number(raw.ts);if(!Number.isFinite(ts)&&Number.isFinite(ageMs)){ts=now-ageMs;}if(!Number.isFinite(ts))ts=now;const tag=String(raw.tag||'OTHER').trim().toUpperCase();const level=String(raw.level||'INFO').trim().toUpperCase();const message=typeof raw.message==='string'?raw.message:JSON.stringify(raw.message??'');const severity=detectLogSeverity(level,message);const color=resolveLogColor(tag);const correlationId=extractCorrelationId(raw,message);const group=resolveLogGroup(tag);return{ id:`log-${++logSequence}`, ts, tag, level, message, severity, color, correlationId, groupKey:group.key, groupLabel:group.label, displayTime:new Date(ts).toLocaleTimeString(), raw };}

function detectLogSeverity(level,message){const normalized=(level||'').toUpperCase();if(normalized.startsWith('E'))return'error';if(normalized.startsWith('W'))return'warn';if(normalized.startsWith('D'))return'debug';if(normalized.startsWith('I'))return'info';const firstChar=(message||'').trim().charAt(0).toUpperCase();if(firstChar==='E')return'error';if(firstChar==='W')return'warn';return'info';}

function resolveLogColor(tag){const direct=LOG_COLOR_MAP[tag];if(direct)return direct;const matchKey=Object.keys(LOG_COLOR_MAP).find(key=>key!=='DEFAULT'&&tag.startsWith(key));return matchKey?LOG_COLOR_MAP[matchKey]:LOG_COLOR_MAP.DEFAULT;}

function resolveLogGroup(tag){const custom=customLogGroups.find(group=>group.patterns?.some(pattern=>matchesPattern(tag,pattern)));if(custom)return custom;for(const group of LOG_DEFAULT_GROUPS){if(group.key==='ALL'||group.key==='OTHER')continue;if(group.matchers.some(pattern=>matchesPattern(tag,pattern))){return group;}}return LOG_DEFAULT_GROUPS.find(g=>g.key==='OTHER')||LOG_DEFAULT_GROUPS[0];}

function matchesPattern(tag,pattern){if(!pattern)return false;const trimmed=pattern.trim();if(!trimmed)return false;if(trimmed.startsWith('/')&&trimmed.endsWith('/')){try{return new RegExp(trimmed.slice(1,-1),'i').test(tag);}catch(e){return false;}}return tag.startsWith(trimmed.toUpperCase());}

function extractCorrelationId(raw,message){if(raw&&raw.correlation_id)return String(raw.correlation_id);const text=message||'';const match=text.match(/(?:corr(?:elation)?|trace|cid)[^A-Za-z0-9]?([A-Za-z0-9-]{4,})/i);return match?match[1].toUpperCase():null;}

function startLogPolling(){if(logSource!=='live'||logPollingTimer)return;logPollingTimer=setInterval(()=>{loadLogs();},LOG_POLL_INTERVAL_MS);}

function stopLogPolling(){if(!logPollingTimer)return;clearInterval(logPollingTimer);logPollingTimer=null;}

function getVisibleLogs(){return logBuffer.filter(entry=>{if(logTagVisibility[entry.tag]===false)return false;if(logTimeWindow){if(logTimeWindow.start&&entry.ts<logTimeWindow.start)return false;if(logTimeWindow.end&&entry.ts>logTimeWindow.end)return false;}if(logActiveGroup==='ALL')return true;const activeCustom=customLogGroups.find(g=>g.key===logActiveGroup);if(activeCustom)return entry.groupKey===logActiveGroup;return entry.groupKey===logActiveGroup||(logActiveGroup==='OTHER'&&entry.groupKey==='OTHER');});}

function renderLogEntries(){const terminal=logTerminalEl||document.getElementById('logTerminal');const emptyState=document.getElementById('logEmptyState');if(!terminal)return;const visible=getVisibleLogs();if(visible.length===0){terminal.innerHTML='';if(emptyState)emptyState.classList.remove('hidden');renderLogLegend();renderLogStats();return;}if(emptyState)emptyState.classList.add('hidden');const fragments=visible.map(renderLogLine).join('');terminal.innerHTML=fragments;logLastRenderedIds=new Set(visible.map(v=>v.id));renderLogLegend();renderLogStats();if(logSearchTerm){recomputeSearchMatches();highlightCurrentSearchMatch();}if(logAutoScroll){requestAnimationFrame(()=>scrollLogToBottom());}}

function renderLogLine(entry){const severityClass=`severity-${entry.severity}`;const baselineClass=logBaselineTimestamp&&entry.ts>=logBaselineTimestamp?' after-baseline':'';const pinnedClass=logPinnedIds.has(entry.id)?' highlighted':'';const corr=entry.correlationId?` data-correlation='${entry.correlationId}'`:'';const severityLabel=entry.severity.toUpperCase();const inlineAction=resolveInlineAction(entry.tag);const actionButton=inlineAction?`<button class='log-line-button' data-inline='${entry.id}' title='${inlineAction.label}'><i class='fas fa-bolt'></i></button>`:'';const bookmarkIcon=logPinnedIds.has(entry.id)?'fas fa-bookmark':'far fa-bookmark';const safeTag=escapeHtml(entry.tag);const safeMessage=escapeHtml(entry.message);const searchText=applySearchHighlight(safeMessage);return `<div class='log-line${baselineClass}${pinnedClass}' data-entry-id='${entry.id}'${corr}>`+
`<span class='log-timestamp'>[${entry.displayTime}]</span>`+
`<span class='log-tag' style='background:${entry.color}' title='${safeTag}'>${safeTag}</span>`+
`<div class='log-message' title='${safeMessage}'>${searchText}</div>`+
`<div class='log-line-controls'>`+
`<span class='log-severity ${severityClass}'>${severityLabel}</span>`+
`<button class='log-line-button' data-copy='${entry.id}' title='Copy line'><i class='fas fa-copy'></i></button>`+
`<button class='log-line-button' data-bookmark='${entry.id}' title='Pin bookmark'><i class='${bookmarkIcon}'></i></button>`+
`<button class='log-line-button' data-context='${entry.id}' title='Show context'><i class='fas fa-eye'></i></button>`+actionButton+
`</div>`+
`</div>`;}

function resolveInlineAction(tag){return LOG_INLINE_ACTIONS.find(action=>action.matchers.some(pattern=>tag.startsWith(pattern)));}

function handleLogTerminalClick(event){const target=event.target.closest('button');if(!target)return;const entryId=target.getAttribute('data-copy')||target.getAttribute('data-bookmark')||target.getAttribute('data-context')||target.getAttribute('data-inline');if(!entryId)return;if(target.hasAttribute('data-copy')){copyLogLine(entryId);}else if(target.hasAttribute('data-bookmark')){toggleLogBookmark(entryId);}else if(target.hasAttribute('data-context')){openLogContext(entryId);}else if(target.hasAttribute('data-inline')){runInlineAction(entryId);}}

function handleLogHover(event){const line=event.target.closest('.log-line');if(!line||!line.dataset.correlation){clearCorrelationHighlight();return;}const corr=line.dataset.correlation;document.querySelectorAll('.log-line').forEach(el=>{el.classList.toggle('correlation-highlight',el.dataset.correlation===corr);});}

function clearCorrelationHighlight(){document.querySelectorAll('.log-line.correlation-highlight').forEach(el=>el.classList.remove('correlation-highlight'));}

function runInlineAction(entryId){const entry=logBuffer.find(item=>item.id===entryId);if(!entry)return;const action=resolveInlineAction(entry.tag);if(action&&typeof action.handler==='function'){action.handler();}}

async function copyLogLine(entryId){const entry=logBuffer.find(item=>item.id===entryId);if(!entry)return;const text=`[${entry.displayTime}] ${entry.tag} ${entry.message}`;try{await navigator.clipboard.writeText(text);showLogToast('Log line copied');}catch(e){console.warn('Clipboard failed',e);}}

function toggleLogBookmark(entryId){if(logPinnedIds.has(entryId)){logPinnedIds.delete(entryId);logBookmarks=logBookmarks.filter(b=>b.id!==entryId);}else{const entry=logBuffer.find(item=>item.id===entryId);if(!entry)return;logPinnedIds.add(entryId);logBookmarks.unshift({id:entry.id,tag:entry.tag,time:entry.displayTime,message:entry.message.slice(0,120)});logBookmarks=logBookmarks.slice(0,20);}renderLogEntries();renderLogBookmarks();}

function renderLogBookmarks(){const container=document.getElementById('logBookmarks');if(!container)return;if(logBookmarks.length===0){container.innerHTML="<div class='text-xs text-gray-500'>No bookmarks yet.</div>";return;}container.innerHTML=logBookmarks.map(bookmark=>`<div class='log-bookmark-card'><p class='text-xs text-gray-400'>[${bookmark.time}] ${bookmark.tag}</p><p class='text-sm text-white mb-2'>${escapeHtml(bookmark.message)}</p><div class='flex justify-between text-xs text-gray-500'><button onclick='scrollToLog("${bookmark.id}")' class='text-green-400'>Jump</button><button onclick='removeBookmark("${bookmark.id}")' class='text-red-400'>Remove</button></div></div>`).join('');}

function scrollToLog(entryId){const line=document.querySelector(`.log-line[data-entry-id="${entryId}"]`);if(!line)return;line.scrollIntoView({behavior:'smooth',block:'center'});line.classList.add('highlighted');setTimeout(()=>line.classList.remove('highlighted'),2000);}

function removeBookmark(entryId){logPinnedIds.delete(entryId);logBookmarks=logBookmarks.filter(b=>b.id!==entryId);renderLogEntries();renderLogBookmarks();}

function clearLogBookmarks(){logPinnedIds.clear();logBookmarks=[];renderLogEntries();renderLogBookmarks();}

function handleLogScrollEvent(){if(!logTerminalEl)return;const nearBottom=logTerminalEl.scrollTop>=logTerminalEl.scrollHeight-logTerminalEl.clientHeight-40;logAutoScroll=nearBottom;const toggle=document.getElementById('logAutoScrollToggle');if(toggle&&toggle.checked!==logAutoScroll){toggle.checked=logAutoScroll;}if(logJumpButton){logJumpButton.classList.toggle('hidden',logAutoScroll);} }

function toggleLogAutoscroll(state){logAutoScroll=state;const toggle=document.getElementById('logAutoScrollToggle');if(toggle&&toggle.checked!==state){toggle.checked=state;}if(state){scrollLogToBottom();}else if(logJumpButton){logJumpButton.classList.remove('hidden');}}

function scrollLogToBottom(){if(!logTerminalEl)return;logTerminalEl.scrollTop=logTerminalEl.scrollHeight; if(logJumpButton)logJumpButton.classList.add('hidden');}

function jumpToLatestLogs(){toggleLogAutoscroll(true);}

function handleLogSearch(value){logSearchTerm=value||'';recomputeSearchMatches();renderLogEntries();}

function recomputeSearchMatches(){logSearchMatches=[];logSearchIndex=-1;if(!logSearchTerm)return;const pattern=logSearchTerm.startsWith('/')&&logSearchTerm.endsWith('/')?new RegExp(logSearchTerm.slice(1,-1),'gi'):new RegExp(escapeRegExp(logSearchTerm),'gi');getVisibleLogs().forEach(entry=>{if(pattern.test(entry.message)){logSearchMatches.push(entry.id);}pattern.lastIndex=0;});if(logSearchMatches.length){logSearchIndex=0;}}

function highlightCurrentSearchMatch(){if(logSearchMatches.length===0)return;const entryId=logSearchMatches[(logSearchIndex+logSearchMatches.length)%logSearchMatches.length];const line=document.querySelector(`.log-line[data-entry-id="${entryId}"]`);if(line){line.classList.add('highlighted');line.scrollIntoView({behavior:'smooth',block:'center'});setTimeout(()=>line.classList.remove('highlighted'),1500);}}

function jumpLogSearch(direction){if(logSearchMatches.length===0)return;logSearchIndex=(logSearchIndex+direction+logSearchMatches.length)%logSearchMatches.length;highlightCurrentSearchMatch();}

function applySearchHighlight(text){if(!logSearchTerm)return text;if(logSearchTerm.startsWith('/')&&logSearchTerm.endsWith('/')){try{return text.replace(new RegExp(logSearchTerm.slice(1,-1),'gi'),match=>`<span class='log-search-highlight'>${match}</span>`);}catch(e){return text;}}const escaped=escapeRegExp(logSearchTerm);return text.replace(new RegExp(escaped,'gi'),match=>`<span class='log-search-highlight'>${match}</span>`);}

function escapeRegExp(str){return str.replace(/[.*+?^${}()|[\]\\]/g,'\\$&');}

function handleLogTimeRange(value){if(value==='all'){logTimeWindow=null;document.getElementById('logCustomStart').value='';document.getElementById('logCustomEnd').value='';renderLogEntries();return;}const minutes=parseInt(value,10);const end=Date.now();const start=end-minutes*60000;logTimeWindow={start,end};renderLogEntries();}

function applyCustomTimeSlice(){const startInput=document.getElementById('logCustomStart');const endInput=document.getElementById('logCustomEnd');const start=startInput?.value?Date.parse(startInput.value):null;const end=endInput?.value?Date.parse(endInput.value):null;if(!start&&!end){logTimeWindow=null;}else{logTimeWindow={start,end};}const rangeSelect=document.getElementById('logTimeRangeSelect');if(rangeSelect)rangeSelect.value='all';renderLogEntries();}

function renderLogLegend(){const container=document.getElementById('logTagLegend');const summary=document.getElementById('logLegendSummary');if(!container)return;const counts={};logBuffer.forEach(entry=>{counts[entry.tag]=(counts[entry.tag]||0)+1;});const entries=Object.entries(counts).sort((a,b)=>b[1]-a[1]);if(entries.length===0){container.innerHTML="<div class='text-xs text-gray-500'>Waiting for logs...</div>";if(summary)summary.textContent='';return;}container.innerHTML=entries.map(([tag,count])=>{const color=resolveLogColor(tag);const disabled=logTagVisibility[tag]===false?'opacity-40':'';return `<button class='log-legend-item ${disabled}' onclick='toggleTagVisibility("${tag}")'><div class='flex items-center'><span class='log-legend-swatch' style='background:${color}'></span>${tag}</div><span>${count}</span></button>`;}).join('');if(summary){summary.textContent=`${entries.length} tags`;} }

function toggleTagVisibility(tag){logTagVisibility[tag]=!(logTagVisibility[tag]===false);renderLogEntries();}

function toggleTagLegend(){const legend=document.querySelector('.log-legend-panel');if(!legend)return;legend.classList.toggle('hidden');}

function renderLogStats(){const strip=document.getElementById('logStatsStrip');if(!strip)return;const total=logBuffer.length;const errors=logBuffer.filter(e=>e.severity==='error').length;const lastError=logBuffer.slice().reverse().find(e=>e.severity==='error');const lastErrorAgo=lastError?describeTimeAgo(lastError.ts):'—';const rate=computeLogRate();const connection=logPollingTimer?'Polling':'Paused';strip.innerHTML=`<span>Total: ${total}</span><span>Errors: ${errors}</span><span>Last error: ${lastErrorAgo}</span><span>Rate: ${rate.toFixed(1)} lines/min</span><span>Poll: ${connection}</span>`;}

function describeTimeAgo(ts){const diff=(Date.now()-ts)/1000;if(diff<1)return'just now';if(diff<60)return`${diff.toFixed(0)}s ago`;const minutes=diff/60;if(minutes<60)return`${minutes.toFixed(0)}m ago`;const hours=minutes/60;if(hours<24)return`${hours.toFixed(0)}h ago`;return`${(hours/24).toFixed(0)}d ago`;}

function computeLogRate(){const now=Date.now();logIngressHistory=logIngressHistory.filter(ts=>now-ts<=600000);return logIngressHistory.length/Math.max((logIngressHistory.length? (now-logIngressHistory[0]):60000)/60000,1);
}

function downloadVisibleLogs(){const logs=getVisibleLogs();if(logs.length===0){showLogToast('No logs to download');return;}const payload=logs.map(e=>`[${e.displayTime}] ${e.tag} ${e.message}`).join('\n');const blob=new Blob([payload],{type:'text/plain'});const url=URL.createObjectURL(blob);const link=document.createElement('a');link.href=url;link.download='kannacloud-logs.txt';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}

async function copyLogShareLink(){const state={group:logActiveGroup,search:logSearchTerm,time:logTimeWindow,hiddenTags:Object.keys(logTagVisibility).filter(tag=>logTagVisibility[tag]===false)};try{const encoded=btoa(JSON.stringify(state));const link=`${window.location.origin}${window.location.pathname}?log=${encoded}`;await navigator.clipboard.writeText(link);showLogToast('Share link copied');}catch(e){console.warn('Share copy failed',e);}}

function setLogBaseline(){if(logBuffer.length===0){showLogToast('No logs yet');return;}logBaselineTimestamp=logBuffer[logBuffer.length-1].ts;showLogToast('Baseline set');renderLogEntries();}

function openLogContext(entryId){const entryIndex=logBuffer.findIndex(e=>e.id===entryId);if(entryIndex<0)return;const context=logBuffer.slice(Math.max(0,entryIndex-10),Math.min(logBuffer.length,entryIndex+11));const body=document.getElementById('logContextBody');const title=document.getElementById('logContextTitle');if(!body||!title)return;title.textContent=`Context around ${logBuffer[entryIndex].tag}`;body.innerHTML=context.map(item=>`<div class='log-context-line ${item.id===entryId?'active':''}'>[${item.displayTime}] ${item.tag} ${escapeHtml(item.message)}</div>`).join('');if(logContextDrawer){logContextDrawer.classList.add('show');logContextDrawer.classList.remove('hidden');}}

function closeLogContext(){if(!logContextDrawer)return;logContextDrawer.classList.remove('show');setTimeout(()=>logContextDrawer.classList.add('hidden'),250);}

function openLogGroupModal(){pendingCustomGroups=JSON.parse(JSON.stringify(customLogGroups||[]));renderLogGroupEditor();document.getElementById('logGroupModal')?.classList.remove('hidden');}

function closeLogGroupModal(){document.getElementById('logGroupModal')?.classList.add('hidden');}

function renderLogGroupEditor(){const editor=document.getElementById('logGroupEditor');if(!editor)return;if(pendingCustomGroups.length===0){pendingCustomGroups.push({key:`custom-${Date.now()}`,label:'New Group',patterns:[]});}
editor.innerHTML=pendingCustomGroups.map((group,index)=>`<div class='log-group-row'><input type='text' value='${group.label||''}' placeholder='Friendly name' data-group-idx='${index}' data-field='label'><input type='text' value='${(group.patterns||[]).join(', ')}' placeholder='Prefixes or /regex/' data-group-idx='${index}' data-field='patterns'><button type='button' class='log-line-button' data-remove-group='${index}' title='Remove'><i class='fas fa-trash'></i></button></div>`).join('');editor.querySelectorAll('input').forEach(input=>{input.addEventListener('input',e=>{const idx=parseInt(e.target.getAttribute('data-group-idx'),10);const field=e.target.getAttribute('data-field');if(field==='label'){pendingCustomGroups[idx].label=e.target.value;}else if(field==='patterns'){pendingCustomGroups[idx].patterns=e.target.value.split(',').map(v=>v.trim()).filter(Boolean);}});});editor.querySelectorAll('[data-remove-group]').forEach(btn=>btn.addEventListener('click',()=>{const idx=parseInt(btn.getAttribute('data-remove-group'),10);pendingCustomGroups.splice(idx,1);renderLogGroupEditor();}));}

function addCustomLogGroup(){pendingCustomGroups.push({key:`custom-${Date.now()}`,label:'New Group',patterns:[]});renderLogGroupEditor();}

function saveCustomLogGroups(){customLogGroups=pendingCustomGroups.filter(group=>group.label&&group.patterns&&group.patterns.length>0);localStorage.setItem('logCustomGroups',JSON.stringify(customLogGroups));buildLogGroupFilters();renderLogEntries();closeLogGroupModal();}

function openLogAlertModal(){document.getElementById('logAlertModal')?.classList.remove('hidden');renderLogAlertEditor();}

function closeLogAlertModal(){document.getElementById('logAlertModal')?.classList.add('hidden');}

function renderLogAlertEditor(){const editor=document.getElementById('logAlertEditor');if(!editor)return;if(logAlertRules.length===0){logAlertRules.push({id:`alert-${Date.now()}`,label:'WiFi Failures',pattern:'/fail/i'});}editor.innerHTML=logAlertRules.map((rule,index)=>`<div class='log-alert-row'><input type='text' value='${rule.label||''}' placeholder='Label' data-alert-idx='${index}' data-field='label'><input type='text' value='${rule.pattern||''}' placeholder='Pattern' data-alert-idx='${index}' data-field='pattern'><button type='button' class='log-line-button' data-remove-alert='${index}' title='Remove'><i class='fas fa-trash'></i></button></div>`).join('');editor.querySelectorAll('input').forEach(input=>{input.addEventListener('input',e=>{const idx=parseInt(e.target.getAttribute('data-alert-idx'),10);const field=e.target.getAttribute('data-field');logAlertRules[idx][field]=e.target.value;});});editor.querySelectorAll('[data-remove-alert]').forEach(btn=>btn.addEventListener('click',()=>{const idx=parseInt(btn.getAttribute('data-remove-alert'),10);logAlertRules.splice(idx,1);renderLogAlertEditor();}));}

function addLogAlertRule(){logAlertRules.push({id:`alert-${Date.now()}`,label:'New Rule',pattern:''});renderLogAlertEditor();}

function saveLogAlertRules(){logAlertRules=logAlertRules.filter(rule=>rule.pattern);localStorage.setItem('logAlertRules',JSON.stringify(logAlertRules));closeLogAlertModal();}

function evaluateAlertRules(entry){if(!Array.isArray(logAlertRules))return;logAlertRules.forEach(rule=>{if(!rule.pattern)return;const matches=rule.pattern.startsWith('/')&&rule.pattern.endsWith('/')?new RegExp(rule.pattern.slice(1,-1),'i').test(entry.message):entry.message.toLowerCase().includes(rule.pattern.toLowerCase());if(matches){triggerLogAlert(rule,entry);}});}

function triggerLogAlert(rule,entry){const container=document.getElementById('logAlertsContainer');if(!container)return;const toast=document.createElement('div');toast.className='log-alert-toast';toast.textContent=`${rule.label||'Alert'} • ${entry.tag}: ${entry.message}`;container.appendChild(toast);setTimeout(()=>toast.remove(),4000);const line=document.querySelector(`.log-line[data-entry-id="${entry.id}"]`);if(line){line.classList.add('alert-hit');setTimeout(()=>line.classList.remove('alert-hit'),1500);}}

function showLogToast(message){const container=document.getElementById('logAlertsContainer');if(!container)return;const toast=document.createElement('div');toast.className='log-alert-toast';toast.textContent=message;container.appendChild(toast);setTimeout(()=>toast.remove(),3000);}

function escapeHtml(value){return (value??'').toString().replace(/[&<>"']/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;','\'':'&#39;'}[char]||char));}

// Manual Mode Terminal Session Management
let manualTerminalSession = [];
let selectedSensorAddress = null;  // Track currently selected sensor

function appendToTerminal(text, type = 'output') {
  const terminal = document.getElementById('manualTerminal');
  if (!terminal) return;
  
  const timestamp = new Date().toLocaleTimeString();
  const line = document.createElement('div');
  
  if (type === 'command') {
    line.className = 'text-white mt-2';
    line.style.whiteSpace = 'pre-wrap';
    line.textContent = `[${timestamp}] > ${text}`;
  } else if (type === 'response') {
    line.className = 'text-green-400';
    line.style.whiteSpace = 'pre-wrap';
    line.textContent = `< ${text}`;
  } else if (type === 'error') {
    line.className = 'text-red-400';
    line.style.whiteSpace = 'pre-wrap';
    line.textContent = `ERROR: ${text}`;
  } else if (type === 'info') {
    line.className = 'text-cyan-400';
    line.style.whiteSpace = 'pre-wrap';
    line.textContent = `ℹ ${text}`;
  } else {
    line.className = 'text-gray-400';
    line.style.whiteSpace = 'pre-wrap';
    line.textContent = text;
  }
  
  terminal.appendChild(line);
  manualTerminalSession.push({ timestamp, text, type });
  terminal.scrollTop = terminal.scrollHeight;
}

async function sendManualModeCommand() {
  const input = document.getElementById('manualModeInput');
  const command = (input?.value || '').trim();
  
  if (!command) {
    appendToTerminal('Please enter a command', 'error');
    return;
  }
  
  // Handle local 'clear' command
  if (command.toLowerCase() === 'clear') {
    input.value = '';
    clearManualTerminal();
    return;
  }
  
  appendToTerminal(command, 'command');
  input.value = '';
  
  try {
    // Build request payload
    const payload = { command };
    
    // Add selected address for commands that need it
    // - Regular commands (not starting with !)
    // - !cmd command (needs to know which sensor)
    if (!command.startsWith('!') || command === '!cmd') {
      const isNumber = /^\d+$/.test(command);
      if (!isNumber && selectedSensorAddress !== null) {
        payload.address = selectedSensorAddress;
      }
    }
    
    const res = await fetch('/api/manual-command', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    
    if (!res.ok) {
      const err = await res.text();
      throw new Error(err || 'Command failed');
    }
    
    const data = await res.json();
    const response = data.response || data.result || 'Command executed successfully';
    
    // Check if this was a successful sensor selection
    if (data.selected_address !== undefined && data.status === 'success') {
      selectedSensorAddress = data.selected_address;
      appendToTerminal(`Sensor selected: Address ${selectedSensorAddress}`, 'info');
      updateSelectedSensorStatus(data.sensor_type, data.selected_address);
    }
    
    appendToTerminal(response, data.status === 'error' ? 'error' : 'response');
  } catch (e) {
    appendToTerminal(e.message, 'error');
    console.error('Manual command failed:', e);
  }
}

function clearManualTerminal() {
  const terminal = document.getElementById('manualTerminal');
  if (!terminal) return;
  
  if (manualTerminalSession.length > 3 && !confirm('Clear the terminal session history?')) {
    return;
  }
  
  terminal.innerHTML = `
    <div class='text-gray-500'>════════════════════════════════════════════════════════</div>
    <div class='text-green-400 font-bold'>  KannaCloud EZO Console - Manual Command Mode</div>
    <div class='text-gray-500'>════════════════════════════════════════════════════════</div>
    <div class='text-gray-400 mt-2'>Welcome! Use this console to interact with your EZO sensors.</div>
    <div class='text-gray-400'>Type commands below and press Enter or click Send.</div>
    <div class='text-gray-500 mt-2'>───────────────────────────────────────────────────────</div>
    <div class='text-cyan-400 mt-2'>Quick Start Commands:</div>
    <div class='text-white'>  !help      <span class='text-gray-500'>- Show all available commands</span></div>
    <div class='text-white'>  !scan      <span class='text-gray-500'>- Find all connected sensors</span></div>
    <div class='text-white'>  &lt;address&gt;  <span class='text-gray-500'>- Select sensor (e.g., 99, 100, 101)</span></div>
    <div class='text-white'>  !cmd       <span class='text-gray-500'>- Show commands for selected sensor</span></div>
    <div class='text-white'>  R          <span class='text-gray-500'>- Read current sensor value</span></div>
    <div class='text-white'>  I          <span class='text-gray-500'>- Get sensor information</span></div>
    <div class='text-gray-500 mt-2'>───────────────────────────────────────────────────────</div>
    <div class='text-yellow-400 mt-2'>💡 Tip: Start with "!scan" to discover your sensors</div>
    <div class='text-gray-500 mt-1'>════════════════════════════════════════════════════════</div>
  `;
  manualTerminalSession = [];
  selectedSensorAddress = null;  // Clear selected sensor
  updateSelectedSensorStatus(null, null);  // Hide status indicator
}

function updateSelectedSensorStatus(sensorType, address) {
  const statusDiv = document.getElementById('selectedSensorStatus');
  const infoSpan = document.getElementById('selectedSensorInfo');
  
  if (!statusDiv || !infoSpan) return;
  
  if (address !== null && sensorType) {
    statusDiv.style.display = 'block';
    infoSpan.textContent = `EZO-${sensorType} @ Address ${address} (0x${address.toString(16).toUpperCase().padStart(2, '0')})`;
  } else {
    statusDiv.style.display = 'none';
    infoSpan.textContent = 'No sensor selected';
  }
}

// Clear session on window close
window.addEventListener('beforeunload', () => {
  manualTerminalSession = [];
  selectedSensorAddress = null;
});

window.onload=()=>{initializeDashboard();};
