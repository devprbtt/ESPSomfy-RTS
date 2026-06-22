#!/usr/bin/env python3
"""Local operator UI for discovering Gree HVACs and deploying the Savant gateway.

Run on the MacBook, open a browser, scan/manual-enter HVAC units, generate the
config, and deploy the Savant helper in one flow.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

import gree_scan_and_build_json as gree_scan


APP_VERSION = "0.1.0"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
DEFAULT_REMOTE_DIR = "/data/codex_probe"
DEFAULT_LISTEN_PORT = 2323


HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Savant Gree Operator</title>
  <style>
    :root {
      --bg: #f5efe4;
      --panel: rgba(255,255,255,0.78);
      --panel-strong: rgba(255,255,255,0.94);
      --ink: #1d2b24;
      --muted: #5a675f;
      --line: rgba(29,43,36,0.12);
      --accent: #116149;
      --accent-2: #d96b2b;
      --danger: #9f2f2f;
      --ok: #1e7c4d;
      --shadow: 0 18px 40px rgba(45, 35, 23, 0.12);
      --radius: 22px;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--ink);
      font-family: "Avenir Next", "Segoe UI", sans-serif;
      background:
        radial-gradient(circle at top left, rgba(217,107,43,0.18), transparent 28%),
        radial-gradient(circle at top right, rgba(17,97,73,0.18), transparent 26%),
        linear-gradient(180deg, #f7f1e7 0%, #efe4d3 100%);
      min-height: 100vh;
    }
    .shell {
      max-width: 1280px;
      margin: 0 auto;
      padding: 28px;
    }
    .hero {
      display: grid;
      grid-template-columns: 1.4fr 1fr;
      gap: 18px;
      margin-bottom: 20px;
    }
    .card {
      background: var(--panel);
      backdrop-filter: blur(14px);
      border: 1px solid rgba(255,255,255,0.55);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
    }
    .hero-main {
      padding: 28px;
      position: relative;
      overflow: hidden;
    }
    .hero-main:before {
      content: "";
      position: absolute;
      inset: auto -80px -90px auto;
      width: 220px;
      height: 220px;
      border-radius: 999px;
      background: radial-gradient(circle, rgba(17,97,73,0.28), transparent 70%);
      pointer-events: none;
    }
    h1 {
      margin: 0 0 10px;
      font-size: clamp(2rem, 5vw, 3.4rem);
      line-height: 0.95;
      letter-spacing: -0.05em;
    }
    .subtitle {
      color: var(--muted);
      font-size: 1.02rem;
      max-width: 60ch;
      line-height: 1.55;
    }
    .hero-side {
      padding: 24px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      gap: 14px;
    }
    .pillrow {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
    }
    .pill {
      padding: 10px 12px;
      border-radius: 999px;
      background: rgba(17,97,73,0.1);
      color: var(--accent);
      font-size: 0.9rem;
      font-weight: 600;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(12, minmax(0,1fr));
      gap: 18px;
    }
    .span-7 { grid-column: span 7; }
    .span-5 { grid-column: span 5; }
    .span-12 { grid-column: span 12; }
    .section {
      padding: 22px;
    }
    .section h2 {
      margin: 0 0 6px;
      font-size: 1.15rem;
      letter-spacing: -0.02em;
    }
    .section p {
      margin: 0 0 18px;
      color: var(--muted);
      line-height: 1.45;
    }
    .formgrid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0,1fr));
      gap: 12px;
    }
    .field {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .field.full { grid-column: 1 / -1; }
    label {
      font-size: 0.84rem;
      text-transform: uppercase;
      letter-spacing: 0.06em;
      color: var(--muted);
      font-weight: 700;
    }
    input, textarea, select {
      width: 100%;
      border: 1px solid var(--line);
      background: var(--panel-strong);
      border-radius: 14px;
      padding: 12px 13px;
      color: var(--ink);
      font: inherit;
      outline: none;
      transition: border-color .18s ease, transform .18s ease, box-shadow .18s ease;
    }
    textarea { min-height: 110px; resize: vertical; }
    input:focus, textarea:focus, select:focus {
      border-color: rgba(17,97,73,0.55);
      box-shadow: 0 0 0 4px rgba(17,97,73,0.08);
      transform: translateY(-1px);
    }
    .actions {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      margin-top: 16px;
    }
    button {
      border: 0;
      border-radius: 999px;
      padding: 12px 16px;
      font: inherit;
      font-weight: 700;
      cursor: pointer;
      transition: transform .18s ease, opacity .18s ease, box-shadow .18s ease;
    }
    button:hover { transform: translateY(-1px); }
    button:disabled { opacity: 0.55; cursor: default; transform: none; }
    .primary {
      background: linear-gradient(135deg, var(--accent), #148865);
      color: white;
      box-shadow: 0 12px 28px rgba(17,97,73,0.28);
    }
    .secondary {
      background: rgba(17,97,73,0.1);
      color: var(--accent);
    }
    .ghost {
      background: rgba(29,43,36,0.06);
      color: var(--ink);
    }
    .warn {
      background: rgba(217,107,43,0.12);
      color: var(--accent-2);
    }
    .danger {
      background: rgba(159,47,47,0.12);
      color: var(--danger);
    }
    .list {
      display: grid;
      gap: 12px;
    }
    .hvac {
      padding: 16px;
      border: 1px solid var(--line);
      border-radius: 18px;
      background: rgba(255,255,255,0.66);
    }
    .hvac-top {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 14px;
      margin-bottom: 12px;
    }
    .hvac-name {
      font-size: 1.02rem;
      font-weight: 800;
      letter-spacing: -0.02em;
    }
    .meta {
      color: var(--muted);
      font-size: 0.9rem;
    }
    .status {
      padding: 14px 16px;
      border-radius: 18px;
      margin-top: 16px;
      background: rgba(29,43,36,0.05);
      border: 1px solid var(--line);
      white-space: pre-wrap;
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-size: 0.88rem;
      line-height: 1.45;
      min-height: 120px;
    }
    .banner {
      margin-top: 14px;
      padding: 12px 14px;
      border-radius: 14px;
      font-weight: 700;
      display: none;
    }
    .banner.show { display: block; }
    .banner.ok { background: rgba(30,124,77,0.12); color: var(--ok); }
    .banner.error { background: rgba(159,47,47,0.12); color: var(--danger); }
    .banner.info { background: rgba(17,97,73,0.1); color: var(--accent); }
    .overlay {
      position: fixed;
      inset: 0;
      display: none;
      align-items: center;
      justify-content: center;
      background: rgba(28, 32, 26, 0.34);
      backdrop-filter: blur(6px);
      z-index: 20;
    }
    .overlay.show { display: flex; }
    .modal {
      width: min(560px, calc(100vw - 32px));
      background: #fffaf3;
      border-radius: 24px;
      padding: 24px;
      box-shadow: 0 28px 70px rgba(0,0,0,0.18);
      border: 1px solid rgba(29,43,36,0.1);
    }
    .spinner {
      width: 44px;
      height: 44px;
      border-radius: 50%;
      border: 4px solid rgba(17,97,73,0.14);
      border-top-color: var(--accent);
      animation: spin 1s linear infinite;
      margin-bottom: 14px;
    }
    .modal h3 { margin: 0 0 8px; font-size: 1.2rem; }
    .modal p { margin: 0; color: var(--muted); line-height: 1.5; }
    .jsonbox {
      background: #1b2722;
      color: #f2f3ef;
      border-radius: 18px;
      padding: 18px;
      white-space: pre-wrap;
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      font-size: 0.82rem;
      overflow: auto;
      max-height: 340px;
    }
    @keyframes spin { to { transform: rotate(360deg); } }
    @media (max-width: 980px) {
      .hero, .grid { grid-template-columns: 1fr; }
      .span-7, .span-5, .span-12 { grid-column: auto; }
    }
    @media (max-width: 640px) {
      .shell { padding: 16px; }
      .formgrid { grid-template-columns: 1fr; }
      h1 { font-size: 2.3rem; }
    }
  </style>
</head>
<body>
  <div class="overlay" id="overlay">
    <div class="modal">
      <div class="spinner"></div>
      <h3 id="overlayTitle">Working</h3>
      <p id="overlayText">Please wait while the operator runs the requested action.</p>
    </div>
  </div>
  <div class="shell">
    <div class="hero">
      <div class="card hero-main">
        <div class="pillrow">
          <div class="pill">Local Scanner</div>
          <div class="pill">Savant Deployer</div>
          <div class="pill">Version """ + APP_VERSION + """</div>
        </div>
        <h1>Savant Gree Operator</h1>
        <div class="subtitle">
          Discover Gree HVACs, fill in anything discovery could not determine, generate the
          Savant gateway config, and deploy everything to the Savant host from one place.
        </div>
      </div>
      <div class="card hero-side">
        <div>
          <h2>Flow</h2>
          <p>1. Scan or add units manually. 2. Review thermostat IDs and names. 3. Enter Savant host credentials. 4. Deploy and verify.</p>
        </div>
        <div class="pillrow">
          <div class="pill">Auto discovery</div>
          <div class="pill">Manual fallback</div>
          <div class="pill">Config preview</div>
        </div>
      </div>
    </div>

    <div class="grid">
      <div class="card section span-7">
        <h2>Discovery</h2>
        <p>Scan a subnet or probe known addresses. If a unit does not appear, add it manually below.</p>
        <div class="formgrid">
          <div class="field">
            <label for="subnet">Subnet</label>
            <input id="subnet" placeholder="192.168.5.0/24">
          </div>
          <div class="field">
            <label for="hosts">Specific Hosts</label>
            <input id="hosts" placeholder="192.168.5.62, 192.168.5.63">
          </div>
          <div class="field">
            <label for="port">UDP Port</label>
            <input id="port" value="7000">
          </div>
          <div class="field">
            <label for="workers">Workers</label>
            <input id="workers" value="48">
          </div>
          <div class="field">
            <label for="timeout">Timeout</label>
            <input id="timeout" value="1.5">
          </div>
          <div class="field">
            <label for="startId">Start Thermostat ID</label>
            <input id="startId" value="1">
          </div>
        </div>
        <div class="actions">
          <button class="primary" id="scanBtn">Scan For Gree HVACs</button>
          <button class="secondary" id="manualBtn">Add Manual HVAC</button>
          <button class="ghost" id="previewBtn">Preview Config JSON</button>
        </div>
        <div class="banner info" id="scanBanner"></div>
      </div>

      <div class="card section span-5">
        <h2>Savant Host</h2>
        <p>Provide the Savant Linux host credentials and where the helper should be deployed.</p>
        <div class="formgrid">
          <div class="field">
            <label for="savantHost">Host IP</label>
            <input id="savantHost" placeholder="192.168.51.213">
          </div>
          <div class="field">
            <label for="savantUser">User</label>
            <input id="savantUser" value="RPM">
          </div>
          <div class="field">
            <label for="savantPass">Password</label>
            <input id="savantPass" type="password" placeholder="RPM">
          </div>
          <div class="field">
            <label for="remoteDir">Remote Dir</label>
            <input id="remoteDir" value="/data/codex_probe">
          </div>
          <div class="field">
            <label for="listenPort">Gateway Port</label>
            <input id="listenPort" value="2323">
          </div>
          <div class="field">
            <label for="listenHost">Listen Host</label>
            <input id="listenHost" value="0.0.0.0">
          </div>
        </div>
        <div class="actions">
          <button class="primary" id="deployBtn">Deploy To Savant</button>
        </div>
        <div class="banner" id="deployBanner"></div>
      </div>

      <div class="card section span-12">
        <h2>HVAC Inventory</h2>
        <p>Review auto-discovered devices, rename them, adjust thermostat IDs, or add fully manual units when discovery misses them.</p>
        <div class="list" id="hvacList"></div>
      </div>

      <div class="card section span-12">
        <h2>Config Preview</h2>
        <p>This is the exact config the deployment step will send to the Savant host.</p>
        <div class="jsonbox" id="configPreview">{}</div>
      </div>

      <div class="card section span-12">
        <h2>Activity</h2>
        <p>Successes, failures, and deployment output will appear here.</p>
        <div class="status" id="logBox">Ready.</div>
      </div>
    </div>
  </div>
  <script>
    const state = { hvacs: [] };

    function log(message) {
      const box = document.getElementById('logBox');
      const stamp = new Date().toLocaleTimeString();
      box.textContent += "\\n[" + stamp + "] " + message;
      box.scrollTop = box.scrollHeight;
    }

    function showBanner(id, kind, message) {
      const el = document.getElementById(id);
      el.className = "banner show " + kind;
      el.textContent = message;
    }

    function hideBanner(id) {
      const el = document.getElementById(id);
      el.className = "banner";
      el.textContent = "";
    }

    function showOverlay(title, text) {
      document.getElementById('overlayTitle').textContent = title;
      document.getElementById('overlayText').textContent = text;
      document.getElementById('overlay').classList.add('show');
    }

    function hideOverlay() {
      document.getElementById('overlay').classList.remove('show');
    }

    function nextThermostatId() {
      let ids = state.hvacs.map(item => Number(item.thermostat_id) || 0);
      return ids.length ? Math.max(...ids) + 1 : Number(document.getElementById('startId').value || 1);
    }

    function makeManualHvac() {
      return {
        thermostat_id: nextThermostatId(),
        name: "Manual Gree HVAC " + nextThermostatId(),
        host: "",
        port: 7000,
        mac: "",
        encryption_version: 2,
        key: "",
        current_temp: 25,
        setpoint: 24,
        fan: "FANAUTO",
        mode: "OFF",
        power: false,
        source: "manual",
        status_decoded: {}
      };
    }

    function currentConfig() {
      const listenPort = Number(document.getElementById('listenPort').value || 2323);
      const listenHost = document.getElementById('listenHost').value || "0.0.0.0";
      return {
        listen_host: listenHost,
        listen_port: listenPort,
        backend: "gree_local",
        devices: state.hvacs.map(item => ({
          thermostat_id: Number(item.thermostat_id),
          name: item.name || ("Gree HVAC " + item.thermostat_id),
          mode: item.mode || "OFF",
          power: Boolean(item.power),
          setpoint: Number(item.setpoint || 24),
          current_temp: Number(item.current_temp || 25),
          fan: item.fan || "FANAUTO"
        })),
        gree_local: {
          note: "Generated by Savant Gree Operator UI",
          timeout: 2.5,
          devices: state.hvacs.map(item => ({
            thermostat_id: Number(item.thermostat_id),
            host: item.host,
            port: Number(item.port || 7000),
            mac: item.mac,
            encryption_version: Number(item.encryption_version || 2),
            key: item.key
          }))
        }
      };
    }

    function updatePreview() {
      document.getElementById('configPreview').textContent = JSON.stringify(currentConfig(), null, 2);
    }

    function bindField(input, hvac, key, transform = (v) => v) {
      input.addEventListener('input', () => {
        hvac[key] = transform(input.value);
        updatePreview();
      });
    }

    function renderHvacs() {
      const root = document.getElementById('hvacList');
      root.innerHTML = '';
      if (!state.hvacs.length) {
        const empty = document.createElement('div');
        empty.className = 'meta';
        empty.textContent = 'No HVAC entries yet. Scan the subnet or add one manually.';
        root.appendChild(empty);
        updatePreview();
        return;
      }

      state.hvacs.sort((a, b) => Number(a.thermostat_id) - Number(b.thermostat_id));
      state.hvacs.forEach((hvac, index) => {
        const card = document.createElement('div');
        card.className = 'hvac';

        const top = document.createElement('div');
        top.className = 'hvac-top';
        top.innerHTML = `
          <div>
            <div class="hvac-name">${hvac.name || ('Gree HVAC ' + hvac.thermostat_id)}</div>
            <div class="meta">${hvac.source === 'scan' ? 'Auto-discovered' : 'Manual'} · ${hvac.host || 'No IP yet'} · MAC ${hvac.mac || 'missing'}</div>
          </div>
        `;

        const del = document.createElement('button');
        del.className = 'danger';
        del.textContent = 'Delete';
        del.onclick = () => {
          state.hvacs.splice(index, 1);
          renderHvacs();
        };
        top.appendChild(del);
        card.appendChild(top);

        const form = document.createElement('div');
        form.className = 'formgrid';
        form.innerHTML = `
          <div class="field"><label>Name</label><input data-k="name" value="${hvac.name || ''}"></div>
          <div class="field"><label>Thermostat ID</label><input data-k="thermostat_id" value="${hvac.thermostat_id || ''}"></div>
          <div class="field"><label>HVAC IP</label><input data-k="host" value="${hvac.host || ''}"></div>
          <div class="field"><label>UDP Port</label><input data-k="port" value="${hvac.port || 7000}"></div>
          <div class="field"><label>MAC</label><input data-k="mac" value="${hvac.mac || ''}"></div>
          <div class="field"><label>Encryption Version</label><select data-k="encryption_version"><option value="1">1</option><option value="2">2</option></select></div>
          <div class="field full"><label>Key</label><input data-k="key" value="${hvac.key || ''}"></div>
          <div class="field"><label>Current Temp</label><input data-k="current_temp" value="${hvac.current_temp || 25}"></div>
          <div class="field"><label>Default Setpoint</label><input data-k="setpoint" value="${hvac.setpoint || 24}"></div>
        `;
        card.appendChild(form);

        form.querySelector('[data-k="encryption_version"]').value = String(hvac.encryption_version || 2);
        bindField(form.querySelector('[data-k="name"]'), hvac, 'name');
        bindField(form.querySelector('[data-k="thermostat_id"]'), hvac, 'thermostat_id', v => Number(v || 0));
        bindField(form.querySelector('[data-k="host"]'), hvac, 'host');
        bindField(form.querySelector('[data-k="port"]'), hvac, 'port', v => Number(v || 7000));
        bindField(form.querySelector('[data-k="mac"]'), hvac, 'mac');
        bindField(form.querySelector('[data-k="encryption_version"]'), hvac, 'encryption_version', v => Number(v || 2));
        bindField(form.querySelector('[data-k="key"]'), hvac, 'key');
        bindField(form.querySelector('[data-k="current_temp"]'), hvac, 'current_temp', v => Number(v || 25));
        bindField(form.querySelector('[data-k="setpoint"]'), hvac, 'setpoint', v => Number(v || 24));

        root.appendChild(card);
      });
      updatePreview();
    }

    async function postJson(url, payload) {
      const response = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      const data = await response.json();
      if (!response.ok || !data.ok) {
        throw new Error(data.error || 'Request failed');
      }
      return data;
    }

    document.getElementById('manualBtn').addEventListener('click', () => {
      state.hvacs.push(makeManualHvac());
      renderHvacs();
      log('Manual HVAC entry added.');
    });

    document.getElementById('previewBtn').addEventListener('click', () => {
      updatePreview();
      showBanner('scanBanner', 'info', 'Config preview refreshed.');
      log('Config preview refreshed.');
    });

    document.getElementById('scanBtn').addEventListener('click', async () => {
      hideBanner('scanBanner');
      showOverlay('Scanning network', 'The operator is probing the subnet and trying to retrieve Gree keys.');
      try {
        const data = await postJson('/api/scan', {
          subnet: document.getElementById('subnet').value.trim(),
          hosts: document.getElementById('hosts').value.split(',').map(v => v.trim()).filter(Boolean),
          port: Number(document.getElementById('port').value || 7000),
          timeout: Number(document.getElementById('timeout').value || 1.5),
          workers: Number(document.getElementById('workers').value || 48),
          start_id: Number(document.getElementById('startId').value || 1)
        });
        state.hvacs = data.hvacs;
        renderHvacs();
        showBanner('scanBanner', 'ok', data.message);
        log(data.message);
      } catch (error) {
        showBanner('scanBanner', 'error', error.message);
        log('Scan failed: ' + error.message);
      } finally {
        hideOverlay();
      }
    });

    document.getElementById('deployBtn').addEventListener('click', async () => {
      hideBanner('deployBanner');
      showOverlay('Deploying to Savant', 'The helper, config, and startup command are being sent to the Savant host.');
      try {
        const payload = {
          savant_host: document.getElementById('savantHost').value.trim(),
          savant_user: document.getElementById('savantUser').value.trim() || 'RPM',
          savant_password: document.getElementById('savantPass').value,
          remote_dir: document.getElementById('remoteDir').value.trim() || '/data/codex_probe',
          config: currentConfig()
        };
        const data = await postJson('/api/deploy', payload);
        showBanner('deployBanner', 'ok', data.message);
        log('Deploy success.\\n' + data.output);
      } catch (error) {
        showBanner('deployBanner', 'error', error.message);
        log('Deploy failed: ' + error.message);
      } finally {
        hideOverlay();
      }
    });

    renderHvacs();
  </script>
</body>
</html>
"""


