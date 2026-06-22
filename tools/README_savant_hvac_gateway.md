# Savant HVAC Gateway

This folder contains the experimental Savant-side HVAC gateway work.

## What we proved

On the Savant host we tested (`RPM@192.168.51.213`):

- the OS is an embedded ARM Linux system built from an OpenEmbedded/Yocto-style stack
- the `RPM` user is not root
- standard package managers are not available in the normal way
- Python is available:
  - `/usr/bin/python`
  - `/usr/bin/python2`
- writable locations exist:
  - `/home/RPM` which points to `/data/RPM`
  - `/data`
  - `/update`
- we can:
  - copy files onto the host
  - run scripts from `/data`
  - run background user processes

What we did **not** prove:

- a clean user-level boot autostart path
- a root-level service install path

So the current recommended model is:

- keep the helper under `/data`
- launch it manually first
- validate the Savant profile end-to-end
- solve autostart only after the protocol/backend is stable

## Files

- `tools/savant_hvac_gateway.py`
- `tools/savant_hvac_gateway.example.json`
- `tools/deploy_savant_hvac_gateway.py`
- `tools/gree_scan_and_build_json.py`
- `tools/savant_gree_operator_ui.py`

## Why the helper exists

This helper gives Savant a TCP endpoint that looks like a thermostat gateway.

The working pattern is:

- Savant profile -> TCP commands
- Python helper on Savant host -> backend translation
- backend -> real HVAC system

This is intentionally similar to the ESP32 bridge pattern, but hosted directly on the Savant Linux box.

## Port choice

Because `RPM` is not root, the helper should not bind to a privileged port like `23`.

Default test port:

- `2323`

## Current helper state

The helper currently supports:

- `mock` backend
- `gree_local` backend
- `lg_thinq` backend scaffold

The `mock` backend is what we used to validate the profile/service path in Savant.
The `gree_local` backend now implements real local UDP control for Gree units using
the same bind/status/cmd packet flow we already proved with the standalone tester.

## Commands currently supported

- `HELP`
- `STA`
- `LIST`
- `REFRESH`
- `HVAC STATUS <id>`
- `HVAC REFRESH <id>`
- `HVAC OFF <id>`
- `HVAC COOL <id>`
- `HVAC HEAT <id>`
- `HVAC FAN <id>`
- `HVAC FANAUTO <id>`
- `HVAC FANLOW <id>`
- `HVAC FANMID <id>`
- `HVAC FANHIGH <id>`
- `HVAC SETPOINT <id> <temp>`

Example response:

```text
OK
R:HVAC COOL 1
R:HVAC FANAUTO 1
R:HVAC SETPOINT 1 22
R:HVAC CURRENTTEMP 1 25
```

## What we validated in Savant already

With the mock backend on the Savant host and the experimental HVAC-only XML:

- `HVAC COOL 1` worked
- `HVAC SETPOINT 1 22` worked
- Savant accepted the reply format
- thermostat ID `1` mapped correctly through the profile

## XML profiles created during testing

Safer experimental files were created so working profiles remain untouched:

- `experimental_lg_hvac_gateway.xml`
  - first copied experiment
  - still carried lighting/shade baggage from DINPLUG
- `experimental_lg_hvac_gateway_hvac_only.xml`
  - recommended test profile
  - HVAC-only
  - points to port `2323`

For future Savant testing, use:

- host: Savant box IP
- port: `2323`
- XML: `experimental_lg_hvac_gateway_hvac_only.xml`

## Recommended backend order

### 1. Gree first

This was the best first real backend to implement on the Savant host, and it is now
the recommended backend for continued Savant-host gateway work.

Why:

- we already made Gree work locally with the ESP32 gateway
- it is local-network control, not cloud control
- the packet/key model is already known from prior work
- it avoids ThinQ cloud auth, PATs, API drift, and internet dependency

Recommended design:

- keep each Gree unit’s IP, MAC, encryption version, and key in the Savant host JSON config
- do **not** try to store this device data in XML state variables
- use XML only for Savant-facing thermostat IDs and service mapping

What the current implementation does:

- merges Savant-facing thermostat IDs/names from the top-level `devices` list with
  per-unit Gree network settings from `gree_local.devices`
- supports configured `host`, `port`, `mac`, `encryption_version`, and `key`
- can auto-retrieve the per-device key with the Gree `bind` flow if the key/version
  are not prefilled in JSON
