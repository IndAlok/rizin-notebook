(function (global, document) {
  "use strict";

  var state = {
    activeIndex: -1,
    controller: null,
    mode: "literal",
    requestId: 0,
    results: [],
    timer: null,
  };

  function cfg() {
    return global.NB_NOTEBOOK_SEARCH || {};
  }

  function panel() {
    return document.getElementById("notebook-search");
  }

  function toolbarButton() {
    return document.getElementById("open-notebook-search");
  }

  function input() {
    return document.getElementById("notebook-search-focus");
  }

  function resultsList() {
    return document.getElementById("notebook-search-results");
  }

  function statusBox() {
    return document.getElementById("notebook-search-status");
  }

  function surfaceSelect() {
    return document.getElementById("notebook-search-surface");
  }

  function cellTypeSelect() {
    return document.getElementById("notebook-search-cell-type");
  }

  function caseCheckbox() {
    return document.getElementById("notebook-search-case");
  }

  function modeButtons() {
    return Array.prototype.slice.call(
      document.querySelectorAll("#notebook-search-mode-group [data-mode]"),
    );
  }

  function normalizeText(text) {
    return String(text || "").replace(/\r\n?/g, "\n");
  }

  function clampIndex(index) {
    if (!state.results.length) return -1;
    if (index < 0) return 0;
    if (index >= state.results.length) return state.results.length - 1;
    return index;
  }

  function searchDefaults() {
    var searchConfig = cfg().config || {};
    return {
      query: "",
      mode: searchConfig.default_mode || "literal",
      surface: searchConfig.default_surface || "all",
      cellType: searchConfig.default_cell_type || "all",
      caseSensitive: false,
    };
  }

  function readStoredState() {
    var defaults = searchDefaults();
    try {
      if (!cfg().storageKey || !global.localStorage) {
        return defaults;
      }
      var raw = global.localStorage.getItem(cfg().storageKey);
      if (!raw) {
        return defaults;
      }
      var parsed = JSON.parse(raw);
      return {
        query: String(parsed.query || defaults.query),
        mode: String(parsed.mode || defaults.mode),
        surface: String(parsed.surface || defaults.surface),
        cellType: String(parsed.cellType || defaults.cellType),
        caseSensitive: !!parsed.caseSensitive,
      };
    } catch (error) {
      return defaults;
    }
  }

  function writeStoredState() {
    try {
      if (!cfg().storageKey || !global.localStorage) {
        return;
      }
      global.localStorage.setItem(
        cfg().storageKey,
        JSON.stringify({
          query: input() ? input().value : "",
          mode: state.mode,
          surface: surfaceSelect() ? surfaceSelect().value : "all",
          cellType: cellTypeSelect() ? cellTypeSelect().value : "all",
          caseSensitive: caseCheckbox() ? caseCheckbox().checked : false,
        }),
      );
    } catch (error) {}
  }

  function setMode(mode) {
    state.mode = mode || "literal";
    modeButtons().forEach(function (button) {
      button.classList.toggle("is-active", button.getAttribute("data-mode") === state.mode);
    });
  }

  function currentLimit() {
    var searchConfig = cfg().config || {};
    var maxResults = Number(searchConfig.max_results || 0);
    if (!maxResults || maxResults < 1) {
      return 200;
    }
    return maxResults;
  }

  function normalizeRanges(ranges, length) {
    if (!ranges || !ranges.length) return [];
    return ranges
      .map(function (range) {
        return {
          start: Math.max(0, Math.min(length, Number(range.start) || 0)),
          end: Math.max(0, Math.min(length, Number(range.end) || 0)),
        };
      })
      .filter(function (range) {
        return range.end > range.start;
      })
      .sort(function (a, b) {
        return a.start - b.start;
      });
  }

  function unwrapMarks(root) {
    if (!root) return;
    var marks = root.querySelectorAll("mark.nb-search-hit");
    for (var i = 0; i < marks.length; i++) {
      var mark = marks[i];
      var parent = mark.parentNode;
      while (mark.firstChild) {
        parent.insertBefore(mark.firstChild, mark);
      }
      parent.removeChild(mark);
    }
    root.normalize();
  }

  function codePointToCodeUnitOffset(text, codePointOffset) {
    if (codePointOffset <= 0) return 0;
    return Array.from(text).slice(0, codePointOffset).join("").length;
  }

  function childOffset(node) {
    var offset = 0;
    while (node && node.previousSibling) {
      node = node.previousSibling;
      offset++;
    }
    return offset;
  }

  function buildTextSegments(root, includeBreaks) {
    var segments = [];
    var codePointIndex = 0;

    function walk(node) {
      if (!node) return;
      if (node.nodeType === Node.TEXT_NODE) {
        var length = Array.from(node.data || "").length;
        if (!length) return;
        segments.push({
          type: "text",
          node: node,
          start: codePointIndex,
          end: codePointIndex + length,
        });
        codePointIndex += length;
        return;
      }
      if (node.nodeType !== Node.ELEMENT_NODE) {
        return;
      }
      if (includeBreaks && node.tagName === "BR") {
        segments.push({
          type: "break",
          parent: node.parentNode,
          offset: childOffset(node),
          start: codePointIndex,
          end: codePointIndex + 1,
        });
        codePointIndex++;
        return;
      }
      for (var child = node.firstChild; child; child = child.nextSibling) {
        walk(child);
      }
    }

    walk(root);
    return segments;
  }

  function boundaryBeforeSegment(segment) {
    if (!segment) return null;
    if (segment.type === "break") {
      return { container: segment.parent, offset: segment.offset };
    }
    return { container: segment.node, offset: 0 };
  }

  function boundaryAfterSegment(segment) {
    if (!segment) return null;
    if (segment.type === "break") {
      return { container: segment.parent, offset: segment.offset + 1 };
    }
    return {
      container: segment.node,
      offset: codePointToCodeUnitOffset(segment.node.data || "", segment.end - segment.start),
    };
  }

  function locateBoundary(root, segments, absoluteOffset) {
    if (!segments.length) {
      return { container: root, offset: 0 };
    }

    for (var i = 0; i < segments.length; i++) {
      var segment = segments[i];
      if (absoluteOffset < segment.start) {
        return boundaryBeforeSegment(segment);
      }
      if (segment.type === "break") {
        if (absoluteOffset === segment.start) {
          return boundaryBeforeSegment(segment);
        }
        if (absoluteOffset === segment.end) {
          return boundaryAfterSegment(segment);
        }
        continue;
      }
      if (absoluteOffset <= segment.end) {
        return {
          container: segment.node,
          offset: codePointToCodeUnitOffset(segment.node.data || "", absoluteOffset - segment.start),
        };
      }
    }

    return boundaryAfterSegment(segments[segments.length - 1]);
  }

  function wrapCodePointRange(root, start, end, includeBreaks) {
    var segments = buildTextSegments(root, includeBreaks);
    var startBoundary = locateBoundary(root, segments, start);
    var endBoundary = locateBoundary(root, segments, end);
    if (!startBoundary || !endBoundary) return null;

    var range = document.createRange();
    range.setStart(startBoundary.container, startBoundary.offset);
    range.setEnd(endBoundary.container, endBoundary.offset);
    if (range.collapsed) return null;

    var mark = document.createElement("mark");
    mark.className = "nb-search-hit";
    mark.appendChild(range.extractContents());
    range.insertNode(mark);
    return mark;
  }

  function lineBounds(text, lineNumber) {
    if (lineNumber <= 0) return null;
    var chars = Array.from(normalizeText(text));
    var start = 0;
    var currentLine = 1;
    while (start < chars.length && currentLine < lineNumber) {
      if (chars[start] === "\n") {
        currentLine++;
      }
      start++;
    }
    if (currentLine !== lineNumber) {
      return null;
    }
    var end = start;
    while (end < chars.length && chars[end] !== "\n") {
      end++;
    }
    return { start: start, end: end };
  }

  function highlightInlineRanges(root, fullText, lineNumber, ranges, includeBreaks) {
    if (!root) return null;
    unwrapMarks(root);

    var bounds = lineBounds(fullText, Number(lineNumber || 0));
    if (!bounds) return null;

    var safeRanges = normalizeRanges(ranges, bounds.end - bounds.start).sort(function (a, b) {
      return b.start - a.start;
    });
    var firstMark = null;
    for (var i = 0; i < safeRanges.length; i++) {
      var range = safeRanges[i];
      var mark = wrapCodePointRange(
        root,
        bounds.start + range.start,
        bounds.start + range.end,
        includeBreaks,
      );
      if (mark) {
        firstMark = mark;
      }
    }
    return firstMark;
  }

  function buildHighlightedTextFragment(text, ranges) {
    var fragment = document.createDocumentFragment();
    var chars = Array.from(String(text || ""));
    var safeRanges = normalizeRanges(ranges, chars.length);
    if (!safeRanges.length) {
      if (chars.length) {
        fragment.appendChild(document.createTextNode(chars.join("")));
      }
      return fragment;
    }

    var cursor = 0;
    safeRanges.forEach(function (range) {
      if (cursor < range.start) {
        fragment.appendChild(document.createTextNode(chars.slice(cursor, range.start).join("")));
      }
      var mark = document.createElement("mark");
      mark.className = "nb-search-hit";
      mark.textContent = chars.slice(range.start, range.end).join("");
      fragment.appendChild(mark);
      cursor = range.end;
    });
    if (cursor < chars.length) {
      fragment.appendChild(document.createTextNode(chars.slice(cursor).join("")));
    }
    return fragment;
  }

  function resultSurfaceLabel(result) {
    if (result.surface === "output") {
      return "Output";
    }
    if (result.cell_type === "markdown") {
      return "Markdown";
    }
    return "Source";
  }

  function resultCellTypeLabel(result) {
    if (result.cell_type === "script") {
      return "Script";
    }
    if (result.cell_type === "markdown") {
      return "Markdown";
    }
    return "Command";
  }

  function setStatus(text, isError) {
    var box = statusBox();
    if (!box) return;
    box.textContent = text;
    box.classList.toggle("is-error", !!isError);
  }

  function clearResults() {
    var list = resultsList();
    if (list) {
      list.replaceChildren();
    }
    state.results = [];
    state.activeIndex = -1;
  }

  function showEmptyState(message) {
    clearResults();
    setStatus(message, false);
  }

  function updateToolbarState() {
    var button = toolbarButton();
    if (!button) return;
    button.classList.toggle("is-active", isNotebookSearchOpen());
  }

  function isNotebookSearchOpen() {
    var currentPanel = panel();
    return !!currentPanel && currentPanel.classList.contains("is-open");
  }

  function openNotebookSearch() {
    if (global.isCommandHelpOpen && global.isCommandHelpOpen()) {
      global.closeCmdHelp();
    }

    var currentPanel = panel();
    if (!currentPanel) return;
    currentPanel.classList.add("is-open");
    currentPanel.setAttribute("aria-hidden", "false");
    updateToolbarState();

    var searchInput = input();
    if (!searchInput) return;
    searchInput.focus();
    searchInput.select();
    if (searchInput.value.trim()) {
      queueNotebookSearch(0);
      return;
    }
    showEmptyState("Search commands, scripts, outputs, and markdown in this notebook.");
  }

  function closeNotebookSearch() {
    var currentPanel = panel();
    if (!currentPanel) return;
    currentPanel.classList.remove("is-open");
    currentPanel.setAttribute("aria-hidden", "true");
    updateToolbarState();
  }

  function queueNotebookSearch(delay) {
    if (state.timer) {
      global.clearTimeout(state.timer);
    }
    state.timer = global.setTimeout(runNotebookSearch, typeof delay === "number" ? delay : 140);
  }

  function setActiveResult(index, shouldScroll) {
    state.activeIndex = clampIndex(index);
    var buttons = document.querySelectorAll(".nb-search-result");
    for (var i = 0; i < buttons.length; i++) {
      var isActive = i === state.activeIndex;
      buttons[i].classList.toggle("is-active", isActive);
      buttons[i].setAttribute("aria-selected", isActive ? "true" : "false");
      if (isActive && shouldScroll && buttons[i].scrollIntoView) {
        buttons[i].scrollIntoView({ block: "nearest" });
      }
    }
  }

  function moveActiveResult(delta) {
    if (!state.results.length) return;
    var nextIndex = state.activeIndex;
    if (nextIndex < 0) {
      nextIndex = 0;
    } else {
      nextIndex = (nextIndex + delta + state.results.length) % state.results.length;
    }
    setActiveResult(nextIndex, true);
  }

  function runNotebookSearch() {
    state.timer = null;

    var searchInput = input();
    var query = searchInput ? searchInput.value.trim() : "";
    if (state.controller) {
      state.controller.abort();
      state.controller = null;
    }
    writeStoredState();
    if (!query) {
      showEmptyState("Search commands, scripts, outputs, and markdown in this notebook.");
      return;
    }

    var requestId = ++state.requestId;
    var params = new URLSearchParams();
    params.set("query", query);
    params.set("mode", state.mode);
    params.set("surface", surfaceSelect() ? surfaceSelect().value : "all");
    params.set("cell_type", cellTypeSelect() ? cellTypeSelect().value : "all");
    params.set("limit", String(currentLimit()));
    if (caseCheckbox() && caseCheckbox().checked) {
      params.set("case_sensitive", "1");
    }

    setStatus("Searching this notebook...", false);

    var controller = null;
    if (global.AbortController) {
      controller = new global.AbortController();
      state.controller = controller;
    }

    global
      .fetch(cfg().searchEndpoint + "?" + params.toString(), {
        headers: { Accept: "application/json" },
        signal: controller ? controller.signal : undefined,
      })
      .then(function (response) {
        return response.json().then(function (body) {
          return { ok: response.ok, body: body };
        });
      })
      .then(function (payload) {
        if (state.controller === controller) {
          state.controller = null;
        }
        if (requestId !== state.requestId) return;
        if (!payload.ok) {
          clearResults();
          setStatus(payload.body.error || "Search failed.", true);
          return;
        }
        renderNotebookSearchResults(payload.body);
      })
      .catch(function (error) {
        if (state.controller === controller) {
          state.controller = null;
        }
        if (error && error.name === "AbortError") {
          return;
        }
        if (requestId !== state.requestId) return;
        clearResults();
        setStatus(error && error.message ? error.message : "Search failed.", true);
      });
  }

  function renderNotebookSearchResults(response) {
    var list = resultsList();
    if (!list) return;

    list.replaceChildren();
    state.results = response.results || [];
    state.activeIndex = state.results.length ? 0 : -1;

    if (!state.results.length) {
      setStatus("No matches found in this notebook.", false);
      return;
    }

    setStatus(
      response.truncated
        ? "Showing " + state.results.length + " of " + response.total + " matches."
        : response.total + " matches found.",
      false,
    );

    var fragment = document.createDocumentFragment();
    state.results.forEach(function (result, index) {
      var item = document.createElement("li");
      item.className = "nb-search-result-item";

      var button = document.createElement("button");
      button.type = "button";
      button.className = "nb-search-result" + (index === 0 ? " is-active" : "");
      button.setAttribute("aria-selected", index === 0 ? "true" : "false");
      button.addEventListener("click", function () {
        jumpToNotebookSearchResult(index);
      });
      button.addEventListener("mouseenter", function () {
        setActiveResult(index, false);
      });

      var meta = document.createElement("div");
      meta.className = "nb-search-result-meta";
      appendMetaBadge(meta, resultSurfaceLabel(result), "nb-search-badge" + (result.surface === "output" ? " is-output" : ""));
      appendMetaBadge(meta, resultCellTypeLabel(result), "nb-search-badge");
      if (response.mode === "fuzzy") {
        appendMetaBadge(meta, "score " + String(result.score || 0), "nb-search-badge is-fuzzy");
      }
      appendMetaText(meta, "cell " + String(Number(result.cell_position || 0) + 1));
      appendMetaText(meta, "line " + String(result.line_number || 0));
      appendMetaText(meta, "cols " + String(result.start_column || 0) + "-" + String(result.end_column || 0));
      button.appendChild(meta);

      var line = document.createElement("div");
      line.className = "nb-search-result-line";
      line.appendChild(buildHighlightedTextFragment(result.line_text, result.ranges));
      button.appendChild(line);

      item.appendChild(button);
      fragment.appendChild(item);
    });
    list.appendChild(fragment);
  }

  function appendMetaBadge(parent, text, className) {
    var badge = document.createElement("span");
    badge.className = className;
    badge.textContent = text;
    parent.appendChild(badge);
  }

  function appendMetaText(parent, text) {
    var span = document.createElement("span");
    span.textContent = text;
    parent.appendChild(span);
  }

  function clearNotebookSearchHighlights() {
    Array.prototype.slice
      .call(document.querySelectorAll(".cell-group.nb-search-active-cell"))
      .forEach(function (cell) {
        cell.classList.remove("nb-search-active-cell");
      });

    Array.prototype.slice
      .call(document.querySelectorAll(".nb-searchable-code"))
      .forEach(function (code) {
        unwrapMarks(code);
      });

    Array.prototype.slice
      .call(document.querySelectorAll('iframe[id^="output-"], iframe[id^="markdown-"]'))
      .forEach(function (iframe) {
        try {
          iframe.contentWindow.postMessage({ type: "notebook-search-clear" }, "*");
        } catch (error) {}
      });
  }

  function highlightSearchableCodeBlock(code, lineNumber, ranges) {
    if (!code) return;
    var target = highlightInlineRanges(
      code,
      normalizeText(code.textContent || ""),
      lineNumber,
      ranges || [],
      false,
    );
    if (target && target.scrollIntoView) {
      target.scrollIntoView({ block: "center", behavior: "smooth" });
    }
  }

  function scrollToCell(cellId) {
    var group = document.getElementById("cell-" + cellId);
    if (!group) return null;
    group.classList.add("nb-search-active-cell");
    if (group.scrollIntoView) {
      group.scrollIntoView({ block: "center", behavior: "smooth" });
    }
    return group;
  }

  function serializeRanges(ranges) {
    return (ranges || [])
      .map(function (range) {
        return range.start + ":" + range.end;
      })
      .join(",");
  }

  function openMarkdownSearchResult(result) {
    var iframe = document.getElementById("markdown-" + result.cell_id);
    if (!iframe) return;

    if (iframe.src && iframe.src.indexOf("/markdown/edit/") >= 0) {
      try {
        iframe.contentWindow.postMessage(
          {
            type: "notebook-search-highlight-editor",
            lineNumber: result.line_number,
            ranges: result.ranges || [],
          },
          "*",
        );
        return;
      } catch (error) {}
    }

    var url = new URL(cfg().markdownEditBase + result.cell_id, global.location.href);
    url.searchParams.set("search_line", String(result.line_number));
    url.searchParams.set("search_ranges", serializeRanges(result.ranges));
    url.searchParams.set("search_request", String(Date.now()));
    iframe.src = url.toString();
  }

  function highlightOutputResult(result) {
    var iframe = document.getElementById("output-" + result.cell_id);
    if (!iframe) return;

    var message = {
      type: "notebook-search-highlight",
      lineNumber: result.line_number,
      ranges: result.ranges || [],
    };
    try {
      iframe.contentWindow.postMessage(message, "*");
      global.setTimeout(function () {
        try {
          iframe.contentWindow.postMessage(message, "*");
        } catch (error) {}
      }, 90);
      global.setTimeout(function () {
        try {
          iframe.contentWindow.postMessage(message, "*");
        } catch (error) {}
      }, 240);
    } catch (error) {}
  }

  function jumpToNotebookSearchResult(index) {
    var result = state.results[index];
    if (!result) return;

    setActiveResult(index, true);
    clearNotebookSearchHighlights();
    scrollToCell(result.cell_id);

    if (result.cell_type === "markdown") {
      openMarkdownSearchResult(result);
      return;
    }
    if (result.surface === "output") {
      highlightOutputResult(result);
      return;
    }
    highlightSearchableCodeBlock(
      document.getElementById("content-" + result.cell_id),
      result.line_number,
      result.ranges || [],
    );
  }

  function applyInitialState() {
    var stored = readStoredState();
    var searchInput = input();
    if (searchInput) {
      searchInput.value = stored.query;
    }
    if (surfaceSelect()) {
      surfaceSelect().value = stored.surface;
    }
    if (cellTypeSelect()) {
      cellTypeSelect().value = stored.cellType;
    }
    if (caseCheckbox()) {
      caseCheckbox().checked = !!stored.caseSensitive;
    }
    setMode(stored.mode);
  }

  function bindNotebookSearchUI() {
    var searchInput = input();
    var searchCloseButton = document.getElementById("close-notebook-search");

    applyInitialState();
    updateToolbarState();

    if (toolbarButton()) {
      toolbarButton().addEventListener("click", function () {
        if (isNotebookSearchOpen()) {
          closeNotebookSearch();
        } else {
          openNotebookSearch();
        }
      });
    }

    if (searchCloseButton) {
      searchCloseButton.addEventListener("click", function () {
        closeNotebookSearch();
      });
    }

    modeButtons().forEach(function (button) {
      button.addEventListener("click", function () {
        setMode(button.getAttribute("data-mode"));
        writeStoredState();
        queueNotebookSearch(0);
      });
    });

    if (searchInput) {
      searchInput.addEventListener("input", function () {
        writeStoredState();
        queueNotebookSearch();
      });
      searchInput.addEventListener("keydown", function (event) {
        if (event.key === "ArrowDown") {
          event.preventDefault();
          moveActiveResult(1);
          return;
        }
        if (event.key === "ArrowUp") {
          event.preventDefault();
          moveActiveResult(-1);
          return;
        }
        if (event.key === "Enter") {
          event.preventDefault();
          if (state.activeIndex >= 0) {
            jumpToNotebookSearchResult(state.activeIndex);
          } else {
            runNotebookSearch();
          }
          return;
        }
        if (event.key === "Escape") {
          event.preventDefault();
          closeNotebookSearch();
        }
      });
    }

    [surfaceSelect(), cellTypeSelect(), caseCheckbox()].forEach(function (control) {
      if (!control) return;
      control.addEventListener("change", function () {
        writeStoredState();
        queueNotebookSearch(0);
      });
    });
  }

  global.openNotebookSearch = openNotebookSearch;
  global.closeNotebookSearch = closeNotebookSearch;
  global.isNotebookSearchOpen = isNotebookSearchOpen;
  global.jumpToNotebookSearchResult = jumpToNotebookSearchResult;

  document.addEventListener("DOMContentLoaded", bindNotebookSearchUI);
})(window, document);
