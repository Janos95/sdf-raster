let Module;
let api;
let initializing;
let heap;
let threaded = false;

function moduleUrl(path) {
  return new URL(`../build-wasm-web/${path}`, self.location.href).href;
}

function wrapApi() {
  return {
    init: Module.cwrap("prospero_init", "number", []),
    defaultText: Module.cwrap("prospero_default_text", "number", []),
    text: Module.cwrap("prospero_text", "number", []),
    setText: Module.cwrap("prospero_set_text", "number", ["string"]),
    render: Module.cwrap("prospero_render", "number", []),
    imageData: Module.cwrap("prospero_image_data", "number", []),
    width: Module.cwrap("prospero_image_width", "number", []),
    height: Module.cwrap("prospero_image_height", "number", []),
    instructionCount: Module.cwrap("prospero_instruction_count", "number", []),
    textLen: Module.cwrap("prospero_text_len", "number", []),
    maxThreads: Module.cwrap("prospero_max_threads", "number", [])
  };
}

function moduleConfig() {
  const script = threaded ? "prospero_web.js" : "prospero_web_fallback.js";
  return {
    script,
    factory: threaded ? "createProsperoModule" : "createProsperoFallbackModule"
  };
}

function makeFrame(seq = 0) {
  const renderUs = api.render();
  if (renderUs < 0) throw new Error("render failed");
  const width = api.width();
  const height = api.height();
  const start = api.imageData();
  const memory = Module.HEAPU8 || heap;
  const pixels = memory.slice(start, start + width * height);
  return {
    type: "frame",
    seq,
    width,
    height,
    renderUs,
    pixels: pixels.buffer,
    text: Module.UTF8ToString(api.text()),
    threads: api.maxThreads(),
    instructions: api.instructionCount(),
    textLen: api.textLen()
  };
}

async function initModule() {
  if (Module) return;
  if (initializing) return initializing;
  initializing = (async () => {
    self.postMessage({ type: "status", label: "Load" });
    const config = moduleConfig();
    importScripts(moduleUrl(config.script));
    const wasmMemory = new WebAssembly.Memory(
      threaded
        ? { initial: 4096, maximum: 4096, shared: true }
        : { initial: 4096, maximum: 4096 }
    );
    Module = await self[config.factory]({
      wasmMemory,
      mainScriptUrlOrBlob: moduleUrl(config.script),
      locateFile(path) {
        return moduleUrl(path);
      },
      printErr(message) {
        console.error(message);
      }
    });
    heap = new Uint8Array(wasmMemory.buffer);
    self.postMessage({ type: "status", label: "Init" });
    api = wrapApi();
    if (api.init() !== 0) throw new Error("initialization failed");
  })();
  return initializing;
}

self.onmessage = event => {
  const message = event.data;
  (async () => {
    if (message.type === "init") {
      threaded = !!message.threaded;
      await initModule();
      self.postMessage({ type: "status", label: "Render" });
      const frame = makeFrame(0);
      self.postMessage(
        { type: "ready", defaultText: Module.UTF8ToString(api.defaultText()), frame },
        [frame.pixels]
      );
      return;
    }
    if (message.type === "render") {
      await initModule();
      if (api.setText(message.text) !== 0) throw new Error("text build failed");
      const frame = makeFrame(message.seq);
      self.postMessage(frame, [frame.pixels]);
    }
  })().catch(error => {
    self.postMessage({ type: "error", message: error.stack || error.message || String(error) });
  });
};