- refreshes real status with the Gree `status` request
- sends `OFF`, `COOL`, `HEAT`, `FAN`, `FANAUTO`, `FANLOW`, `FANMID`, `FANHIGH`,
  and `SETPOINT` through the local Gree `cmd` protocol

Operator discovery tooling now also exists:

- `tools/gree_scan_and_build_json.py`
- scans a subnet like `192.168.5.0/24` or a list of specific hosts
- identifies responsive Gree units
- retrieves each unit's MAC, encryption version, and key when possible
- prints ready-to-paste `gree_local.devices` entries and suggested top-level `devices`

There is now also a local browser UI for operators:

- `tools/savant_gree_operator_ui.py`
- runs on the MacBook
- opens a local browser interface
- supports subnet scan or manual HVAC entry
- lets the user edit names, thermostat IDs, IPs, keys, and ports
- generates the full Savant helper JSON automatically
- deploys the helper and generated config to the Savant host using the provided SSH credentials
- reports deployment success or failure and shows the command output

What still needs real-world validation:

- exact fan-speed mapping on every model family
- half-degree setpoint behavior across different firmware revisions
- whether every unit accepts the reduced preserved-key payload we are sending
- multi-split `sub@main` MAC cases on live hardware

This is a better separation of concerns:

- XML = Savant protocol/UI definition
- JSON on `/data` = per-site HVAC inventory and secrets
- Python helper = protocol translation and state handling

### 2. LG ThinQ second

LG ThinQ should come after Gree.

Why:

- official Home Assistant integration is cloud-based
- requires PAT/auth handling
- depends on internet and cloud APIs
- adds more operational risk than Gree local control

## Local development

Run the helper locally:

```bash
python tools/savant_hvac_gateway.py tools/savant_hvac_gateway.example.json
```

If you want a pure local protocol smoke test without touching a real unit yet,
switch `backend` back to `mock` in the example JSON first.

For Gree testing, fill in:

- `devices[*].thermostat_id`
- `gree_local.devices[*].host`
- `gree_local.devices[*].mac`
- optionally `gree_local.devices[*].encryption_version`
- optionally `gree_local.devices[*].key`

If `encryption_version` and `key` are omitted, the helper will attempt the Gree
`bind` flow automatically.

To generate the JSON more quickly when you are on the target LAN:

```bash
python3 tools/gree_scan_and_build_json.py --subnet 192.168.5.0/24
```

Or probe known hosts directly:

```bash
python3 tools/gree_scan_and_build_json.py --host 192.168.5.62 --host 192.168.5.63
```

This prints:

- ready-to-paste `gree_local.devices`
- suggested top-level `devices`
- a combined JSON snippet you can merge into the Savant helper config

To run the full operator UI instead:

```bash
python3 tools/savant_gree_operator_ui.py
```

By default it starts on:

- `http://127.0.0.1:8765`

Current UI workflow:

1. Scan the subnet or add HVAC units manually.
2. Review and edit thermostat IDs, names, IPs, MACs, encryption versions, and keys.
3. Enter the Savant host IP, username, password, and remote directory.
4. Deploy the generated config and helper to the Savant host.

Current scope:

- Gree local discovery and config generation
- Savant helper deployment

Still future work:

- automatic Savant XML placement/install
- post-deploy live command testing from the UI
- direct on-host enrollment/runtime auto-discovery inside the Savant helper

Then test:

```bash
nc 127.0.0.1 2323
```

Try:

```text
HELP
STA
HVAC OFF 1
HVAC COOL 1
HVAC SETPOINT 1 22
```

## Savant host deployment

You can deploy manually or use the deployment helper:

```bash
python3 tools/deploy_savant_hvac_gateway.py \
  --host 192.168.51.213 \
  --password RPM \
  --config tools/savant_hvac_gateway.example.json \
  --start
```

Default remote directory:

- `/data/codex_probe`

The deploy tool:

- copies the Python helper
- copies the config file
- optionally starts the helper
- optionally stops the prior helper instance first

## Operational note

This project currently assumes manual process management on the Savant host.

That is acceptable for protocol development and first deployment testing.

Once the backend is real and stable, the next infrastructure task should be:

- find a reliable Savant-compatible autostart strategy for the `RPM` user or system init
