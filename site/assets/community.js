/* Snapmap+ site — Community section behavior.
   Renders GitHub Discussions (via the community service) natively on the site:
   1. Index — search, sort, category tabs, the forum list (community.html).
   2. Post view — the post, its action bar, and the discussion thread (community-post.html).
   3. Composer — a rich-text editor for writing/editing posts (community-compose.html).
   Shared foundations: an inline SVG icon set, in-site modals + toasts (never browser
   confirm/alert), a WYSIWYG editor that serializes to markdown (Discussions' storage format),
   and an opaque-session sign-in. Each feature activates only when its markup is present. */

(function () {
  "use strict";

  var WORKER = "https://snapmap-plus-community.doom-snapmap.workers.dev";
  var REPO_URL = "https://github.com/doom-snapmap/snapmap-plus";
  var DISCUSSIONS_URL = REPO_URL + "/discussions";
  var SESSION_KEY = "smp_session";
  var DRAFT_KEY = "smp_draft2";

  /* ---------- utils ---------- */

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function fmtDate(iso) {
    var d = new Date(iso);
    var days = (Date.now() - d.getTime()) / 86400000;
    if (days < 1) {
      var hours = Math.floor(days * 24);
      return hours <= 1 ? "just now" : hours + " hours ago";
    }
    if (days < 30) {
      var n = Math.floor(days);
      return n === 1 ? "yesterday" : n + " days ago";
    }
    return d.toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" });
  }

  function debounce(fn, ms) {
    var t;
    return function () {
      var args = arguments, self = this;
      clearTimeout(t);
      t = setTimeout(function () { fn.apply(self, args); }, ms);
    };
  }

  /* ---------- icons (inline SVG, stroke = currentColor) ---------- */

  var ICONS = {
    search: '<circle cx="11" cy="11" r="7"/><path d="m21 21-4.3-4.3"/>',
    up: '<path d="M12 19V5"/><path d="m5 12 7-7 7 7"/>',
    comment: '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>',
    reply: '<polyline points="9 17 4 12 9 7"/><path d="M20 18v-2a4 4 0 0 0-4-4H4"/>',
    edit: '<path d="M17 3a2.8 2.8 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5Z"/>',
    trash: '<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>',
    x: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
    chevron: '<polyline points="6 9 12 15 18 9"/>',
    link: '<path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>',
    image: '<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.09-3.09a2 2 0 0 0-2.82 0L6 21"/>',
    bold: '<path d="M6 4h8a4 4 0 0 1 0 8H6z"/><path d="M6 12h9a4 4 0 0 1 0 8H6z"/>',
    italic: '<line x1="19" y1="4" x2="10" y2="4"/><line x1="14" y1="20" x2="5" y2="20"/><line x1="15" y1="4" x2="9" y2="20"/>',
    strike: '<path d="M16 4H9a3 3 0 0 0-2.83 4"/><path d="M14 12a4 4 0 0 1 0 8H6"/><line x1="4" y1="12" x2="20" y2="12"/>',
    quote: '<path d="M3 21c3-1 5-3.5 5-8V5H3v8h4"/><path d="M13 21c3-1 5-3.5 5-8V5h-5v8h4"/>',
    code: '<polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/>',
    listUl: '<line x1="9" y1="6" x2="21" y2="6"/><line x1="9" y1="12" x2="21" y2="12"/><line x1="9" y1="18" x2="21" y2="18"/><line x1="4" y1="6" x2="4.01" y2="6"/><line x1="4" y1="12" x2="4.01" y2="12"/><line x1="4" y1="18" x2="4.01" y2="18"/>',
    listOl: '<line x1="10" y1="6" x2="21" y2="6"/><line x1="10" y1="12" x2="21" y2="12"/><line x1="10" y1="18" x2="21" y2="18"/><path d="M4 6h1v4"/><path d="M4 10h2"/><path d="M6 18H4c0-1 2-2 2-3s-1-1.5-2-1"/>',
    external: '<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>',
    alert: '<path d="m21.73 18-8-14a2 2 0 0 0-3.46 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/>',
    check: '<polyline points="20 6 9 17 4 12"/>',
    plus: '<path d="M5 12h14"/><path d="M12 5v14"/>',
    signout: '<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/>',
  };

  function ic(name, cls) {
    return '<svg class="ic' + (cls ? " " + cls : "") + '" viewBox="0 0 24 24" aria-hidden="true">' + ICONS[name] + "</svg>";
  }

  /* ---------- session + API ---------- */

  function captureSession() {
    if (location.hash.indexOf("#session=") === 0) {
      var sid = location.hash.slice("#session=".length);
      if (/^[0-9a-f]{64}$/i.test(sid)) {
        try { localStorage.setItem(SESSION_KEY, sid); } catch (e) {}
      }
      history.replaceState(null, "", location.pathname + location.search);
    } else if (location.hash === "#login_error") {
      history.replaceState(null, "", location.pathname + location.search);
      window.__smpLoginError = true;
    }
  }

  function sessionId() {
    try { return localStorage.getItem(SESSION_KEY); } catch (e) { return null; }
  }

  function clearSession() {
    try { localStorage.removeItem(SESSION_KEY); } catch (e) {}
  }

  function authHeaders() {
    var sid = sessionId();
    return sid ? { "Authorization": "Bearer " + sid } : {};
  }

  function api(path, opts) {
    opts = opts || {};
    var headers = opts.headers || {};
    var ah = authHeaders();
    Object.keys(ah).forEach(function (k) { headers[k] = ah[k]; });
    if (opts.json) {
      headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(opts.json);
    }
    return fetch(WORKER + path, {
      method: opts.method || "GET",
      headers: headers,
      body: opts.body || undefined,
    }).then(function (r) {
      if (r.status === 401 && sessionId()) clearSession();   // stale session — drop it
      return r.json().then(function (data) { return { ok: r.ok, status: r.status, data: data }; });
    }).catch(function () { return { ok: false, status: 0, data: null }; });
  }

  var mePromise = null;
  function me() {
    if (!sessionId()) return Promise.resolve(null);
    if (!mePromise) {
      mePromise = api("/auth/me").then(function (r) { return r.ok ? r.data : null; });
    }
    return mePromise;
  }

  /* ---------- modal + toast (in-site; never browser confirm/alert) ---------- */

  var modalEl = null;

  function buildModal() {
    if (modalEl) return modalEl;
    modalEl = document.createElement("dialog");
    modalEl.className = "smp-modal";
    document.body.appendChild(modalEl);
    return modalEl;
  }

  /* opts: { title, message, confirmLabel, cancelLabel, danger, input:{label?,placeholder,value} } */
  function openModal(opts) {
    var dlg = buildModal();
    return new Promise(function (resolve) {
      dlg.innerHTML =
        '<div class="modal-body">' +
          '<h3 class="modal-title' + (opts.danger ? " is-danger" : "") + '">' +
            ic(opts.danger ? "alert" : "check") + esc(opts.title) + "</h3>" +
          (opts.message ? '<p class="modal-msg">' + esc(opts.message) + "</p>" : "") +
          (opts.input ? '<input type="text" placeholder="' + esc(opts.input.placeholder || "") +
            '" value="' + esc(opts.input.value || "") + '" aria-label="' + esc(opts.title) + '">' : "") +
        "</div>" +
        '<div class="modal-actions">' +
          '<button class="btn btn-ghost" type="button" data-m-cancel>' + esc(opts.cancelLabel || "Cancel") + "</button>" +
          '<button class="btn ' + (opts.danger ? "btn-danger" : "btn-primary") + '" type="button" data-m-ok>' +
            esc(opts.confirmLabel || "OK") + "</button>" +
        "</div>";

      var input = dlg.querySelector("input");
      var done = false;
      function finish(val) {
        if (done) return;
        done = true;
        dlg.close();
        resolve(val);
      }
      dlg.querySelector("[data-m-cancel]").addEventListener("click", function () { finish(opts.input ? null : false); });
      dlg.querySelector("[data-m-ok]").addEventListener("click", function () {
        finish(opts.input ? (input.value.trim() || null) : true);
      });
      if (input) {
        input.addEventListener("keydown", function (e) {
          if (e.key === "Enter") { e.preventDefault(); finish(input.value.trim() || null); }
        });
      }
      dlg.addEventListener("cancel", function () { finish(opts.input ? null : false); }, { once: true });
      dlg.showModal();
      if (input) { input.focus(); input.select(); }
    });
  }

  function confirmModal(opts) { return openModal(opts); }
  function promptModal(opts) { opts.input = opts.input || { placeholder: "" }; return openModal(opts); }

  var toastHost = null;
  function toast(msg, isError) {
    if (!toastHost) {
      toastHost = document.createElement("div");
      toastHost.className = "toast-host";
      document.body.appendChild(toastHost);
    }
    var el = document.createElement("div");
    el.className = "toast" + (isError ? " is-error" : "");
    el.setAttribute("role", "status");
    el.innerHTML = ic(isError ? "alert" : "check") + "<span>" + esc(msg) + "</span>";
    toastHost.appendChild(el);
    setTimeout(function () { el.remove(); }, 4200);
  }

  function apiError(r, fallback) {
    return (r && r.data && r.data.error) ? r.data.error : fallback;
  }

  /* ---------- HTML → markdown (the editor's output; Discussions store markdown) ---------- */

  function escapeMd(t) {
    return t.replace(/([\\`*_~\[\]])/g, "\\$1");
  }

  function inlineMd(node) {
    var out = "";
    var kids = node.childNodes;
    for (var i = 0; i < kids.length; i++) {
      var n = kids[i];
      if (n.nodeType === 3) { out += escapeMd(n.nodeValue.replace(/\s+/g, " ")); continue; }
      if (n.nodeType !== 1) continue;
      var tag = n.tagName;
      if (tag === "BR") { out += "\n"; continue; }
      if (tag === "SCRIPT" || tag === "STYLE" || tag === "SVG" || tag === "BUTTON" || tag === "TEMPLATE") continue;
      if (n.classList && n.classList.contains("video-embed")) continue;
      if (tag === "STRONG" || tag === "B") { out += "**" + inlineMd(n) + "**"; continue; }
      if (tag === "EM" || tag === "I") { out += "*" + inlineMd(n) + "*"; continue; }
      if (tag === "DEL" || tag === "S" || tag === "STRIKE") { out += "~~" + inlineMd(n) + "~~"; continue; }
      if (tag === "CODE") { out += "`" + n.textContent + "`"; continue; }
      if (tag === "A") {
        var href = n.getAttribute("href") || "";
        if (n.classList && n.classList.contains("anchor")) continue;   // GitHub heading permalinks
        var text = inlineMd(n);
        out += (text.trim() === href || !text.trim()) ? href : "[" + text + "](" + href + ")";
        continue;
      }
      if (tag === "IMG") {
        out += "![" + (n.getAttribute("alt") || "screenshot") + "](" + (n.getAttribute("src") || "") + ")";
        continue;
      }
      if (tag === "INPUT") { continue; }   // task-list checkboxes handled at the <li> level
      out += inlineMd(n);
    }
    return out;
  }

  function listMd(node, ordered, indent) {
    var out = [];
    var idx = 1;
    var kids = node.children;
    for (var i = 0; i < kids.length; i++) {
      var li = kids[i];
      if (li.tagName !== "LI") continue;
      var marker = ordered ? (idx + ". ") : "- ";
      var box = li.querySelector && li.querySelector(':scope > input[type="checkbox"], :scope > p > input[type="checkbox"]');
      if (box) marker += box.checked ? "[x] " : "[ ] ";
      var sub = "";
      var clone = li.cloneNode(true);
      var nested = clone.querySelectorAll(":scope > ul, :scope > ol");
      for (var j = 0; j < nested.length; j++) {
        sub += "\n" + listMd(nested[j], nested[j].tagName === "OL", indent + "  ");
        nested[j].remove();
      }
      out.push(indent + marker + inlineMd(clone).trim() + sub);
      idx++;
    }
    return out.join("\n");
  }

  function blockMd(node) {
    var blocks = [];
    var kids = node.childNodes;
    for (var i = 0; i < kids.length; i++) {
      var n = kids[i];
      if (n.nodeType === 3) {
        var t = escapeMd(n.nodeValue.replace(/\s+/g, " ")).trim();
        if (t) blocks.push(t);
        continue;
      }
      if (n.nodeType !== 1) continue;
      var tag = n.tagName;
      if (tag === "SCRIPT" || tag === "STYLE" || tag === "SVG" || tag === "BUTTON" || tag === "TEMPLATE") continue;
      if (n.classList && n.classList.contains("video-embed")) continue;
      if (/^H[1-6]$/.test(tag)) {
        blocks.push("#".repeat(+tag[1]) + " " + inlineMd(n).trim());
      } else if (tag === "P") {
        var p = inlineMd(n).trim();
        if (p) blocks.push(p);
      } else if (tag === "UL" || tag === "OL") {
        blocks.push(listMd(n, tag === "OL", ""));
      } else if (tag === "BLOCKQUOTE") {
        blocks.push(blockMd(n).split("\n").map(function (l) { return "> " + l; }).join("\n"));
      } else if (tag === "PRE") {
        var lang = "";
        var cls = (n.className || "") + " " + ((n.parentElement && n.parentElement.className) || "");
        var lm = cls.match(/highlight-source-(\w+)/) || cls.match(/language-(\w+)/);
        if (lm) lang = lm[1];
        blocks.push("```" + lang + "\n" + n.textContent.replace(/\n$/, "") + "\n```");
      } else if (tag === "HR") {
        blocks.push("---");
      } else if (tag === "TABLE") {
        blocks.push(n.textContent.trim());   // tables degrade to text (the editor never produces them)
      } else if (tag === "DIV" || tag === "SECTION" || tag === "ARTICLE") {
        var inner = blockMd(n);
        if (inner) blocks.push(inner);
      } else {
        var flow = inlineMd(n).trim();
        if (flow) blocks.push(flow);
      }
    }
    return blocks.join("\n\n");
  }

  function htmlToMarkdown(root) {
    return blockMd(root).replace(/\n{3,}/g, "\n\n").trim();
  }

  /* ---------- rich text editor ---------- */

  function uploadImageFile(file, onDone, onNote) {
    if (!file) return;
    onNote("Uploading " + (file.name || "image") + "…");
    fetch(WORKER + "/media/upload", { method: "POST", headers: authHeaders(), body: file })
      .then(function (r) { return r.json().then(function (d) { return { ok: r.ok, data: d }; }); })
      .then(function (r) {
        if (r.ok && r.data.url) { onNote(""); onDone(r.data.url); }
        else onNote("Upload failed: " + apiError(r, "error"));
      })
      .catch(function () { onNote("Upload failed — check your connection."); });
  }

  /* opts: { compact, placeholder, initialHTML } */
  function createEditor(opts) {
    opts = opts || {};
    var root = document.createElement("div");
    root.className = "rte" + (opts.compact ? " rte-compact" : "");

    var TOOLS = opts.compact
      ? ["bold", "italic", "|", "code", "link", "image"]
      : ["bold", "italic", "strike", "|", "h2", "h3", "|", "listUl", "listOl", "|", "quote", "code", "|", "link", "image"];

    var LABELS = {
      bold: "Bold", italic: "Italic", strike: "Strikethrough", h2: "Heading", h3: "Subheading",
      listUl: "Bullet list", listOl: "Numbered list", quote: "Quote", code: "Code block",
      link: "Insert link", image: "Insert screenshot",
    };

    var tb = document.createElement("div");
    tb.className = "rte-toolbar";
    tb.setAttribute("role", "toolbar");
    TOOLS.forEach(function (t) {
      if (t === "|") {
        var sep = document.createElement("span");
        sep.className = "rte-sep";
        tb.appendChild(sep);
        return;
      }
      var b = document.createElement("button");
      b.type = "button";
      b.className = "rte-btn";
      b.setAttribute("data-t", t);
      b.setAttribute("title", LABELS[t]);
      b.setAttribute("aria-label", LABELS[t]);
      b.innerHTML = (t === "h2" || t === "h3") ? t.toUpperCase() : ic(t, "ic-sm");
      tb.appendChild(b);
    });

    var area = document.createElement("div");
    area.className = "rte-area";
    area.contentEditable = "true";
    area.setAttribute("data-placeholder", opts.placeholder || "Write here…");
    if (opts.initialHTML) area.innerHTML = opts.initialHTML;

    var note = document.createElement("div");
    note.className = "rte-note";
    note.setAttribute("aria-live", "polite");

    var file = document.createElement("input");
    file.type = "file";
    file.accept = "image/png,image/jpeg,image/gif,image/webp";
    file.hidden = true;

    root.appendChild(tb);
    root.appendChild(area);
    root.appendChild(file);

    function setNote(t) { note.textContent = t; }

    function exec(cmd, val) {
      area.focus();
      document.execCommand(cmd, false, val || null);
      refreshState();
    }

    function curBlock() {
      var v = "";
      try { v = String(document.queryCommandValue("formatBlock") || "").toLowerCase(); } catch (e) {}
      return v.replace(/[<>]/g, "");
    }

    function toggleBlock(tag) {
      exec("formatBlock", curBlock() === tag ? "<p>" : "<" + tag + ">");
    }

    function insertImage(url) {
      area.focus();
      exec("insertHTML", '<img src="' + esc(url) + '" alt="screenshot">');
    }

    var actions = {
      bold: function () { exec("bold"); },
      italic: function () { exec("italic"); },
      strike: function () { exec("strikeThrough"); },
      h2: function () { toggleBlock("h2"); },
      h3: function () { toggleBlock("h3"); },
      listUl: function () { exec("insertUnorderedList"); },
      listOl: function () { exec("insertOrderedList"); },
      quote: function () { toggleBlock("blockquote"); },
      code: function () { toggleBlock("pre"); },
      link: function () {
        var sel = window.getSelection();
        var saved = sel.rangeCount ? sel.getRangeAt(0).cloneRange() : null;
        promptModal({ title: "Insert link", input: { placeholder: "https://…" }, confirmLabel: "Insert" })
          .then(function (url) {
            if (!url) return;
            if (!/^https?:\/\//i.test(url)) url = "https://" + url;
            area.focus();
            if (saved) { sel.removeAllRanges(); sel.addRange(saved); }
            if (saved && !saved.collapsed) document.execCommand("createLink", false, url);
            else document.execCommand("insertHTML", false, '<a href="' + esc(url) + '">' + esc(url) + "</a>");
          });
      },
      image: function () { file.click(); },
    };

    tb.addEventListener("mousedown", function (e) { e.preventDefault(); });   // keep the selection
    tb.addEventListener("click", function (e) {
      var b = e.target.closest("[data-t]");
      if (b) actions[b.getAttribute("data-t")]();
    });

    file.addEventListener("change", function () {
      uploadImageFile(this.files[0], insertImage, setNote);
      this.value = "";
    });
    area.addEventListener("dragover", function (e) { e.preventDefault(); });
    area.addEventListener("drop", function (e) {
      if (e.dataTransfer.files.length) {
        e.preventDefault();
        uploadImageFile(e.dataTransfer.files[0], insertImage, setNote);
      }
    });
    area.addEventListener("paste", function (e) {
      var files = e.clipboardData && e.clipboardData.files;
      if (files && files.length) {
        e.preventDefault();
        uploadImageFile(files[0], insertImage, setNote);
        return;
      }
      e.preventDefault();   // paste as plain text — formatting comes from the toolbar
      var text = e.clipboardData.getData("text/plain");
      if (text) document.execCommand("insertText", false, text);
    });

    function refreshState() {
      var states = { bold: 0, italic: 0, strike: 0 };
      try {
        states.bold = document.queryCommandState("bold");
        states.italic = document.queryCommandState("italic");
        states.strike = document.queryCommandState("strikeThrough");
      } catch (e) {}
      var blk = curBlock();
      tb.querySelectorAll("[data-t]").forEach(function (b) {
        var t = b.getAttribute("data-t");
        var on = false;
        if (t === "bold" || t === "italic" || t === "strike") on = !!states[t];
        else if (t === "h2" || t === "h3") on = blk === t;
        else if (t === "quote") on = blk === "blockquote";
        else if (t === "code") on = blk === "pre";
        b.classList.toggle("on", on);
      });
    }
    document.addEventListener("selectionchange", function () {
      if (area.contains((window.getSelection() || {}).anchorNode)) refreshState();
    });

    return {
      root: root,
      area: area,
      note: note,
      isEmpty: function () { return !area.textContent.trim() && !area.querySelector("img"); },
      getHTML: function () { return area.innerHTML; },
      setHTML: function (h) { area.innerHTML = h; },
      getMarkdown: function () { return htmlToMarkdown(area); },
      focus: function () { area.focus(); },
    };
  }

  /* ---------- video embeds (rendered posts) ---------- */

  function videoEmbed(href) {
    var m = href.match(/^https?:\/\/(?:www\.)?youtube\.com\/watch\?(?:.*&)?v=([\w-]{6,20})/) ||
            href.match(/^https?:\/\/youtu\.be\/([\w-]{6,20})/);
    if (m) return "https://www.youtube-nocookie.com/embed/" + m[1];
    m = href.match(/^https?:\/\/(?:www\.)?streamable\.com\/([a-z0-9]{4,12})$/i);
    if (m) return "https://streamable.com/e/" + m[1];
    return null;
  }

  function enhanceVideos(container) {
    if (!container) return;
    Array.prototype.slice.call(container.querySelectorAll("a[href]")).forEach(function (a) {
      var src = videoEmbed(a.getAttribute("href") || "");
      if (!src) return;
      if (a.textContent.trim() !== a.getAttribute("href")) return;   // only bare links
      var frame = document.createElement("div");
      frame.className = "video-embed";
      var iframe = document.createElement("iframe");
      iframe.src = src;
      iframe.setAttribute("allowfullscreen", "");
      iframe.setAttribute("loading", "lazy");
      iframe.setAttribute("title", "Embedded video");
      iframe.setAttribute("allow", "encrypted-media; picture-in-picture");
      frame.appendChild(iframe);
      a.parentNode.insertBefore(frame, a.nextSibling);
    });
  }

  /* ---------- shared rendering ---------- */

  function avatarImg(a, px) {
    if (!a || !a.avatarUrl) return "";
    var sized = a.avatarUrl + (a.avatarUrl.indexOf("?") >= 0 ? "&" : "?") + "s=" + (px * 2);
    return '<img class="avatar" src="' + esc(sized) + '" width="' + px + '" height="' + px + '" alt="" loading="lazy">';
  }

  function authorLink(a) {
    var login = esc(a && a.login || "ghost");
    return a && a.url ? '<a href="' + esc(a.url) + '" rel="noopener">' + login + "</a>" : login;
  }

  function tagChips(d) {
    var html = "";
    if (d.category) html += '<span class="tag-chip">' + esc(d.category.name) + "</span>";
    (d.tags || []).forEach(function (t) {
      html += '<span class="tag-chip tag-chip-label">' + esc(t.name) + "</span>";
    });
    return html;
  }

  function skeletonRows(n) {
    var html = "";
    for (var i = 0; i < n; i++) {
      html += '<div class="sk-row"><div class="sk-line sk-w60"></div><div class="sk-line sk-w35"></div></div>';
    }
    return html;
  }

  function failNote(host, what) {
    host.innerHTML =
      '<div class="load-note">Could not load ' + what + " right now. " +
      'The community also lives on <a href="' + DISCUSSIONS_URL + '">GitHub Discussions</a>.</div>';
  }

  function signInButtonHtml(label) {
    return '<a class="btn btn-ghost btn-signin" href="' + WORKER + '/auth/login" rel="nofollow">' +
      (label || "Sign in with GitHub") + "</a>";
  }

  /* ---------- auth widget ---------- */

  function initAuthWidget() {
    var host = document.getElementById("community-auth");
    if (!host) return;
    if (window.__smpLoginError) {
      host.innerHTML = '<span class="auth-error">Sign-in didn’t complete — try again.</span> ' + signInButtonHtml();
      return;
    }
    me().then(function (user) {
      if (!user) { host.innerHTML = signInButtonHtml(); return; }
      host.innerHTML =
        '<span class="auth-chip">' + avatarImg({ avatarUrl: user.avatar }, 26) +
        '<span class="auth-name">' + esc(user.login) + "</span>" +
        '<button class="auth-signout" type="button" title="Sign out">' + ic("signout", "ic-sm") + "Sign out</button></span>";
      host.querySelector(".auth-signout").addEventListener("click", function () {
        api("/auth/logout", { method: "POST" }).then(function () {
          clearSession();
          location.reload();
        });
      });
    });
  }

  /* ---------- 1. index ---------- */

  function initIndex() {
    var list = document.getElementById("community-list");
    if (!list) return;
    var tabs = document.getElementById("community-tabs");
    var more = document.getElementById("community-more");
    var searchBox = document.getElementById("community-searchbox");
    var searchInput = document.getElementById("community-search");
    var searchClear = document.getElementById("community-search-clear");
    var sortSel = document.getElementById("community-sort");
    var params = new URLSearchParams(location.search);
    var activeCat = params.get("category") || "";
    var query = "";
    var gen = 0;   // request generation — stale responses are dropped

    api("/community/categories").then(function (r) {
      if (!r.ok || !tabs) return;
      var html = '<a class="' + (activeCat ? "" : "active") + '" href="community.html">All posts</a>';
      r.data.categories.forEach(function (c) {
        html += '<a class="' + (c.slug === activeCat ? "active" : "") +
          '" href="community.html?category=' + esc(encodeURIComponent(c.slug)) + '">' + esc(c.name) + "</a>";
      });
      tabs.innerHTML = html;
    });

    function rowHtml(d, showUpdated) {
      var when = showUpdated && d.updatedAt
        ? "active " + fmtDate(d.updatedAt)
        : fmtDate(d.createdAt);
      /* the whole row is ONE link — never nest the author's profile link inside it (nested
         anchors are invalid HTML; the parser splits the row apart). Profile links live on the
         post page. */
      return (
        '<a class="forum-row" href="community-post.html?n=' + d.number + '">' +
          '<div class="forum-main">' +
            '<h2 class="forum-title"><span>' + esc(d.title) + "</span>" + tagChips(d) + "</h2>" +
            '<div class="forum-meta">' + avatarImg(d.author, 16) +
              "<span>" + esc(d.author && d.author.login || "ghost") + "</span>" +
              '<span class="sep">&middot;</span><time datetime="' + esc(d.createdAt) + '">' + when + "</time>" +
            "</div>" +
          "</div>" +
          '<div class="forum-stats">' +
            '<span class="stat" title="Upvotes and reactions">' + ic("up", "ic-sm") + d.reactionCount + "</span>" +
            '<span class="stat" title="Comments">' + ic("comment", "ic-sm") + d.commentCount + "</span>" +
          "</div>" +
        "</a>"
      );
    }

    function emptyHtml() {
      if (query) {
        return '<div class="empty-state"><h3>No matches</h3>' +
          "<p>Nothing found for “" + esc(query) + "”. Try a different term.</p></div>";
      }
      return '<div class="empty-state"><h3>No posts yet</h3>' +
        "<p>This is where the community's guides and tips will live.</p>" +
        '<a class="btn btn-primary" href="community-compose.html">Write the first post</a></div>';
    }

    function load(after, append) {
      var g = ++gen;
      if (!append) {
        list.innerHTML = skeletonRows(3);
        if (more) more.innerHTML = "";
      }
      var path;
      if (query) {
        path = "/community/search?q=" + encodeURIComponent(query);
      } else {
        var qs = [];
        if (activeCat) qs.push("category=" + encodeURIComponent(activeCat));
        if (sortSel && sortSel.value === "active") qs.push("sort=active");
        if (after) qs.push("after=" + encodeURIComponent(after));
        path = "/community/discussions" + (qs.length ? "?" + qs.join("&") : "");
      }
      api(path).then(function (r) {
        if (g !== gen) return;   // superseded by a newer search/sort
        if (!r.ok) { if (!append) failNote(list, "community posts"); return; }
        var data = r.data;
        if (!data.discussions.length && !append) {
          list.innerHTML = emptyHtml();
          return;
        }
        var showUpdated = !query && sortSel && sortSel.value === "active";
        var html = data.discussions.map(function (d) { return rowHtml(d, showUpdated); }).join("");
        if (append) list.insertAdjacentHTML("beforeend", html);
        else list.innerHTML = html;
        if (more) {
          if (!query && data.pageInfo && data.pageInfo.hasNextPage) {
            more.innerHTML = '<button class="btn btn-ghost" type="button">Show more posts</button>';
            more.querySelector("button").addEventListener("click", function () {
              this.disabled = true;
              load(data.pageInfo.endCursor, true);
            });
          } else {
            more.innerHTML = "";
          }
        }
      });
    }

    if (searchInput) {
      var run = debounce(function () {
        var q = searchInput.value.trim();
        if (q === query) return;
        query = q.length >= 2 ? q : "";
        if (searchBox) searchBox.classList.toggle("has-query", !!searchInput.value);
        if (sortSel) sortSel.disabled = !!query;
        load(null, false);
      }, 300);
      searchInput.addEventListener("input", run);
      if (searchClear) {
        searchClear.addEventListener("click", function () {
          searchInput.value = "";
          run();
          searchInput.focus();
        });
      }
    }
    if (sortSel) sortSel.addEventListener("change", function () { load(null, false); });

    load(null, false);
  }

  /* ---------- 2. post view ---------- */

  function initPost() {
    var host = document.getElementById("community-post");
    if (!host) return;
    var thread = document.getElementById("community-comments");
    var params = new URLSearchParams(location.search);
    var n = parseInt(params.get("n") || "", 10);
    if (!n) { location.replace("community.html"); return; }

    var visibleTop = 10;                 // top-level comments shown
    var expandedReplies = {};            // comment id -> true (all replies shown)

    function upvoteBtn(subjectId, count, quiet) {
      return '<button class="' + (quiet ? "c-act" : "action-btn") + '" type="button" data-react="' +
        esc(subjectId) + '" title="Upvote">' + ic("up", "ic-sm") +
        (quiet ? '<span class="count">' + count + "</span>" : "<span>Upvote</span>" + (count ? '<span class="count">' + count + "</span>" : "")) +
        "</button>";
    }

    function upCount(reactions) {
      var up = (reactions || []).find(function (r) { return r.content === "THUMBS_UP"; });
      return up ? up.count : 0;
    }

    function render() {
      Promise.all([api("/community/discussions/" + n), me()]).then(function (results) {
        var r = results[0], user = results[1];
        if (!r.ok) { failNote(host, "this post"); return; }
        var d = r.data;
        document.title = d.title + " — Snapmap+ Community";
        var mine = function (a) { return !!(user && a && a.login === user.login); };

        /* --- the post --- */
        host.innerHTML =
          '<div class="post-head"><div class="section-head"><h1 class="section-title">' + esc(d.title) + "</h1></div></div>" +
          '<div class="post-meta post-meta-page">' + avatarImg(d.author, 20) +
            '<span class="author-name">' + authorLink(d.author) + "</span>" +
            '<span class="sep">&middot;</span><time datetime="' + esc(d.createdAt) + '">' + fmtDate(d.createdAt) + "</time>" +
            '<span class="sep">&middot;</span>' + tagChips(d) +
          "</div>" +
          '<div class="post-body">' + d.bodyHTML + "</div>" +
          '<div class="post-actions">' +
            upvoteBtn(d.id, upCount(d.reactions), false) +
            '<a class="action-btn action-quiet" href="#discussion">' + ic("comment", "ic-sm") +
              "<span>Comments</span>" + '<span class="count">' + d.commentCount + "</span></a>" +
            (mine(d.author)
              ? '<a class="action-btn action-quiet" href="community-compose.html?edit=' + n + '">' + ic("edit", "ic-sm") + "<span>Edit</span></a>" +
                '<button class="action-btn action-quiet action-danger" type="button" data-del-post>' + ic("trash", "ic-sm") + "<span>Delete</span></button>"
              : "") +
            '<a class="gh-link" href="' + esc(d.url) + '" rel="noopener">View on GitHub ' + ic("external", "ic-sm") + "</a>" +
          "</div>";
        enhanceVideos(host.querySelector(".post-body"));

        var delPost = host.querySelector("[data-del-post]");
        if (delPost) {
          delPost.addEventListener("click", function () {
            confirmModal({
              title: "Delete this post?",
              message: "The post and its comments are removed for everyone. This can't be undone.",
              confirmLabel: "Delete post",
              danger: true,
            }).then(function (yes) {
              if (!yes) return;
              delPost.disabled = true;
              api("/community/discussions/" + n, { method: "DELETE" }).then(function (r2) {
                if (r2.ok) { toast("Post deleted."); location.href = "community.html"; }
                else { delPost.disabled = false; toast("Delete failed: " + apiError(r2, "try again"), true); }
              });
            });
          });
        }

        if (!thread) return;

        /* --- the thread: composer docked on top, then comments --- */
        function commentBlock(c, isReply) {
          return (
            '<article class="comment" data-cid="' + esc(c.id) + '">' +
              '<div class="comment-head">' + avatarImg(c.author, 22) +
                '<span class="c-author">' + authorLink(c.author) + "</span>" +
                '<time datetime="' + esc(c.createdAt) + '">' + fmtDate(c.createdAt) + "</time>" +
              "</div>" +
              '<div class="comment-body">' + c.bodyHTML + "</div>" +
              '<div class="comment-actions">' +
                upvoteBtn(c.id, upCount(c.reactions), true) +
                (!isReply && user ? '<button class="c-act" type="button" data-reply>' + ic("reply", "ic-sm") + "Reply</button>" : "") +
                (mine(c.author)
                  ? '<button class="c-act" type="button" data-edit>' + ic("edit", "ic-sm") + "Edit</button>" +
                    '<button class="c-act danger" type="button" data-del>' + ic("trash", "ic-sm") + "Delete</button>"
                  : "") +
              "</div>" +
            "</article>"
          );
        }

        var html =
          '<div class="thread-head" id="discussion"><h2>Discussion</h2>' +
          '<span class="thread-count">' + d.commentCount + (d.commentCount === 1 ? " comment" : " comments") + "</span></div>" +
          '<div id="thread-composer" class="composer-box"></div>' +
          '<div class="comment-list">';

        var shown = d.comments.slice(0, visibleTop);
        shown.forEach(function (c) {
          html += commentBlock(c, false);
          var replies = c.replies || [];
          var showAll = !!expandedReplies[c.id];
          var visible = showAll ? replies : replies.slice(0, 2);
          if (replies.length) {
            html += '<div class="replies" data-replies="' + esc(c.id) + '">';
            visible.forEach(function (rp) { html += commentBlock(rp, true); });
            if (!showAll && replies.length > 2) {
              html += '<button class="thread-more" type="button" data-more-replies="' + esc(c.id) + '">' +
                ic("chevron", "ic-sm") + "Show " + (replies.length - 2) + " more " +
                (replies.length - 2 === 1 ? "reply" : "replies") + "</button>";
            }
            html += '<div class="reply-slot" data-slot="' + esc(c.id) + '"></div></div>';
          } else {
            html += '<div class="replies" style="display:none" data-replies="' + esc(c.id) + '">' +
              '<div class="reply-slot" data-slot="' + esc(c.id) + '"></div></div>';
          }
        });
        html += "</div>";
        if (d.comments.length > visibleTop) {
          html += '<button class="thread-more" type="button" data-more-comments>' + ic("chevron", "ic-sm") +
            "Show " + (d.comments.length - visibleTop) + " more comments</button>";
        }
        thread.innerHTML = html;
        Array.prototype.slice.call(thread.querySelectorAll(".comment-body")).forEach(enhanceVideos);

        /* composer on top */
        var composerHost = document.getElementById("thread-composer");
        if (!user) {
          composerHost.innerHTML =
            '<div class="thread-signin">' + signInButtonHtml() +
            '<span class="signin-note">Sign in to join the discussion — your GitHub account is your identity.</span></div>';
        } else {
          renderCollapsedComposer(composerHost, user);
        }

        function renderCollapsedComposer(hostEl, u) {
          hostEl.innerHTML =
            '<div class="composer-collapsed" role="button" tabindex="0">' +
            avatarImg({ avatarUrl: u.avatar }, 22) + "<span>Write a comment…</span></div>";
          var open = function () { renderOpenComposer(hostEl, u); };
          var c = hostEl.querySelector(".composer-collapsed");
          c.addEventListener("click", open);
          c.addEventListener("keydown", function (e) {
            if (e.key === "Enter" || e.key === " ") { e.preventDefault(); open(); }
          });
        }

        function renderOpenComposer(hostEl, u) {
          hostEl.innerHTML = "";
          var ed = createEditor({ compact: true, placeholder: "Write a comment — share what worked, ask a follow-up…" });
          hostEl.appendChild(ed.root);
          hostEl.appendChild(ed.note);
          var actions = document.createElement("div");
          actions.className = "composer-actions";
          actions.innerHTML =
            '<button class="btn btn-ghost" type="button" data-c-cancel>Cancel</button>' +
            '<button class="btn btn-primary" type="button" data-c-send>Comment</button>';
          hostEl.appendChild(actions);
          ed.focus();
          actions.querySelector("[data-c-cancel]").addEventListener("click", function () {
            renderCollapsedComposer(hostEl, u);
          });
          actions.querySelector("[data-c-send]").addEventListener("click", function () {
            if (ed.isEmpty()) { toast("Write something first.", true); return; }
            var btn = this;
            btn.disabled = true; btn.textContent = "Posting…";
            api("/community/discussions/" + n + "/comments", { method: "POST", json: { body: ed.getMarkdown() } })
              .then(function (r2) {
                if (r2.ok) { toast("Comment posted."); render(); }
                else {
                  btn.disabled = false; btn.textContent = "Comment";
                  toast("Could not post: " + apiError(r2, "try again"), true);
                }
              });
          });
        }

      });
    }

    /* thread + post interactions: bound ONCE by delegation — render() replaces innerHTML only,
       so binding inside render would stack duplicate handlers on every refresh */
    if (thread) {
      thread.addEventListener("click", function (e) {
          var t;

          if ((t = e.target.closest("[data-more-comments]"))) {
            visibleTop += 10;
            render();
            return;
          }
          if ((t = e.target.closest("[data-more-replies]"))) {
            expandedReplies[t.getAttribute("data-more-replies")] = true;
            render();
            return;
          }

          var article = e.target.closest(".comment");

          if ((t = e.target.closest("[data-reply]")) && article) {
            var cid = article.getAttribute("data-cid");
            var slot = thread.querySelector('[data-slot="' + cid + '"]');
            if (!slot || slot.firstChild) return;
            slot.closest(".replies").style.display = "";
            var red = createEditor({ compact: true, placeholder: "Write a reply…" });
            slot.appendChild(red.root);
            slot.appendChild(red.note);
            var ra = document.createElement("div");
            ra.className = "composer-actions";
            ra.innerHTML =
              '<button class="btn btn-ghost" type="button">Cancel</button>' +
              '<button class="btn btn-primary" type="button">Reply</button>';
            slot.appendChild(ra);
            red.focus();
            ra.children[0].addEventListener("click", function () { slot.innerHTML = ""; });
            ra.children[1].addEventListener("click", function () {
              if (red.isEmpty()) { toast("Write something first.", true); return; }
              this.disabled = true; this.textContent = "Posting…";
              api("/community/discussions/" + n + "/comments", { method: "POST", json: { body: red.getMarkdown(), replyToId: cid } })
                .then(function (r2) {
                  if (r2.ok) { expandedReplies[cid] = true; toast("Reply posted."); render(); }
                  else { toast("Could not post: " + apiError(r2, "try again"), true); }
                });
            });
            return;
          }

          if ((t = e.target.closest("[data-edit]")) && article) {
            if (article.classList.contains("comment-editing")) return;
            article.classList.add("comment-editing");
            var body = article.querySelector(".comment-body");
            var eed = createEditor({ compact: true, initialHTML: body.innerHTML });
            var ea = document.createElement("div");
            ea.className = "composer-actions";
            ea.innerHTML =
              '<button class="btn btn-ghost" type="button">Cancel</button>' +
              '<button class="btn btn-primary" type="button">Save</button>';
            body.parentNode.insertBefore(eed.root, body.nextSibling);
            eed.root.parentNode.insertBefore(ea, eed.root.nextSibling);
            eed.focus();
            ea.children[0].addEventListener("click", function () {
              eed.root.remove(); ea.remove();
              article.classList.remove("comment-editing");
            });
            var cid2 = article.getAttribute("data-cid");
            ea.children[1].addEventListener("click", function () {
              if (eed.isEmpty()) { toast("Write something first.", true); return; }
              this.disabled = true; this.textContent = "Saving…";
              api("/community/comments/" + encodeURIComponent(cid2), { method: "PATCH", json: { body: eed.getMarkdown() } })
                .then(function (r2) {
                  if (r2.ok) { toast("Comment updated."); render(); }
                  else { toast("Edit failed: " + apiError(r2, "try again"), true); }
                });
            });
            return;
          }

          if ((t = e.target.closest("[data-del]")) && article) {
            var cid3 = article.getAttribute("data-cid");
            confirmModal({
              title: "Delete this comment?",
              message: "It is removed for everyone. This can't be undone.",
              confirmLabel: "Delete comment",
              danger: true,
            }).then(function (yes) {
              if (!yes) return;
              api("/community/comments/" + encodeURIComponent(cid3), { method: "DELETE" }).then(function (r2) {
                if (r2.ok) { toast("Comment deleted."); render(); }
                else toast("Delete failed: " + apiError(r2, "try again"), true);
              });
            });
            return;
          }

          if ((t = e.target.closest("[data-react]"))) {
            if (!sessionId()) { location.href = WORKER + "/auth/login"; return; }
            t.disabled = true;
            api("/community/reactions", { method: "POST", json: { subjectId: t.getAttribute("data-react"), content: "THUMBS_UP" } })
              .then(function (r2) {
                if (r2.ok) render();
                else { t.disabled = false; toast("Could not react: " + apiError(r2, "try again"), true); }
              });
          }
        });

        host.addEventListener("click", function (e) {
          var t = e.target.closest("[data-react]");
          if (!t) return;
          if (!sessionId()) { location.href = WORKER + "/auth/login"; return; }
          t.disabled = true;
          api("/community/reactions", { method: "POST", json: { subjectId: t.getAttribute("data-react"), content: "THUMBS_UP" } })
            .then(function (r2) {
              if (r2.ok) render();
              else { t.disabled = false; toast("Could not react: " + apiError(r2, "try again"), true); }
            });
        });
    }

    render();
  }

  /* ---------- 3. composer page ---------- */

  function initCompose() {
    var host = document.getElementById("community-compose");
    if (!host) return;
    var params = new URLSearchParams(location.search);
    var editN = parseInt(params.get("edit") || "", 10) || null;

    me().then(function (user) {
      if (!user) {
        host.innerHTML =
          '<div class="thread-signin">' + signInButtonHtml() +
          ' <span class="signin-note">to write a post. Your GitHub account is the author — no separate registration.</span></div>';
        return;
      }

      var loads = [api("/community/categories")];
      if (editN) loads.push(api("/community/discussions/" + editN));
      Promise.all(loads).then(function (results) {
        var r = results[0];
        if (!r.ok) { failNote(host, "the composer"); return; }
        var editing = null;
        if (editN) {
          var pr = results[1];
          if (!pr.ok) { failNote(host, "the post to edit"); return; }
          if (pr.data.author.login !== user.login) {
            host.innerHTML = '<div class="load-note">Only the author can edit this post.</div>';
            return;
          }
          editing = pr.data;
          var titleEl = document.querySelector(".section-title");
          if (titleEl) titleEl.textContent = "Edit post";
          document.title = "Edit post — Snapmap+ Community";
        }

        var cats = r.data.categories;
        var options = cats.map(function (c) {
          var sel = editing && editing.category && editing.category.slug === c.slug ? " selected" : "";
          return '<option value="' + esc(c.id) + '"' + sel + ">" + esc(c.name) + "</option>";
        }).join("");

        host.innerHTML =
          '<form class="compose-form">' +
            '<div class="field"><label for="c-title">Title</label>' +
            '<input id="c-title" type="text" maxlength="200" placeholder="e.g. How to wire a door to a switch" required></div>' +
            '<div class="field"><label for="c-cat">Category</label>' +
            '<div class="select-wrap"><select id="c-cat">' + options + "</select>" + ic("chevron", "ic-sm") + "</div></div>" +
            '<div class="field"><label>Post</label><div id="c-editor"></div></div>' +
            '<div class="compose-footer">' +
              '<span class="compose-status" id="c-status" aria-live="polite"></span>' +
              '<span class="spacer"></span>' +
              '<a class="btn btn-ghost" href="' + (editing ? "community-post.html?n=" + editN : "community.html") + '">Cancel</a>' +
              '<button class="btn btn-primary" type="submit">' + (editing ? "Save changes" : "Publish") + "</button>" +
            "</div>" +
          "</form>";

        var form = host.querySelector("form");
        var titleInput = host.querySelector("#c-title");
        var status = host.querySelector("#c-status");
        var ed = createEditor({
          placeholder: "Write your guide or tip. Format with the toolbar, drop screenshots straight in, and paste a YouTube link on its own line to embed the video.",
          initialHTML: editing ? editing.bodyHTML : "",
        });
        host.querySelector("#c-editor").appendChild(ed.root);
        host.querySelector("#c-editor").appendChild(ed.note);

        if (editing) {
          titleInput.value = editing.title;
        } else {
          try {
            var draft = JSON.parse(localStorage.getItem(DRAFT_KEY) || "null");
            if (draft) {
              titleInput.value = draft.title || "";
              if (draft.html) ed.setHTML(draft.html);
            }
          } catch (e) {}
          var save = debounce(function () {
            try {
              localStorage.setItem(DRAFT_KEY, JSON.stringify({ title: titleInput.value, html: ed.getHTML() }));
              status.textContent = "Draft saved";
            } catch (e) {}
          }, 800);
          titleInput.addEventListener("input", save);
          ed.area.addEventListener("input", save);
        }

        form.addEventListener("submit", function (e) {
          e.preventDefault();
          var title = titleInput.value.trim();
          if (title.length < 3) { toast("Give the post a title.", true); titleInput.focus(); return; }
          if (ed.isEmpty()) { toast("Write the post before publishing.", true); ed.focus(); return; }
          var md = ed.getMarkdown();
          if (md.length < 10) { toast("The post is a little too short.", true); ed.focus(); return; }
          var btn = form.querySelector('button[type="submit"]');
          var idle = editing ? "Save changes" : "Publish";
          btn.disabled = true;
          btn.textContent = editing ? "Saving…" : "Publishing…";
          var payload = { title: title, categoryId: host.querySelector("#c-cat").value, body: md };
          var call = editing
            ? api("/community/discussions/" + editN, { method: "PATCH", json: payload })
            : api("/community/discussions", { method: "POST", json: payload });
          call.then(function (r2) {
            if (r2.ok && (editing || r2.data.number)) {
              if (!editing) { try { localStorage.removeItem(DRAFT_KEY); } catch (err) {} }
              location.href = "community-post.html?n=" + (editing ? editN : r2.data.number);
            } else {
              btn.disabled = false;
              btn.textContent = idle;
              toast((editing ? "Save" : "Publish") + " failed: " + apiError(r2, "try again"), true);
            }
          });
        });
      });
    });
  }

  /* ---------- boot ---------- */

  function boot() {
    captureSession();
    initAuthWidget();
    initIndex();
    initPost();
    initCompose();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
