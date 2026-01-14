# Changelog

All notable changes to the ODrive GUI will be documented in this file.

## [Unreleased]

### Added
- **Internationalization (i18n) Support**: Complete Chinese language support for the entire GUI
  - Added vue-i18n v8.28.2 for internationalization
  - Language switcher in application header
  - Persistent language preference (saved to localStorage)
  - Full translation coverage (200+ strings):
    - Application header and navigation
    - Start/setup page
    - Dashboard controls and buttons
    - Wizard (all pages, choices, and tooltips)
    - Motor and encoder calibration messages
    - Error labels and status messages
    - Plot controls
  - Professional Chinese translations for motor control terminology
  - Translation files: `src/locales/en.json` and `src/locales/zh.json`
  - Chinese README: `README.zh.md`

### Fixed
- Fixed `scripts/enumGenerate.js` script error when parsing lines without '=' separator
  - Added proper validation for enum lines
  - Improved error handling for malformed input

### Changed
- Enhanced README.md with language support documentation
- Added i18n development guidelines for future language additions

---

## Previous Versions

For older version history, please refer to the main ODrive repository changelog.
