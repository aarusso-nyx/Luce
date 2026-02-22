
// script.js: Enhanced UI with WebSocket and REST /api integration
// Single-page WebUI script
// Uses async/await and helper functions for clarity
(async function init() {
  const relaysContainer = document.getElementById('relays');
  const dayStatusEl     = document.getElementById('day-status');
  const clockEl         = document.getElementById('clock');
  const nameInput       = document.getElementById('name');
  const ssidInput       = document.getElementById('ssid');
  const passInput       = document.getElementById('pass');
  const mqttSelect      = document.getElementById('mqtt-select');
  const mqttInput       = document.getElementById('mqtt');
  const configForm      = document.getElementById('config-form');
  const saveBtn         = configForm.querySelector('button[type=submit]');
  const restartBtn      = document.getElementById('restart');
  // Disable buttons until changes occur
  saveBtn.disabled = true;
  restartBtn.disabled = true;

  // Sensor/threshold controls
  const thresholdSlider   = document.getElementById('threshold-slider');
  const thresholdValue    = document.getElementById('threshold-value');
  const sensorValue       = document.getElementById('sensor-value');
  const voltageValue      = document.getElementById('voltage-value');
  const temperatureValue  = document.getElementById('temperature-value');
  const humidityValue     = document.getElementById('humidity-value');
  // Initialize slider change events
  if (thresholdSlider) {
    thresholdSlider.oninput = () => { thresholdValue.textContent = thresholdSlider.value; };
    thresholdSlider.onchange = () => {
      fetch(`/sensor?threshold=${thresholdSlider.value}`, { method: 'PUT' });
    };
  }
  // WebSocket for real-time updates
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onmessage = msg => {
    let data;
    try {
      data = JSON.parse(msg.data);
    } catch (e) {
      // Plain log line
      appendLog(msg.data);
      return;
    }
    if (data.type === 'log') {
      appendLog(data.message);
      return;
    }
    // Real-time clock from epoch tstamp
    if (data.tstamp !== undefined) {
      const dt = new Date(data.tstamp * 1000);
      clockEl.textContent = dt.toLocaleTimeString();
    }
    if (data.state !== undefined && data.night !== undefined && data.day !== undefined) {
      renderRelays(data.state, data.night);
      renderDay(data.day);
    }
    // Update sensor and threshold from live data
    if (data.light !== undefined && data.voltage !== undefined) {
      if (thresholdSlider) {
        thresholdSlider.value = data.light;
        thresholdValue.textContent  = data.light;
      }
      sensorValue.textContent     = data.light;
      voltageValue.textContent    = data.voltage;
      temperatureValue.textContent = data.temperature;
      humidityValue.textContent    = data.humidity;
      // Update dashboard charts
      updateCharts(data.light, data.voltage, data.temperature, data.humidity);
    }
    else if (data.id !== undefined) {
      updateRelay(data.id, data.state, data.night);
      if (data.day !== undefined) renderDay(data.day);
    }
  };

  // Fetch initial relay state
  try {
    const relaysRes = await fetch('/relays');
    const relaysData = await relaysRes.json();
    renderRelays(relaysData.state, relaysData.night);
    renderDay(relaysData.day);
  } catch (e) { console.error('Error loading relays:', e); }
  // Load initial configuration and track changes
  const initialConfig = {};
  // Load initial configuration
  try {
    const cfgRes = await fetch('/config');
    const cfg = await cfgRes.json();
    nameInput.value = cfg.name || '';
    ssidInput.value = cfg.ssid || '';
    passInput.value = '';
    mqttInput.value = cfg.mqtt || '';
    initialConfig.name = cfg.name || '';
    initialConfig.ssid = cfg.ssid || '';
    initialConfig.pass = '';
    initialConfig.mqtt = cfg.mqtt || '';
  } catch (e) { console.error('Error loading config:', e); }
  // Fetch initial sensor data
  // Load initial sensor readings
  try {
    const sensRes = await fetch('/sensor');
    const sens = await sensRes.json();
    if (thresholdSlider) {
      thresholdSlider.value = sens.threshold;
      thresholdValue.textContent = sens.threshold;
    }
    sensorValue.textContent = sens.value;
    renderDay(sens.value < sens.threshold ? 1 : 0);
  } catch (e) { console.error('Error loading sensor:', e); }

  // Render relays grid (4x2)
  function renderRelays(stateMask, nightMask) {
    relaysContainer.innerHTML = '';
    for (let i = 0; i < 8; i++) {
      const on    = ((stateMask >> i)&1) === 1;
      const nmask = ((nightMask>>i)&1) === 1;
      const item = document.createElement('div'); item.className = 'grid-item';
      item.innerHTML = `
        <div class="title">Relay ${i}</div>
        <div class="switch-row">
          <label>On</label>
          <label class="switch">
            <input type="checkbox" class="relay-switch" data-idx="${i}" ${on?'checked':''}>
            <span class="slider"></span>
          </label>
        </div>
        <div class="switch-row">
          <label>Night</label>
          <label class="switch">
            <input type="checkbox" class="night-switch" data-idx="${i}" ${nmask?'checked':''}>
            <span class="slider"></span>
          </label>
        </div>
      `;
      relaysContainer.appendChild(item);
    }
    // Bind events
    relaysContainer.querySelectorAll('.relay-switch').forEach(el => {
      el.onchange = () => {
        const idx = el.dataset.idx;
        const val = el.checked ? 1 : 0;
        fetch(`/relays/${idx}?state=${val}`, {method:'PUT'})
          .then(r => r.json())
          .then(d => updateRelay(idx, d.state, d.night));
      };
    });
    relaysContainer.querySelectorAll('.night-switch').forEach(el => {
      el.onchange = () => {
        const idx = el.dataset.idx;
        const val = el.checked ? 1 : 0;
        fetch(`/relays/${idx}?night=${val}`, {method:'PUT'})
          .then(r => r.json())
          .then(d => updateRelay(idx, d.state, d.night));
      };
    });
  }

  // Update single relay
  function updateRelay(idx, state, night) {
    const elOn = relaysContainer.querySelector(`.relay-switch[data-idx=\"${idx}\"]`);
    const elNi = relaysContainer.querySelector(`.night-switch[data-idx=\"${idx}\"]`);
    if (elOn)  elOn.checked  = state === 1;
    if (elNi)  elNi.checked  = night === 1;
  }

  // Display day/night status
  function renderDay(day) {
    if (day !== undefined) {
      dayStatusEl.textContent = day? 'Day' : 'Night';
      dayStatusEl.className   = day? 'status day' : 'status night';
    }
  }

  // Config form submit
  configForm.onsubmit = async e => {
    e.preventDefault();
    const actions = [];
    // Determine MQTT value
    const mqttVal = mqttSelect.value === 'custom' ? mqttInput.value : mqttSelect.value;
    // Name
    if (nameInput.value !== initialConfig.name) {
      actions.push(fetch(`/config/name?name=${encodeURIComponent(nameInput.value)}`, {method:'PUT'}));
    }
    // SSID
    if (ssidInput.value !== initialConfig.ssid) {
      actions.push(fetch(`/config/ssid?ssid=${encodeURIComponent(ssidInput.value)}`, {method:'PUT'}));
    }
    // Password (only if changed)
    if (passInput.value) {
      actions.push(fetch(`/config/pass?pass=${encodeURIComponent(passInput.value)}`, {method:'PUT'}));
    }
    // MQTT
    if (mqttVal !== initialConfig.mqtt) {
      actions.push(fetch(`/config/mqtt?mqtt=${encodeURIComponent(mqttVal)}`, {method:'PUT'}));
    }
    if (actions.length === 0) {
      alert('No changes to save');
      return;
    }
    try {
      await Promise.all(actions);
      initialConfig.name = nameInput.value;
      initialConfig.ssid = ssidInput.value;
      initialConfig.pass = '';
      initialConfig.mqtt = mqttVal;
      saveBtn.disabled = true;
      restartBtn.disabled = false;
      alert('Config saved. Click Restart to apply.');
    } catch (e) {
      console.error('Error saving config:', e);
      alert('Error saving config');
    }
  };

  // Restart button
  restartBtn.onclick = () => {
    if (confirm('Restart device now?')) {
      fetch('/config/restart', {method:'POST'});
    }
  };

  // Enable Save when form inputs change
  const markDirty = () => { saveBtn.disabled = false; };
  [nameInput, ssidInput, passInput, mqttInput].forEach(el => {
    el.addEventListener('input', markDirty);
  });
  mqttSelect.addEventListener('change', () => {
    if (mqttSelect.value === 'custom') mqttInput.style.display = 'block';
    else {
      mqttInput.style.display = 'none'; mqttInput.value = '';
    }
    markDirty();
  });
  // Handle MQTT select
  mqttSelect.onchange = () => {
    if (mqttSelect.value === 'custom') {
      mqttInput.style.display = 'block';
    } else {
      mqttInput.style.display = 'none';
      mqttInput.value = '';
    }
    saveBtn.disabled = false;
  };

  // === Diagnostics actions ===
  const diagBootBtn    = document.getElementById('diag-boot');
  const diagRuntimeBtn = document.getElementById('diag-runtime');
  const diagMinimalBtn = document.getElementById('diag-minimal');
  const diagOutput     = document.getElementById('diag-output');
  function runDiag(endpoint) {
    diagOutput.textContent = 'Loading...';
    fetch(endpoint)
      .then(r => r.text())
      .then(txt => { diagOutput.textContent = txt; })
      .catch(err => { diagOutput.textContent = 'Error: ' + err; });
  }
  diagBootBtn.onclick    = () => runDiag('/api/diag/boot');
  diagRuntimeBtn.onclick = () => runDiag('/api/diag/runtime');
  diagMinimalBtn.onclick = () => runDiag('/api/diag/minimal');

  // === Logs viewer ===
  const logsContainer = document.getElementById('logs');
  const logsLimit     = document.getElementById('logs-limit');
  const logsRefresh   = document.getElementById('logs-refresh');
  function appendLog(line) {
    const div = document.createElement('div');
    div.textContent = line;
    logsContainer.appendChild(div);
    logsContainer.scrollTop = logsContainer.scrollHeight;
  }
  function loadLogs() {
    logsContainer.textContent = 'Loading logs...';
    const limit = parseInt(logsLimit.value) || 200;
    fetch(`/api/logs?limit=${limit}`)
      .then(r => r.json())
      .then(d => {
        logsContainer.textContent = '';
        d.logs.forEach(line => appendLog(line));
      })
      .catch(err => { logsContainer.textContent = 'Error: ' + err; });
  }
  logsRefresh.onclick = loadLogs;
  // Load initial logs
  loadLogs();
  // Fetch firmware version
  fetch('/api/version')
    .then(r => r.json())
    .then(d => { document.getElementById('version').textContent = 'v' + d.version; })
    .catch(console.error);

  // === Dashboard charts ===
  const historyLimit = 50;
  const labels = [];
  const lightData = [], voltageData = [], tempData = [], humidityData = [];
  function createChart(ctxId, label, color, dataArr) {
    const ctx = document.getElementById(ctxId).getContext('2d');
    return new Chart(ctx, {
      type: 'line',
      data: { labels: labels, datasets: [{ label: label, data: dataArr, borderColor: color, fill: false }] },
      options: { responsive: true, maintainAspectRatio: false, scales: { x: { display: true }, y: { display: true } } }
    });
  }
  const chartLight       = createChart('chart-light', 'Light',       '#ff9800', lightData);
  const chartVoltage     = createChart('chart-voltage', 'Voltage',   '#3f51b5', voltageData);
  const chartTemperature = createChart('chart-temperature','Temperature','#e91e63', tempData);
  const chartHumidity    = createChart('chart-humidity','Humidity',    '#009688', humidityData);
  function updateCharts(ldr, voltage, temp, hum) {
    const nowLabel = new Date().toLocaleTimeString();
    labels.push(nowLabel);
    lightData.push(ldr);
    voltageData.push(voltage);
    tempData.push(temp);
    humidityData.push(hum);
    if (labels.length > historyLimit) {
      labels.shift(); lightData.shift(); voltageData.shift(); tempData.shift(); humidityData.shift();
    }
    chartLight.update(); chartVoltage.update(); chartTemperature.update(); chartHumidity.update();
  }
})();