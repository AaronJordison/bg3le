# Changelog

## v0.2.0 (2026-10-02)

- **Native plugins.** bg3le loads shared libraries from `~/.local/share/bg3le/plugins` (or `$BG3LE_PLUGINS_DIR`)
  with the game, with no `LD_PRELOAD` or launch option change. A plugin gets logging, SDL event handlers that can
  keep events from the game, SDL's own functions, and settings.
- **Plugin settings in Lua.** `Ext.Plugins.List`, `GetSettings`, `Get` and `Set`, so a Script Extender mod can show
  a plugin's settings in the Mod Configuration Menu.
- Settings persist in `<plugin>.settings.json` beside the plugin, applied from its first frame. The file lists every
  setting and can be edited by hand with the game closed.
- The release package includes `include/bg3le_plugin.h`, the whole plugin API. See "Native plugins" in the README.
- First plugin: [Linux Native Camera Tweaks](https://github.com/lenonk/LinuxNativeCameraTweaks).

## v0.1.1

- Fixed Mod Configuration Menu 1.41 failing to load ("attempt to call a nil value (method 'GetSettingValue')").
  Mod script chunks are now named `<Directory>/<file>` as upstream names them, which MCM uses to tell which mod
  is calling.
- Releases are a zip, which Nexus Mods accepts, and keep `install.py` executable when unzipped.

## v0.1.0

- First release: bg3se's public `Ext` API on Baldur's Gate 3's native Linux build, in both contexts, with Script
  Extender mods loading from their paks, plus an installer.
