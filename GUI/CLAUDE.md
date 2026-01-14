# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the ODrive GUI - an Electron-based desktop application for interfacing with ODrive motor controllers. It uses Vue.js 2 for the frontend and a Python Flask/SocketIO server for ODrive hardware communication.

## Architecture

### Three-Tier Communication Architecture

1. **Frontend (Vue.js)**: User interface with dashboards, controls, and plots
2. **Backend (Python Flask/SocketIO)**: Bridges between the GUI and ODrive hardware using odrivetool and fibre
3. **Hardware**: ODrive motor controllers connected via USB

The communication flow:
- Vue components → Vuex store → Socket.IO client ([src/comms/socketio.js](src/comms/socketio.js)) → Python server ([server/odrive_server.py](server/odrive_server.py)) → ODrive hardware

### Key Electron Architecture

- **Main Process**: [src/background.js](src/background.js) - Manages window lifecycle and spawns the Python server
- **Renderer Process**: Vue.js app ([src/App.vue](src/App.vue), [src/main.js](src/main.js))
- **Preload Script**: [src/preload.js](src/preload.js) - IPC bridge between main and renderer

The Python server is spawned when the Electron app starts and communicates with the renderer via IPC events (`start-server`, `kill-server`, `server-stdout`, `server-stderr`).

### State Management (Vuex)

The [src/store.js](src/store.js) is the central Vuex store managing:
- `odrives`: Connected ODrive device states
- `odriveConfigs`: Device configuration metadata (full/functions/params/writeAble)
- `dashboards`: Array of dashboard configurations
- `sampledProperties` and `propSamples`: Real-time data sampling for plots
- `currentDash`: Active dashboard view

### Dashboard System

Dashboards are JSON-based configurations containing:
- **controls**: UI widgets (CtrlNumeric, CtrlSlider, CtrlEnum, CtrlBoolean, CtrlFunction)
- **actions**: Quick-action buttons with preset values
- **plots**: Real-time chart configurations with variable paths

Dashboard files are stored in [src/assets/dashboards/](src/assets/dashboards/) and loaded based on ODrive firmware version (0.4.12 vs 0.5.1+). Users can import/export custom dashboards.

### Component Structure

- **views/**: Top-level pages (Start, Dashboard, Wizard)
- **components/controls/**: Control widgets for ODrive parameters
- **components/actions/**: Action button components
- **components/plots/**: Chart.js-based plotting components
- **components/wizard/**: Motor setup wizard screens
- **components/Axis.vue**: Per-axis control panel

### Fibre-JS Integration

[fibre-js/](fibre-js/) provides JavaScript bindings for the Fibre protocol used by ODrive. While included in the repo, the GUI primarily uses the Python server approach rather than direct WebUSB communication.

## Common Development Commands

### Development
```bash
npm install              # Install dependencies
npm run serve            # Run Vue dev server (web only)
npm run electron:serve   # Run Electron app with hot-reload
```

### Running with Local ODrive Dependencies
If working with unreleased ODrive firmware changes:
```bash
npm run electron:serve -- ../tools/
```
This uses the odrivetool and fibre modules from the adjacent `tools/` directory.

### Building
```bash
npm run electron:build              # Build for current platform
npm run electron:build -- -mwl      # Build for Mac, Windows, Linux
```

The build outputs a portable executable (Windows) or AppImage (Linux).

### Enum Generation
```bash
npm run enumGenerate
```
Reads [../../tools/odrive/enums.py](../../tools/odrive/enums.py) and generates [src/assets/odriveEnums.json](src/assets/odriveEnums.json). This runs automatically before `electron:serve` and `electron:build`.

### Linting
```bash
npm run lint             # Lint and auto-fix
```

### ARM/Raspberry Pi Builds
Install PhantomJS first:
```bash
sudo apt install phantomjs
npm run electron:build
```

## Python Server Requirements

The Python server requires:
```bash
pip install flask flask_socketio flask_cors odrive
```

The server expects either `python` or `python3` to be available and running Python 3.x.

## Build Configuration

- **vue.config.js**: Electron-builder settings
  - `asar: false`: Files remain unpackaged for easier debugging
  - `extraResources: "server"`: Bundles Python server with the app
  - Outputs portable executables (Windows) and AppImages (Linux)

## Important Implementation Details

### Variable Path Format
ODrive properties are accessed via dot-notation paths like:
```
odrives.odrive0.axis0.controller.input_pos
```

These paths are used throughout controls, actions, and plots. The first segment (`odrives`) is stripped when sending to the Python server.

### Real-Time Plotting
Plots use Chart.js with the streaming plugin. Variables to plot are registered in `sampledProperties` and the server continuously sends updates via Socket.IO. Data is stored in `propSamples` with timestamps.

### Dashboard JSON Structure
When creating/modifying dashboards, follow this structure:
```json
{
  "name": "Dashboard Name",
  "component": "Dashboard",
  "id": "unique-uuid",
  "controls": [{"controlType": "CtrlNumeric", "path": "..."}],
  "actions": [{"actionType": "Action", "path": "...", "val": 0}],
  "plots": [{"name": "uuid", "vars": [{"path": "...", "color": "#..."}]}]
}
```

### Wizard Flow
The setup wizard ([views/Wizard.vue](views/Wizard.vue)) guides users through motor configuration using a decision-tree of wizard components in [components/wizard/choices/](components/wizard/choices/). It reads/writes ODrive parameters via the same Socket.IO interface.

## Testing Notes

- The GUI connects to `https://0.0.0.0:8080` by default for the Socket.IO server
- In development mode (`npm run electron:serve`), Vue DevTools are automatically installed
- The Electron main process logs Python server stdout/stderr to the console
- Socket.IO events can be monitored in browser DevTools console
