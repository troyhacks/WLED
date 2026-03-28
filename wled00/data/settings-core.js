// WLED Settings Core Utilities
// Shared by all settings pages that use the /json/cfg API.
// Alpine.js is loaded separately via alpine.min.js

const wledCfg = {
  // Load settings for a page from /json/cfg?p=N
  async load(p) {
    const base = wledCfg._base();
    const r = await fetch(`${base}/json/cfg?p=${p}`);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  },

  // Save settings for a page via PATCH /json/cfg?p=N
  async save(p, data) {
    const base = wledCfg._base();
    const r = await fetch(`${base}/json/cfg?p=${p}`, {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  },

  // Returns true if value is a password sentinel {"len": N}
  isSentinel(v) {
    return v !== null && typeof v === 'object' && !Array.isArray(v) && 'len' in v;
  },

  // Base URL: empty for device, or http://IP for file:// mode
  _base() {
    if (typeof _wledBase !== 'undefined') return _wledBase;
    if (window.location.protocol === 'file:') {
      let ip = localStorage.getItem('locIp');
      if (!ip) {
        ip = prompt('File mode: enter WLED IP address');
        localStorage.setItem('locIp', ip);
      }
      window._wledBase = `http://${ip}`;
      return window._wledBase;
    }
    window._wledBase = '';
    return '';
  },

  // Upload a file to WLED (e.g. /presets.json, /cfg.json)
  async upload(fileInput, destPath) {
    const file = fileInput.files[0];
    if (!file) return { error: 'No file selected' };
    const base = wledCfg._base();
    const fd = new FormData();
    fd.append('data', file, destPath);
    const r = await fetch(`${base}/upload`, { method: 'POST', body: fd });
    fileInput.value = '';
    return { ok: r.ok, status: r.status, text: await r.text() };
  }
};

// --- Alpine.js shared components ---
// Register after Alpine is loaded via: document.addEventListener('alpine:init', ...)

document.addEventListener('alpine:init', () => {

  // passwordField — handles sentinel objects {"len":N}.
  // Usage: <div x-data="passwordField('OP', cfg)" ...>
  //   <input type="password" x-model="display" @change="onChange">
  //   <span x-show="unchanged" x-text="'('+sentinel.len+' chars set)'"></span>
  // </div>
  Alpine.data('passwordField', (key, parentCfg) => ({
    display: '',
    unchanged: false,
    get sentinel() { return parentCfg && parentCfg[key]; },
    init() {
      const v = parentCfg && parentCfg[key];
      if (wledCfg.isSentinel(v)) {
        this.unchanged = true;
        this.display = '';
      } else {
        this.display = v || '';
      }
    },
    onChange() {
      this.unchanged = false;
      if (parentCfg) parentCfg[key] = this.display;
    },
    // Returns the value to send: sentinel if unchanged, new string if changed
    value() {
      if (this.unchanged) return this.sentinel;
      return this.display;
    }
  }));

  // gpioPin — GPIO selector that marks reserved/in-use/read-only pins.
  // Usage: <select x-data="gpioPin(model, gpioMeta)" x-model="model.pin">
  //   <template x-for="opt in options"><option :value="opt.n" :disabled="opt.dis" x-text="opt.label"></option></template>
  // </select>
  Alpine.data('gpioPin', (binding, gpio) => ({
    get options() {
      if (!gpio) return [];
      const rsvd = gpio.rsvd || [];
      const ro = gpio.ro_gpio || [];
      const used = gpio.um_p || [];
      const max = gpio.max_gpio || 48;
      const opts = [{ n: -1, label: 'None', dis: false }];
      for (let i = 0; i <= max; i++) {
        const isRsvd = rsvd.includes(i);
        const isRo = ro.includes(i);
        const isUsed = used.includes(i);
        let label = `GPIO ${i}`;
        if (isRsvd) label += ' (reserved)';
        else if (isRo) label += ' (input only)';
        else if (isUsed) label += ' (in use)';
        opts.push({ n: i, label, dis: isRsvd });
      }
      return opts;
    }
  }));

  // numberInput — numeric input with min/max/step validation
  Alpine.data('numberInput', (binding, min, max, step = 1) => ({
    get invalid() {
      const v = Number(binding);
      return isNaN(v) || v < min || v > max;
    }
  }));

});

// --- Page base mixin ---
// Every settings page can spread this into its Alpine x-data object.
// Usage: function pageSettings() { return { ...wledPageBase(N), cfg: {}, ... } }
function wledPageBase(pageNum) {
  return {
    _page: pageNum,
    saving: false,
    saved: false,
    error: null,
    async _load() {
      try {
        return await wledCfg.load(this._page);
      } catch (e) {
        this.error = e.message;
        return null;
      }
    },
    async _save(data) {
      this.saving = true;
      this.saved = false;
      this.error = null;
      try {
        await wledCfg.save(this._page, data);
        this.saved = true;
        setTimeout(() => { this.saved = false; }, 3000);
      } catch (e) {
        this.error = e.message;
      } finally {
        this.saving = false;
      }
    }
  };
}

// === Usermod Settings Helpers ===
// For vanilla JS usermod settings pages (not Alpine.js based)
// Usage: const config = await umCfg.load('Example');
//        umCfg.save('Example', config);
//        const missing = await umCfg.validate('Example', config);

const umCfg = {
  // Load usermod config from cfg.json
  // Returns the usermod section of config, or empty object if not found
  async load(umName) {
    const base = wledCfg._base();
    const r = await fetch(`${base}/cfg.json`);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const cfg = await r.json();
    return cfg.um && cfg.um[umName] ? cfg.um[umName] : {};
  },

  // Save usermod config via /json/cfg?p=8
  async save(umName, config) {
    const base = wledCfg._base();
    const payload = { um: {} };
    payload.um[umName] = config;
    const r = await fetch(`${base}/json/cfg?p=8`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    if (!r.ok) throw new Error('Save failed');
    return true;
  },

  // Get expected config fields and defaults for a usermod from C++ source of truth
  // Returns { fields: [...], defaults: {...} } or { fields: [], defaults: {} }
  async getInfo(umName) {
    const base = wledCfg._base();
    const r = await fetch(`${base}/json/usermod-fields?page=${umName}`);
    if (!r.ok) return { fields: [], defaults: {} };
    return await r.json();
  },

  // Get expected config fields for a usermod from C++ source of truth
  async getExpectedFields(umName) {
    const info = await umCfg.getInfo(umName);
    return info.fields || [];
  },

  // Get defaults for a usermod from C++ source of truth
  async getDefaults(umName) {
    const info = await umCfg.getInfo(umName);
    return info.defaults || {};
  },

  // Validate loaded config against expected fields from C++
  // Returns array of missing field names (empty if all present)
  async validate(umName, loadedConfig) {
    const fields = await umCfg.getExpectedFields(umName);
    if (!fields.length) return [];
    return fields.filter(f => !(f in loadedConfig));
  },

  // Apply defaults to a loaded config
  // defaults: object with { fieldName: defaultValue, ... }
  // Returns new object with defaults merged in (loaded values take precedence)
  withDefaults(loadedConfig, defaults) {
    const result = {};
    for (const key in defaults) {
      result[key] = loadedConfig[key] !== undefined ? loadedConfig[key] : defaults[key];
    }
    // Copy any extra fields from loadedConfig that aren't in defaults
    for (const key in loadedConfig) {
      if (!(key in defaults)) {
        result[key] = loadedConfig[key];
      }
    }
    return result;
  },

  // Show error message
  showError(msg, elId = 'error-msg') {
    const el = document.getElementById(elId);
    if (el) {
      el.textContent = msg;
      el.style.color = 'red';
      el.style.margin = '8px 0';
      el.style.display = 'block';
    }
    console.error(msg);
  },

  // Show success message (auto-hides after duration ms)
  showSuccess(msg, elId = 'saved-msg', duration = 2000) {
    const el = document.getElementById(elId);
    if (el) {
      el.textContent = msg;
      el.style.color = 'green';
      el.style.margin = '8px 0';
      el.style.display = 'block';
      setTimeout(() => { el.style.display = 'none'; }, duration);
    }
  },

  // Hide error/success messages
  hideMessages(elId = 'error-msg') {
    const el = document.getElementById(elId);
    if (el) el.style.display = 'none';
  },

  // Initialize a settings page with minimal boilerplate.
  // Usage: umCfg.initPage('Name') — auto-discovers everything
  //        umCfg.initPage('Name', { saveButton: '#btn' }) — with options
  //        umCfg.initPage('Name', { field: 'selector' }, { saveButton: '#btn' }) — with fieldMap and options
  // options:
  //   saveButton: selector for save button (default: auto-discovers button[type=submit] in form)
  //   loadingMsg: selector for loading message element (default: #loading-msg)
  //   mainContent: selector for main content element (default: #main-content)
  //   errorMsg: selector for error message element (default: #error-msg)
  //   savedMsg: selector for saved message element (default: #saved-msg)
  async initPage(umName, fieldMapOrOptions, options = {}) {
    // If second arg is options (has saveButton/loadingMsg keys), use as options
    if (fieldMapOrOptions && typeof fieldMapOrOptions === 'object' &&
        ('saveButton' in fieldMapOrOptions || 'loadingMsg' in fieldMapOrOptions ||
         'mainContent' in fieldMapOrOptions || 'errorMsg' in fieldMapOrOptions)) {
      options = fieldMapOrOptions;
      fieldMapOrOptions = null;
    }

    // Auto-discover form and its fields
    const form = document.querySelector('form');
    if (!fieldMapOrOptions || typeof fieldMapOrOptions !== 'object' || Object.keys(fieldMapOrOptions).length === 0) {
      fieldMapOrOptions = {};
      if (form) {
        const els = form.querySelectorAll('input, select, textarea');
        els.forEach(el => {
          const name = el.name || el.id;
          if (name) fieldMapOrOptions[name] = `[name="${name}"]`;
        });
      }
    }

    // Auto-discover standard elements if not provided
    if (!options.saveButton) {
      // Find first submit button in form; use id, name, or tag selector
      const btn = form ? form.querySelector('button[type=submit], input[type=submit]') : null;
      if (btn) {
        if (btn.id) options.saveButton = `#${btn.id}`;
        else if (btn.name) options.saveButton = `[name="${btn.name}"]`;
        else options.saveButton = 'button[type=submit]';
      }
    }
    if (!options.loadingMsg) options.loadingMsg = '#loading-msg';
    if (!options.mainContent) options.mainContent = '#main-content';
    if (!options.errorMsg) options.errorMsg = '#error-msg';
    if (!options.savedMsg) options.savedMsg = '#saved-msg';

    // Auto-insert status elements if missing from DOM
    const insertIfMissing = (id, html) => {
      if (!document.getElementById(id)) {
        const el = document.createElement('div');
        el.id = id;
        el.innerHTML = html;
        el.style.display = 'none';
        el.style.margin = '8px 0';
        document.body.insertBefore(el, document.body.firstChild);
      }
    };
    insertIfMissing('error-msg', '');
    insertIfMissing('saved-msg', '&#10004; Saved!');
    insertIfMissing('loading-msg', 'Loading settings...');

    const defaults = await umCfg.getDefaults(umName);
    const config = umCfg.withDefaults(await umCfg.load(umName), defaults);

    // Validate
    const missing = await umCfg.validate(umName, config);
    if (missing.length > 0) {
      umCfg.showError('Warning: Config incomplete - missing: ' + missing.join(', '), options.errorMsg);
      setTimeout(() => umCfg.hideMessages(options.errorMsg), 8000);
    }

    // Helper to get field definition { element: Selector, type: string }
    function parseField(key, def) {
      if (typeof def === 'string') {
        return { element: def, type: null }; // auto-detect later
      }
      return { element: def.element, type: def.type || null };
    }

    // Helper to get value from element
    function getValue(key, el) {
      const tag = el.tagName.toLowerCase();
      const type = el.type;
      if (tag === 'input') {
        if (type === 'checkbox') return el.checked;
        if (type === 'color' || type === 'number' || type === 'text' || type === 'password') return el.value;
      }
      if (tag === 'select') return el.value;
      return el.value;
    }

    // Helper to set value on element
    function setValue(el, value) {
      const tag = el.tagName.toLowerCase();
      const type = el.type;
      if (tag === 'input') {
        if (type === 'checkbox') { el.checked = !!value; return; }
        if (type === 'color' || type === 'number' || type === 'text' || type === 'password') { el.value = value; return; }
      }
      if (tag === 'select') {
        // Find option with matching value, set as selected
        const opts = el.options;
        for (let i = 0; i < opts.length; i++) {
          if (opts[i].value === String(value)) {
            el.selectedIndex = i;
            return;
          }
        }
        // If no match found, don't change selection
        return;
      }
      el.value = value;
    }

    // Auto-detect type from element and get current value
    const values = {};
    const fields = {};
    for (const key in fieldMap) {
      const { element, type } = parseField(key, fieldMap[key]);
      const el = document.querySelector(element);
      if (!el) { console.warn(`umCfg.initPage: element not found: ${element}`); continue; }
      const tag = el.tagName.toLowerCase();
      let actualType = type;
      if (!actualType) {
        if (tag === 'select') actualType = 'select';
        else if (tag === 'input') actualType = el.type || 'text';
        else actualType = 'text';
      }
      values[key] = getValue(key, el);
      fields[key] = { element: el, type: actualType };
    }

    // Override with loaded config values
    for (const key in config) {
      if (key in fields) {
        setValue(fields[key].element, config[key]);
        values[key] = config[key];
      }
    }

    // Show main content, hide loading message
    const loadingEl = document.querySelector(options.loadingMsg || '#loading-msg');
    const mainEl = document.querySelector(options.mainContent || '#main-content');
    if (loadingEl) loadingEl.style.display = 'none';
    if (mainEl) mainEl.style.display = 'block';

    // Wire up save button
    const saveBtn = document.querySelector(options.saveButton || '#save-btn');
    if (saveBtn) {
      saveBtn.addEventListener('click', async () => {
        // Gather current values from elements
        const currentConfig = {};
        for (const key in fields) {
          currentConfig[key] = getValue(key, fields[key].element);
        }
        try {
          umCfg.showSuccess('Saving...', options.savedMsg || 'saved-msg');
          await umCfg.save(umName, currentConfig);
          umCfg.showSuccess('Saved!', options.savedMsg || 'saved-msg');
          setTimeout(() => window.location.reload(), 1000);
        } catch(e) {
          umCfg.showError('Error: ' + e.message, options.errorMsg || 'error-msg');
        }
      });
    }
  }
};
