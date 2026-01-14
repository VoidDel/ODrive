# ODrive GUI

[English](#odrive-gui) | [中文](README.zh.md)

A desktop application for configuring and controlling ODrive motor controllers.

## Features

- **Internationalization (i18n)**: Full support for English and Chinese languages
- Interactive dashboard with real-time plotting
- Step-by-step setup wizard for motor configuration
- Live parameter tuning and monitoring
- Motor and encoder calibration tools

## Language Support

The GUI supports multiple languages. Click the language button in the header to switch between:
- **English** (Default)
- **中文** (Chinese)

Your language preference is automatically saved and will be restored when you reopen the application.

## Requirements

Python requirements: `pip install flask flask_socketio flask_cors odrive`

If the default odrive python package is not desired, the path to the modules can be passed as command line arguments.

example on windows 10: 
```
./odrive_gui_win.exe C:/Users/<you>/ODrive/tools C:/Users/<you>/ODrive/Firmware
```

The first argument is for your local version of odrivetool, the second is for fibre.

## Development and testing instructions

### Project setup
```
npm install
```

### Compiles and hot-reloads for development
```
npm run serve
```

### Lints and fixes files
```
npm run lint
```

### Serve electron version of GUI
```
npm run electron:serve
```

### Package electron app into executable
```
npm run electron:build
```

### Build electron app for all platforms
```
npm run electron:build -- -mwl
```

### Running from source
On the devel git branch, there may be unreleased changes to dependencies like fibre or the ODrive enumerations.
Use this command to launch the GUI with the dependencies from the repo:
```
npm run electron:serve -- ../tools/
```

### Building for rpi and potentially other ARM platform devices

PhantomJS is required as a dependency, so it must be installed first:
```
sudo apt install phantomjs
```

After it is installed, `npm run electron:build` can be used to build the AppImage for ARM

## Internationalization (i18n)

The GUI uses [vue-i18n](https://kazupon.github.io/vue-i18n/) for internationalization.

### Translation Files

Translation files are located in `src/locales/`:
- `en.json` - English translations
- `zh.json` - Chinese translations (中文翻译)
- `index.js` - i18n configuration

### Adding a New Language

1. Create a new translation file in `src/locales/` (e.g., `ja.json` for Japanese)
2. Copy the structure from `en.json` and translate all strings
3. Import and add the new locale in `src/locales/index.js`:
   ```javascript
   import ja from './ja.json'

   export default {
     en,
     zh,
     ja  // Add your new language here
   }
   ```
4. Add a language switcher option in `src/App.vue` if needed

### Translation Coverage

- ✅ Application header and navigation
- ✅ Start/setup page
- ✅ Dashboard controls and buttons
- ✅ Wizard (all pages, choices, and tooltips)
- ✅ Calibration messages
- ✅ Error labels and status messages
- ✅ Plot controls

### Customize configuration
See [Configuration Reference](https://cli.vuejs.org/config/).
