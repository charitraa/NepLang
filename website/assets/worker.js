// Runs one NepLang program in a fresh WebAssembly instance. The page terminates this
// worker on timeout (endless loops), so the page itself never freezes.
importScripts('../wasm/neplang.js');

const MODES = {
  run: (f) => [f],
  vm: (f) => ['--vm', f],
  check: (f) => ['check', f],
  c: (f) => ['--emit-c', f],
  ir: (f) => ['--ir', f],
};

onmessage = async (event) => {
  const { code, mode, input } = event.data;
  const bytes = new TextEncoder().encode(input || '');
  let pos = 0;

  // Output is batched: one message per line would flood the page on a printing loop.
  let batch = [];
  const flush = () => {
    if (batch.length) postMessage({ type: 'out', lines: batch });
    batch = [];
  };
  const push = (stream, text) => {
    batch.push([stream, text]);
    if (batch.length >= 200) flush();
  };

  let status = 0;
  try {
    const mod = await createNepLang({
      locateFile: (p) => '../wasm/' + p,
      print: (t) => push('out', t),
      printErr: (t) => push('err', t),
      stdin: () => (pos < bytes.length ? bytes[pos++] : null),
    });
    mod.FS.writeFile('/main.np', code);
    mod.FS.chdir('/');
    status = mod.callMain((MODES[mode] || MODES.run)('main.np'));
  } catch (e) {
    if (e && typeof e.status === 'number') status = e.status;   // exit(1) after an error
    else { push('err', 'Internal error: ' + e); status = 1; }
  }
  flush();
  postMessage({ type: 'done', status });
};
