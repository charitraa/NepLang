// Shared site code: theme toggle, NepLang syntax highlighting, the code editor, the
// WebAssembly runner, copy buttons. No dependencies.
(function () {
  'use strict';

  /* ---------- theme ---------- */

  const THEME_KEY = 'neplang-theme';
  function applyTheme(t) {
    if (t) document.documentElement.setAttribute('data-theme', t);
    else document.documentElement.removeAttribute('data-theme');
  }
  try { applyTheme(localStorage.getItem(THEME_KEY)); } catch (e) { /* storage blocked */ }

  function currentTheme() {
    const set = document.documentElement.getAttribute('data-theme');
    if (set) return set;
    return matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  }

  function setupThemeToggle() {
    const btn = document.querySelector('.theme-toggle');
    if (!btn) return;
    const label = () => {
      const dark = currentTheme() === 'dark';
      btn.setAttribute('aria-label', dark ? 'Switch to light theme' : 'Switch to dark theme');
      btn.innerHTML = dark
        ? '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><circle cx="12" cy="12" r="4.5"/><path d="M12 2v2.5M12 19.5V22M2 12h2.5M19.5 12H22M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"/></svg>'
        : '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/></svg>';
    };
    label();
    btn.addEventListener('click', () => {
      const next = currentTheme() === 'dark' ? 'light' : 'dark';
      applyTheme(next);
      try { localStorage.setItem(THEME_KEY, next); } catch (e) { /* ignore */ }
      label();
    });
  }

  /* ---------- highlighting ---------- */

  const KEYWORDS = new Set(['vana', 'yedi', 'natara', 'jabasamma', 'ko_lagi', 'in', 'kaam', 'farkau',
    'class', 'yo', 'ra', 'wa', 'hoina', 'ruk', 'agadi', 'import', 'interface', 'super', 'const',
    'khali', 'sacho', 'jutho']);
  const TYPES = new Set(['int', 'float', 'bool', 'string', 'array', 'object', 'any', 'void']);

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function highlightString(s) {
    // "${...}" pieces are code; show the braces and inner code distinctly.
    let html = '';
    let i = 0;
    while (i < s.length) {
      const open = s.indexOf('${', i);
      if (open < 0 || (open > 0 && s[open - 1] === '\\')) {
        if (open < 0) { html += '<span class="tok-str">' + esc(s.slice(i)) + '</span>'; break; }
        html += '<span class="tok-str">' + esc(s.slice(i, open + 2)) + '</span>';
        i = open + 2;
        continue;
      }
      const close = s.indexOf('}', open);
      html += '<span class="tok-str">' + esc(s.slice(i, open)) + '</span>';
      if (close < 0) { html += '<span class="tok-interp">' + esc(s.slice(open)) + '</span>'; break; }
      html += '<span class="tok-interp">${</span>' + highlight(s.slice(open + 2, close)) + '<span class="tok-interp">}</span>';
      i = close + 1;
    }
    return html;
  }

  const TOKEN = /(#[^\n]*)|("(?:[^"\\\n]|\\.)*"?)|(\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)|([A-Za-z_][A-Za-z0-9_]*)(\s*\()?|([\s\S])/g;

  function highlight(code) {
    // a fresh regex per call: highlightString() calls this recursively for "${...}"
    const re = new RegExp(TOKEN.source, 'g');
    let html = '';
    let m;
    while ((m = re.exec(code))) {
      if (m[1]) html += '<span class="tok-com">' + esc(m[1]) + '</span>';
      else if (m[2]) html += highlightString(m[2]);
      else if (m[3]) html += '<span class="tok-num">' + m[3] + '</span>';
      else if (m[4]) {
        const w = m[4];
        let cls = KEYWORDS.has(w) ? 'tok-kw' : TYPES.has(w) ? 'tok-type' : m[5] ? 'tok-fn' : '';
        html += cls ? '<span class="' + cls + '">' + w + '</span>' : w;
        if (m[5]) html += esc(m[5]);
      } else html += esc(m[6]);
    }
    return html;
  }

  /* ---------- editor ---------- */

  function createEditor(host, code, onRun) {
    host.classList.add('editor');
    host.innerHTML = '<div class="gutter" aria-hidden="true"></div><div class="area">' +
      '<pre class="hl" aria-hidden="true"></pre>' +
      '<textarea spellcheck="false" autocapitalize="off" autocomplete="off" aria-label="NepLang code"></textarea></div>';
    const gutter = host.querySelector('.gutter');
    const pre = host.querySelector('pre');
    const ta = host.querySelector('textarea');
    ta.value = code;

    function render() {
      // trailing newline keeps the highlight layer as tall as the textarea
      pre.innerHTML = highlight(ta.value) + '\n';
      const lines = ta.value.split('\n').length;
      let g = '';
      for (let i = 1; i <= lines; i++) g += i + '\n';
      gutter.textContent = g;
      sync();
    }
    function sync() {
      pre.scrollTop = ta.scrollTop;
      pre.scrollLeft = ta.scrollLeft;
      gutter.scrollTop = ta.scrollTop;
    }
    ta.addEventListener('scroll', sync);
    ta.addEventListener('input', render);

    ta.addEventListener('keydown', (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
        e.preventDefault();
        if (onRun) onRun();
        return;
      }
      const start = ta.selectionStart, end = ta.selectionEnd, v = ta.value;
      if (e.key === 'Tab' && !e.shiftKey && start === end) {
        e.preventDefault();
        ta.setRangeText('    ', start, end, 'end');
        render();
      } else if (e.key === 'Enter' && start === end) {
        // keep the indentation, and indent one more level after '{'
        const lineStart = v.lastIndexOf('\n', start - 1) + 1;
        const indent = /^[ \t]*/.exec(v.slice(lineStart, start))[0];
        const extra = v[start - 1] === '{' ? '    ' : '';
        e.preventDefault();
        ta.setRangeText('\n' + indent + extra, start, end, 'end');
        render();
      } else if (e.key === '}' && start === end) {
        // dedent a closing brace typed on an otherwise blank line
        const lineStart = v.lastIndexOf('\n', start - 1) + 1;
        const before = v.slice(lineStart, start);
        if (/^ {4,}$/.test(before)) {
          e.preventDefault();
          ta.setRangeText(before.slice(4) + '}', lineStart, start, 'end');
          render();
        }
      }
    });

    render();
    return {
      get value() { return ta.value; },
      set value(v) { ta.value = v; ta.scrollTop = 0; render(); },
      focus() { ta.focus(); },
      textarea: ta,
    };
  }

  /* ---------- runner ---------- */

  const workerUrl = (document.currentScript && document.currentScript.src)
    ? new URL('worker.js', document.currentScript.src).href : 'assets/worker.js';

  // run({code, mode, input}, onLines(lines), timeoutMs) -> Promise<{status, timedOut, truncated}>
  function run(opts, onLines, timeoutMs) {
    return new Promise((resolve) => {
      let worker;
      try { worker = new Worker(workerUrl); } catch (e) {
        onLines([['err', 'This browser could not start the NepLang runner: ' + e.message]]);
        resolve({ status: 1 });
        return;
      }
      let total = 0;
      let finished = false;
      const LIMIT = 5000;
      const finish = (r) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        worker.terminate();
        resolve(r);
      };
      const timer = setTimeout(() => finish({ status: 1, timedOut: true }), timeoutMs || 5000);
      worker.onmessage = (e) => {
        const d = e.data;
        if (d.type === 'out') {
          const room = LIMIT - total;
          if (room > 0) onLines(d.lines.slice(0, room));
          total += d.lines.length;
          if (total > LIMIT) finish({ status: 1, truncated: true });
        } else if (d.type === 'done') finish({ status: d.status });
      };
      worker.onerror = (e) => {
        onLines([['err', 'Could not load NepLang (' + (e.message || 'network error') + ').']]);
        finish({ status: 1 });
      };
      worker.postMessage(opts);
    });
  }

  // Appends program output to a <pre> element.
  function outputWriter(pre) {
    return (lines) => {
      const frag = document.createDocumentFragment();
      for (const [stream, text] of lines) {
        if (stream === 'err') {
          const span = document.createElement('span');
          span.className = 'err';
          span.textContent = text + '\n';
          frag.appendChild(span);
        } else frag.appendChild(document.createTextNode(text + '\n'));
      }
      pre.appendChild(frag);
    };
  }

  function note(pre, text) {
    const span = document.createElement('span');
    span.className = 'note';
    span.textContent = text + '\n';
    pre.appendChild(span);
  }

  function describeEnd(pre, r, mode) {
    if (r.timedOut) note(pre, 'Stopped after 5 seconds. Is there a loop that never ends?');
    else if (r.truncated) note(pre, 'Stopped after 5000 lines of output.');
    else if (r.status === 0 && !pre.textContent) note(pre, mode === 'check' ? 'No errors found.' : 'The program finished without printing anything.');
  }

  /* ---------- share links ---------- */

  function encodeCode(code) {
    const bytes = new TextEncoder().encode(code);
    let bin = '';
    bytes.forEach((b) => { bin += String.fromCharCode(b); });
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  function decodeCode(s) {
    try {
      const bin = atob(s.replace(/-/g, '+').replace(/_/g, '/'));
      return new TextDecoder().decode(Uint8Array.from(bin, (c) => c.charCodeAt(0)));
    } catch (e) { return null; }
  }

  /* ---------- copy buttons ---------- */

  function setupCopyButtons(root) {
    (root || document).querySelectorAll('.cmd').forEach((box) => {
      if (box.querySelector('button')) return;
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.textContent = 'Copy';
      btn.addEventListener('click', async () => {
        const text = box.querySelector('pre').textContent;
        try { await navigator.clipboard.writeText(text); btn.textContent = 'Copied'; } catch (e) { btn.textContent = 'Select and copy'; }
        setTimeout(() => { btn.textContent = 'Copy'; }, 1600);
      });
      box.appendChild(btn);
    });
  }

  /* ---------- platform ---------- */

  function detectOS() {
    const p = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || navigator.userAgent || '').toLowerCase();
    if (p.includes('win')) return 'windows';
    if (p.includes('mac') || p.includes('iphone') || p.includes('ipad')) return 'macos';
    if (p.includes('linux') || p.includes('x11') || p.includes('android')) return 'linux';
    return 'linux';
  }

  document.addEventListener('DOMContentLoaded', () => {
    setupThemeToggle();
    setupCopyButtons();
  });

  window.NepLang = { highlight, createEditor, run, outputWriter, note, describeEnd, encodeCode, decodeCode, setupCopyButtons, detectOS };
})();
