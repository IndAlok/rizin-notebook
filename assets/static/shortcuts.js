/* shortcuts.js — Rizin Notebook keyboard shortcuts (configurable)
 *
 * Reads user-configurable keybindings from window.NOTEBOOK_KB (set by server
 * template before this script loads). Falls back to built-in defaults.
 *
 * API:
 *   NotebookShortcuts.bind(bindings)       — register shortcuts
 *   NotebookShortcuts.annotate(el, action) — add visible tooltip to element
 *   NotebookShortcuts.combo(action)        — get current combo for an action
 *   NotebookShortcuts.formatCombo(combo)   — human-readable display string
 *   NotebookShortcuts.record(callback)     — capture next keypress for settings
 */
(function (global, document) {
  "use strict";

  var isMac = /Mac|iPhone|iPad|iPod/.test(global.navigator.platform);

  /* ── Configuration ────────────────────────────────────────── */

  // Server-provided keybindings (set by template <script> before this file)
  var config = global.NOTEBOOK_KB || {};

  // Built-in defaults — MUST match Go DefaultKeybindings in keybindings.go
  var defaults = {
    about: "alt+a",
    new_page: "ctrl+n",
    settings: "alt+s",
    toggle_pipe: "alt+o",
    new_command: "alt+c",
    new_markdown: "alt+m",
    new_script: "alt+j",
    save: "mod+s",
    cancel: "escape",
    execute: "ctrl+enter",
    edit_markdown: "alt+e",
  };

  /* ── Key normalization ────────────────────────────────────── */

  function normalizeKey(key) {
    if (!key) return "";
    var n = String(key).toLowerCase();
    switch (n) {
      case " ":
      case "spacebar":
        return "space";
      case "esc":
        return "escape";
      case "arrowup":
        return "up";
      case "arrowdown":
        return "down";
      case "arrowleft":
        return "left";
      case "arrowright":
        return "right";
      default:
        return n;
    }
  }

  function expandToken(token) {
    token = normalizeKey(token);
    if (token === "mod") return isMac ? "meta" : "ctrl";
    return token;
  }

  function parseCombo(combo) {
    var parts = String(combo || "")
      .split("+")
      .map(function (p) {
        return expandToken(p.trim());
      })
      .filter(Boolean);
    return {
      ctrl: parts.indexOf("ctrl") >= 0,
      meta: parts.indexOf("meta") >= 0,
      alt: parts.indexOf("alt") >= 0,
      shift: parts.indexOf("shift") >= 0,
      key:
        parts.filter(function (p) {
          return ["ctrl", "meta", "alt", "shift"].indexOf(p) < 0;
        })[0] || "",
    };
  }

  /* ── Match event against combo ────────────────────────────── */

  function matches(event, combo) {
    var spec = parseCombo(combo);
    if (!spec.key) return false;
    if (!!event.ctrlKey !== spec.ctrl) return false;
    if (!!event.metaKey !== spec.meta) return false;
    if (!!event.altKey !== spec.alt) return false;
    if (!!event.shiftKey !== spec.shift) return false;
    return normalizeKey(event.key) === spec.key;
  }

  function isEditableTarget(target) {
    if (!target || target === document.body) return false;
    if (target.isContentEditable) return true;
    var tag = (target.tagName || "").toLowerCase();
    return tag === "input" || tag === "textarea" || tag === "select";
  }

  /* ── Display helpers ──────────────────────────────────────── */

  function formatCombo(combo) {
    if (!combo) return "";
    var tokens = String(combo)
      .split("+")
      .map(function (t) {
        t = t.trim().toLowerCase();
        if (t === "mod") return isMac ? "⌘" : "Ctrl";
        if (t === "ctrl") return isMac ? "⌃" : "Ctrl";
        if (t === "meta") return isMac ? "⌘" : "Meta";
        if (t === "alt") return isMac ? "⌥" : "Alt";
        if (t === "shift") return "⇧";
        if (t === "escape") return "Esc";
        if (t === "enter") return "↩";
        if (t === "space") return "Space";
        return t.length === 1
          ? t.toUpperCase()
          : t.charAt(0).toUpperCase() + t.slice(1);
      });
    return tokens.join(isMac ? "" : "+");
  }

  /* ── Core API ─────────────────────────────────────────────── */

  /** Get the effective combo string for an action name. */
  function combo(action) {
    return config[action] || defaults[action] || "";
  }

  /**
   * Register keyboard shortcuts.
   * Each binding can specify either:
   *   { action: 'about', handler: fn }          — combo looked up from config
   *   { combo: 'ctrl+enter', handler: fn }      — explicit combo
   *
   * Options:
   *   allowInInputs: true   — fire even when focused on input/textarea
   *   when: function(e)     — extra condition that must return true
   */
  function bind(bindings) {
    document.addEventListener(
      "keydown",
      function (event) {
        if (!bindings || !bindings.length) return;
        for (var i = 0; i < bindings.length; i++) {
          var b = bindings[i];
          if (!b) continue;

          // Resolve the combo string
          var c = "";
          if (b.action) {
            c = combo(b.action);
          } else if (b.combo) {
            c = String(b.combo);
          }
          if (!c) continue;

          // Match against event
          if (!matches(event, c)) continue;

          // Skip if in editable element and not allowed
          if (isEditableTarget(event.target) && !b.allowInInputs) continue;

          // Conditional check
          if (b.when && !b.when(event)) continue;

          // CRITICAL: prevent browser default action (Ctrl+S save, Alt+D address bar, etc.)
          event.preventDefault();
          event.stopPropagation();

          b.handler(event);
          return; // only first match fires
        }
      },
      true,
    ); // capture phase — fires before any other handler
  }

  /**
   * Add a visible Spectre CSS tooltip showing the keyboard shortcut.
   * @param {Element} element — the DOM element to annotate
   * @param {string} actionOrCombo — action name (e.g. 'about') or raw combo
   */
  function annotate(element, actionOrCombo) {
    if (!element) return;
    // If it's a known action name, look up the combo; otherwise use as-is
    var c = defaults[actionOrCombo] ? combo(actionOrCombo) : actionOrCombo;
    if (!c) return;
    var display = formatCombo(c);
    // Build tooltip text: "Button Text (Shortcut)" or just "Shortcut"
    var text = (element.textContent || "").replace(/\s+/g, " ").trim();
    var tooltip = text ? text + " (" + display + ")" : display;
    element.setAttribute("data-tooltip", tooltip);
    element.setAttribute("aria-keyshortcuts", display);
    // Add Spectre CSS tooltip classes
    if (element.classList) {
      element.classList.add("tooltip");
      element.classList.add("tooltip-bottom");
    }
  }

  /**
   * Record the next key combo pressed by the user (for keybinding editor).
   * Calls callback(comboString) when a non-modifier key is pressed.
   */
  function record(callback) {
    function handler(e) {
      e.preventDefault();
      e.stopPropagation();
      var parts = [];
      if (e.ctrlKey) parts.push("ctrl");
      if (e.altKey) parts.push("alt");
      if (e.shiftKey) parts.push("shift");
      if (e.metaKey) parts.push("meta");
      var key = normalizeKey(e.key);
      // Ignore bare modifier keys
      if (["control", "alt", "shift", "meta"].indexOf(key) >= 0) return;
      parts.push(key);
      document.removeEventListener("keydown", handler, true);
      if (callback) callback(parts.join("+"));
    }
    document.addEventListener("keydown", handler, true);
  }

  /* ── Export ───────────────────────────────────────────────── */

  global.NotebookShortcuts = {
    bind: bind,
    annotate: annotate,
    combo: combo,
    formatCombo: formatCombo,
    record: record,
    isEditableTarget: isEditableTarget,
    defaults: defaults,
  };
})(window, document);
