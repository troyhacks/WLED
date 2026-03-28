# Example Usermod - Settings Page Demo

A minimal example demonstrating the auto-discovery settings page system for WLED usermods.

## Files

- `usermod_v2_example.h` - The usermod C++ code
- `settings_example.htm` - The settings page HTML (auto-discovered at build time)
- `README.md` - This file

## How It Works

### 1. Settings Page Auto-Discovery

Place your settings page in your usermod directory with the naming convention `settings_<name>.htm`.

For example: `usermod_v2_example/settings_example.htm` becomes accessible at `/settings_example`.

The build system (`tools/cdata.js`) automatically:
- Scans `usermods/*/settings*.htm` at build time
- Generates `wled00/html_usermod_settings_registry.h`
- Serves pages at `/settings_<name>` URLs

If a usermod has `CONFIG_FIELDS` declared but no settings page, a skeleton is auto-generated at build time.

### 2. Config Fields with Defaults Declaration

In your usermod header, add a `CONFIG_FIELDS` comment with field names and default values:

```cpp
/*
 * CONFIG_FIELDS: enabled=false, debugMode=false, myColor="#ff0000", mySpeed=100
 */
#include "wled.h"
```

Supported default value formats:
- Booleans: `true`, `false`
- Numbers: `123`, `3.14`
- Strings: `"#ff0000"`
- Arrays: `[]`

This is the **single source of truth** for what fields your usermod expects.

### 3. Implementing the Usermod

```cpp
class ExampleUsermod : public Usermod {
private:
  bool enabled = false;
  bool debugMode = false;
  char myColor[32] = "#ff0000";
  uint16_t mySpeed = 100;

  static const char _name[];
  static const char _enabled[];
  static const char _debugMode[];
  static const char _myColor[];
  static const char _mySpeed[];

public:
  ExampleUsermod(bool enabled) : Usermod("Example", enabled) {}

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_debugMode)] = debugMode;
    top[FPSTR(_myColor)] = myColor;
    top[FPSTR(_mySpeed)] = mySpeed;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;
    enabled = top[FPSTR(_enabled)] | enabled;
    debugMode = top[FPSTR(_debugMode)] | debugMode;
    strlcpy(myColor, top[FPSTR(_myColor)] | "#ff0000", sizeof(myColor));
    mySpeed = top[FPSTR(_mySpeed)] | mySpeed;
    return !top[FPSTR(_enabled)].isNull();
  }
};

// String constants
inline const char ExampleUsermod::_name[] PROGMEM = "Example";
inline const char ExampleUsermod::_enabled[] PROGMEM = "enabled";
inline const char ExampleUsermod::_debugMode[] PROGMEM = "debugMode";
inline const char ExampleUsermod::_myColor[] PROGMEM = "myColor";
inline const char ExampleUsermod::_mySpeed[] PROGMEM = "mySpeed";
```

### 4. Implementing the Settings Page (Minimal)

```html
<!DOCTYPE html>
<html>
<head><title>Example Usermod Settings</title></head>
<body>
  <form>
    <input type='checkbox' name='enabled'>
    <input type='checkbox' name='debugMode'>
    <input type='color' name='myColor' value='#ff0000'>
    <input type='number' name='mySpeed' value='100'>
    <button type="submit">Save</button>
  </form>
  <script src="/settings-core.js"></script>
  <script>umCfg.initPage('Example');</script>
</body>
</html>
```

**That's it.** `umCfg.initPage()` handles everything:
- Auto-discovers all form fields by `name` attribute
- Auto-finds the submit button
- Auto-inserts status elements (error/success messages) if missing
- Fetches defaults from C++ via `/json/usermod-fields`
- Loads config from `cfg.json`
- Merges with defaults and populates form
- Wires up the save button

### 5. Registering the Usermod

In your `usermods_list.cpp`, add:

```cpp
#include "usermod_v2_example.h"
// ...
void registerUsermods() {
  // ...
  usermods.add(new ExampleUsermod(WLED_FEATURE/example));
}
```

## Key Points

1. **Settings page URL**: `/settings_<name>` where `<name>` is derived from `settings_<name>.htm`

2. **Save endpoint**: `umCfg.save()` uses `/json/cfg?p=8` which automatically calls `readFromConfig()` on all usermods after saving.

3. **No duplication**: Field definitions live in `CONFIG_FIELDS` (C++), not duplicated in HTML/JS.

4. **Defaults from C++**: The `CONFIG_FIELDS` comment includes default values. Parsed at build time and served via `/json/usermod-fields`.

5. **Auto-discovery**: Form fields, submit button, and status elements are all auto-discovered — no explicit configuration needed.

## umCfg Helper API

| Method | Description |
|--------|-------------|
| `await umCfg.load(umName)` | Load config from cfg.json |
| `await umCfg.save(umName, config)` | Save config via /json/cfg?p=8 |
| `await umCfg.getInfo(umName)` | Get { fields, defaults } from C++ |
| `await umCfg.getExpectedFields(umName)` | Get expected field names |
| `await umCfg.getDefaults(umName)` | Get default values from C++ |
| `await umCfg.validate(umName, config)` | Returns missing field names |
| `umCfg.withDefaults(config, defaults)` | Apply defaults to loaded config |
| `umCfg.initPage(umName)` | Initialize page — fully auto-discovered |
| `umCfg.initPage(umName, options)` | With options (saveButton, etc.) |
| `umCfg.showError(msg)` | Show error message |
| `umCfg.showSuccess(msg)` | Show success message (auto-hides) |
| `umCfg.hideMessages()` | Hide error/success messages |

## Available Endpoints

| Endpoint | Description |
|----------|-------------|
| `/settings_example` | The settings page |
| `/json/usermod-pages` | Returns JSON array of all available usermod pages |
| `/json/usermod-fields?page=example` | Returns { fields, defaults } for validation |
| `/json/cfg?p=8` | Save config (POST) |
| `/cfg.json` | Load config (GET) |
