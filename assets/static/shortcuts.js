(function (global, document) {
  "use strict";

  var isMac = /Mac|iPhone|iPad|iPod/.test(global.navigator.platform);
  var config = global.NOTEBOOK_KB || {};
  var defaults = {
    about: "alt+a",
    new_page: "ctrl+n",
    settings: "alt+s",
    toggle_pipe: "alt+o",
    new_command: "alt+c",
    new_markdown: "alt+m",
    new_script: "alt+j",
    search_notebook: "mod+shift+f",
    save: "mod+s",
    cancel: "escape",
    execute: "ctrl+enter",
    edit_markdown: "alt+e",
  };

  function normalizeKey(key) {
    if (!key) return "";
    var normalized = String(key).toLowerCase();
    switch (normalized) {
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
        return normalized;
    }
  }

  function expandToken(token) {
    token = normalizeKey(token);
    if (token === "mod") {
      return isMac ? "meta" : "ctrl";
    }
    return token;
  }

  function parseCombo(combo) {
    var parts = String(combo || "")
      .split("+")
      .map(function (part) {
        return expandToken(part.trim());
      })
      .filter(Boolean);
    return {
      ctrl: parts.indexOf("ctrl") >= 0,
      meta: parts.indexOf("meta") >= 0,
      alt: parts.indexOf("alt") >= 0,
      shift: parts.indexOf("shift") >= 0,
      key:
        parts.filter(function (part) {
          return ["ctrl", "meta", "alt", "shift"].indexOf(part) < 0;
        })[0] || "",
    };
  }

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

  function formatCombo(combo) {
    if (!combo) return "";
    var tokens = String(combo)
      .split("+")
      .map(function (token) {
        token = token.trim().toLowerCase();
        if (token === "mod") return isMac ? "⌘" : "Ctrl";
        if (token === "ctrl") return isMac ? "⌃" : "Ctrl";
        if (token === "meta") return isMac ? "⌘" : "Meta";
        if (token === "alt") return isMac ? "⌥" : "Alt";
        if (token === "shift") return "⇧";
        if (token === "escape") return "Esc";
        if (token === "enter") return "↩";
        if (token === "space") return "Space";
        return token.length === 1
          ? token.toUpperCase()
          : token.charAt(0).toUpperCase() + token.slice(1);
      });
    return tokens.join(isMac ? "" : "+");
  }

  function combo(action) {
    return config[action] || defaults[action] || "";
  }

  function bind(bindings) {
    document.addEventListener(
      "keydown",
      function (event) {
        if (!bindings || !bindings.length) return;
        for (var i = 0; i < bindings.length; i++) {
          var binding = bindings[i];
          if (!binding) continue;

          var comboString = "";
          if (binding.action) {
            comboString = combo(binding.action);
          } else if (binding.combo) {
            comboString = String(binding.combo);
          }
          if (!comboString) continue;
          if (!matches(event, comboString)) continue;
          if (isEditableTarget(event.target) && !binding.allowInInputs) continue;
          if (binding.when && !binding.when(event)) continue;

          event.preventDefault();
          event.stopPropagation();
          binding.handler(event);
          return;
        }
      },
      true,
    );
  }

  function annotate(element, actionOrCombo) {
    if (!element) return;
    var comboString = defaults[actionOrCombo] ? combo(actionOrCombo) : actionOrCombo;
    if (!comboString) return;
    var display = formatCombo(comboString);
    var text = (element.textContent || "").replace(/\s+/g, " ").trim();
    var tooltip = text ? text + " (" + display + ")" : display;
    element.setAttribute("data-tooltip", tooltip);
    element.setAttribute("aria-keyshortcuts", display);
    if (element.classList) {
      element.classList.add("tooltip");
      element.classList.add("tooltip-bottom");
    }
  }

  function record(callback) {
    function handler(event) {
      event.preventDefault();
      event.stopPropagation();
      var parts = [];
      if (event.ctrlKey) parts.push("ctrl");
      if (event.altKey) parts.push("alt");
      if (event.shiftKey) parts.push("shift");
      if (event.metaKey) parts.push("meta");
      var key = normalizeKey(event.key);
      if (["control", "alt", "shift", "meta"].indexOf(key) >= 0) return;
      parts.push(key);
      document.removeEventListener("keydown", handler, true);
      if (callback) callback(parts.join("+"));
    }

    document.addEventListener("keydown", handler, true);
  }

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