def run_command(command):
    completed = subprocess.run(command, capture_output=True, text=True)
    return completed.returncode, (completed.stdout or "") + (completed.stderr or "")


def build_expect_scp(files, user, host, password, remote_dir):
    joined = " ".join(files)
    return (
        "set timeout 45; "
        "spawn scp {files} {user}@{host}:{remote}; "
        "expect \"*?assword:*\" {{ send \"{password}\\r\" }}; "
        "expect eof"
    ).format(files=joined, user=user, host=host, remote=remote_dir, password=password)


def build_expect_ssh(command, user, host, password):
    return (
        "set timeout 45; "
        "spawn ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
        "{user}@{host} \"{command}\"; "
        "expect \"*?assword:*\" {{ send \"{password}\\r\" }}; "
        "expect eof"
    ).format(user=user, host=host, command=command.replace("\"", "\\\""), password=password)


def scan_hvacs(payload):
    hosts = gree_scan.hosts_from_args(payload.get("subnet"), payload.get("hosts", []))
    port = int(payload.get("port", gree_scan.DEFAULT_PORT))
    timeout = float(payload.get("timeout", gree_scan.DEFAULT_TIMEOUT))
    workers = max(1, int(payload.get("workers", gree_scan.DEFAULT_WORKERS)))
    start_id = int(payload.get("start_id", 1))

    results = []
    with gree_scan.ThreadPoolExecutor(max_workers=workers) as executor:
        future_map = {
            executor.submit(gree_scan.probe_host, host, port, timeout): host
            for host in hosts
        }
        for future in gree_scan.as_completed(future_map):
            result = future.result()
            if result:
                results.append(result)

    good = [item for item in results if not item.get("error")]
    good.sort(key=lambda item: tuple(int(part) for part in item["host"].split(".")))
    hvacs = []
    for index, result in enumerate(good, start=start_id):
        status = result.get("status_decoded") or {}
        hvacs.append({
            "thermostat_id": index,
            "name": "Gree HVAC %d (%s)" % (index, result["host"]),
            "host": result["host"],
            "port": result["port"],
            "mac": result["mac"],
            "encryption_version": result["encryption_version"],
            "key": result["key"],
            "current_temp": int(round(status.get("current_temp_c", 25) or 25)),
            "setpoint": int(round(status.get("target_temp_c", 24) or 24)),
            "fan": "FANAUTO",
            "mode": "OFF",
            "power": False,
            "source": "scan",
            "status_decoded": status,
        })
    return {
        "hvacs": hvacs,
        "message": "Scan complete. %d unit(s) discovered and prepared for deployment." % len(hvacs),
        "raw_results": results,
    }


def validate_config(config):
    devices = config.get("devices", [])
    gree_devices = config.get("gree_local", {}).get("devices", [])
    if not devices or not gree_devices:
        raise ValueError("Add at least one HVAC before deploying.")
    ids = set()
    for device in devices:
        thermostat_id = int(device.get("thermostat_id", 0))
        if thermostat_id < 1:
            raise ValueError("Each thermostat needs a valid ID.")
        if thermostat_id in ids:
            raise ValueError("Duplicate thermostat ID %s" % thermostat_id)
        ids.add(thermostat_id)
        if not device.get("name"):
            raise ValueError("Each thermostat needs a name.")
    for device in gree_devices:
        if not device.get("host"):
            raise ValueError("Each Gree device needs an IP address.")
        if not device.get("mac"):
            raise ValueError("Each Gree device needs a MAC address.")
        if not device.get("key"):
            raise ValueError("Each Gree device needs a key.")


def deploy_to_savant(payload):
    host = payload.get("savant_host", "").strip()
    user = payload.get("savant_user", "RPM").strip() or "RPM"
    password = payload.get("savant_password", "")
    remote_dir = payload.get("remote_dir", DEFAULT_REMOTE_DIR).strip() or DEFAULT_REMOTE_DIR
    config = payload.get("config", {})

    if not host:
        raise ValueError("Savant host IP is required.")
    if not password:
        raise ValueError("Savant password is required.")

    validate_config(config)

    tools_dir = Path(__file__).resolve().parent
    script_path = tools_dir / "savant_hvac_gateway.py"
    if not script_path.exists():
        raise ValueError("Missing helper script at %s" % script_path)

    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
        json.dump(config, handle, indent=2)
        handle.flush()
        temp_config_path = handle.name

    try:
        rc, output = run_command(["expect", "-c", build_expect_ssh("mkdir -p %s" % remote_dir, user, host, password)])
        if rc != 0:
            raise RuntimeError("Could not create remote directory.\n" + output)

        rc, output2 = run_command([
            "expect", "-c",
            build_expect_scp([str(script_path), temp_config_path], user, host, password, remote_dir + "/")
        ])
        if rc != 0:
            raise RuntimeError("Could not copy helper/config to Savant host.\n" + output2)

        start_cmd = (
            "cd {remote}; "
            "PID=$(cat hvac_gateway.pid 2>/dev/null || true); "
            "if [ -n \"$PID\" ]; then kill \"$PID\" >/dev/null 2>&1 || true; fi; "
            "nohup python savant_hvac_gateway.py {cfg} > hvac_gateway.log 2>&1 & echo $! > hvac_gateway.pid; "
            "sleep 1; echo PID:$(cat hvac_gateway.pid); "
            "echo ---LOG---; sed -n '1,24p' hvac_gateway.log"
        ).format(remote=remote_dir, cfg=Path(temp_config_path).name)
        rc, output3 = run_command(["expect", "-c", build_expect_ssh(start_cmd, user, host, password)])
        if rc != 0:
            raise RuntimeError("Files copied, but the helper could not be started.\n" + output3)

        return {
            "message": "Deployment succeeded. Savant helper is running on %s:%s." % (
                host, config.get("listen_port", DEFAULT_LISTEN_PORT)
            ),
            "output": output + "\n" + output2 + "\n" + output3,
        }
    finally:
        try:
            os.unlink(temp_config_path)
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    def _json_response(self, payload, status=HTTPStatus.OK):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _html_response(self, payload):
        body = payload.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/":
            self._html_response(HTML)
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length or 0)
        try:
            payload = json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            self._json_response({"ok": False, "error": "Invalid JSON payload"}, status=HTTPStatus.BAD_REQUEST)
            return

        try:
            if parsed.path == "/api/scan":
                data = scan_hvacs(payload)
                self._json_response({"ok": True, **data})
                return
            if parsed.path == "/api/deploy":
                data = deploy_to_savant(payload)
                self._json_response({"ok": True, **data})
                return
            self._json_response({"ok": False, "error": "Unknown endpoint"}, status=HTTPStatus.NOT_FOUND)
        except Exception as exc:
            self._json_response({"ok": False, "error": str(exc)}, status=HTTPStatus.BAD_REQUEST)

    def log_message(self, format, *args):
        return


def main(argv):
    host = DEFAULT_HOST
    port = DEFAULT_PORT
    if len(argv) > 1:
        port = int(argv[1])

    server = ThreadingHTTPServer((host, port), Handler)
    url = "http://%s:%d" % (host, port)
    print("Starting Savant Gree Operator UI %s at %s" % (APP_VERSION, url))
    print("Press Ctrl+C to stop.")

    timer = threading.Timer(0.6, lambda: webbrowser.open(url))
    timer.daemon = True
    timer.start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
