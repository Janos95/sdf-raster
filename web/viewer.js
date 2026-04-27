(() => {
  const canvas = document.getElementById("viewport");
  const ctx = canvas.getContext("2d", { alpha: false });
  const textarea = document.getElementById("text-input");
  const resetButton = document.getElementById("reset-button");
  const paramsControls = document.getElementById("params-controls");
  const state = document.getElementById("state");
  const renderMs = document.getElementById("render-ms");
  const threads = document.getElementById("threads");
  const instructions = document.getElementById("instructions");
  const textLen = document.getElementById("text-len");

  const PARAM_SPECS = [
    { key: "scale", label: "Scale", min: 0.1, max: 2, step: 0.01, precision: 2 },
    { key: "marginX", label: "Margin X", min: 0, max: 1024, step: 1, precision: 0 },
    { key: "marginY", label: "Margin Y", min: 0, max: 1024, step: 1, precision: 0 },
    { key: "maxWidth", label: "Max Width", min: 1, max: 1024, step: 1, precision: 0 },
    { key: "tracking", label: "Tracking", min: -10, max: 40, step: 0.5, precision: 1 },
    { key: "spaceWidth", label: "Space", min: 0, max: 100, step: 1, precision: 0 },
    { key: "lineGap", label: "Line Gap", min: -20, max: 80, step: 1, precision: 0 }
  ];
  const PARAM_BY_KEY = new Map(PARAM_SPECS.map(spec => [spec.key, spec]));
  const TEXT_RENDER_DELAY = 70;
  const PARAM_RENDER_DELAY = 0;

  let worker;
  let imageData;
  let defaultText = "";
  let renderTimer = 0;
  let nextSeq = 1;
  let latestSeq = 0;
  let ready = false;
  let rendering = false;
  let renderPending = false;
  let paramsState = {};
  let defaultParams = {};
  const paramElements = new Map();

  function setState(label, kind = "") {
    state.textContent = label;
    state.className = `state ${kind}`.trim();
  }

  function setControlsEnabled(enabled) {
    textarea.disabled = !enabled;
    resetButton.disabled = !enabled;
    for (const elements of paramElements.values()) {
      elements.range.disabled = !enabled;
    }
  }

  function clampParam(spec, value, fallback) {
    const parsed = Number(value);
    if (!Number.isFinite(parsed)) return fallback;
    return Math.min(spec.max, Math.max(spec.min, parsed));
  }

  function formatParam(spec, value) {
    const fixed = Number(value).toFixed(spec.precision);
    return spec.precision === 0 ? String(Math.round(Number(fixed))) : fixed;
  }

  function currentParams() {
    const params = {};
    for (const spec of PARAM_SPECS) {
      const fallback = defaultParams[spec.key] ?? spec.min;
      params[spec.key] = clampParam(spec, paramsState[spec.key], fallback);
    }
    return params;
  }

  function syncParamControl(key) {
    const elements = paramElements.get(key);
    if (!elements) return;
    const { spec, range, label } = elements;
    const value = paramsState[key];
    range.value = String(value);
    label.textContent = `${spec.label}: ${formatParam(spec, value)}`;
  }

  function syncAllParamControls() {
    for (const spec of PARAM_SPECS) syncParamControl(spec.key);
  }

  function setParam(key, value) {
    const spec = PARAM_BY_KEY.get(key);
    if (!spec) return;
    const fallback = paramsState[key] ?? defaultParams[key] ?? spec.min;
    paramsState[key] = clampParam(spec, value, fallback);
    syncParamControl(key);
    requestRender(PARAM_RENDER_DELAY);
  }

  function setParams(params) {
    const next = {};
    for (const spec of PARAM_SPECS) {
      const fallback = defaultParams[spec.key] ?? spec.min;
      next[spec.key] = clampParam(spec, params?.[spec.key], fallback);
    }
    paramsState = next;
    syncAllParamControls();
  }

  function buildParamControls() {
    paramsControls.textContent = "";
    paramElements.clear();
    for (const spec of PARAM_SPECS) {
      const row = document.createElement("label");
      row.className = "param-row";

      const name = document.createElement("span");
      name.className = "param-label";
      name.textContent = spec.label;

      const range = document.createElement("input");
      range.type = "range";
      range.min = String(spec.min);
      range.max = String(spec.max);
      range.step = String(spec.step);

      row.append(name, range);
      paramsControls.append(row);
      paramElements.set(spec.key, { spec, range, label: name });

      range.addEventListener("input", () => setParam(spec.key, range.value));
    }
    syncAllParamControls();
    setControlsEnabled(ready);
  }

  function paintGray(width, height, buffer) {
    if (!imageData || canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
      imageData = ctx.createImageData(width, height);
    }
    const src = new Uint8Array(buffer);
    const dst = imageData.data;
    for (let i = 0, j = 0; i < src.length; i++, j += 4) {
      const v = src[i];
      dst[j] = v;
      dst[j + 1] = v;
      dst[j + 2] = v;
      dst[j + 3] = 255;
    }
    ctx.putImageData(imageData, 0, 0);
  }

  function updateStats(frame) {
    renderMs.textContent = `${(frame.renderUs / 1000).toFixed(3)} ms`;
    threads.textContent = String(frame.threads);
    instructions.textContent = String(frame.instructions);
    textLen.textContent = String(frame.textLen);
  }

  function requestRender(delay = TEXT_RENDER_DELAY) {
    if (!ready) return;
    renderPending = true;
    window.clearTimeout(renderTimer);
    renderTimer = window.setTimeout(sendRender, delay);
  }

  function sendRender() {
    renderTimer = 0;
    if (!ready || !renderPending) return;
    if (rendering) return;
    renderPending = false;
    rendering = true;
    setControlsEnabled(true);
    setState("Render", "busy");
    latestSeq = nextSeq++;
    worker.postMessage({
      type: "render",
      seq: latestSeq,
      text: textarea.value,
      params: currentParams()
    });
  }

  function handleFrame(frame) {
    if (frame.seq && frame.seq < latestSeq) return;
    const hasNewerInput = renderPending;
    if (!hasNewerInput && typeof frame.text === "string" && textarea.value !== frame.text) {
      const cursor = textarea.selectionStart;
      textarea.value = frame.text;
      const nextCursor = Math.min(cursor, frame.text.length);
      textarea.setSelectionRange(nextCursor, nextCursor);
    }
    if (!hasNewerInput && frame.params) setParams(frame.params);
    paintGray(frame.width, frame.height, frame.pixels);
    updateStats(frame);
    rendering = false;
    setControlsEnabled(true);
    if (renderPending) {
      setState("Render", "busy");
      window.clearTimeout(renderTimer);
      renderTimer = window.setTimeout(sendRender, 0);
    } else {
      setState("Ready");
    }
  }

  function boot() {
    setControlsEnabled(false);
    worker = new Worker("prospero_worker.js");
    worker.onmessage = event => {
      const message = event.data;
      if (message.type === "ready") {
        ready = true;
        defaultText = message.defaultText;
        defaultParams = message.defaultParams || {};
        setParams(message.params || defaultParams);
        buildParamControls();
        textarea.value = defaultText;
        handleFrame(message.frame);
        return;
      }
      if (message.type === "frame") {
        handleFrame(message);
        return;
      }
      if (message.type === "status") {
        setState(message.label, message.kind || "busy");
        return;
      }
      if (message.type === "error") {
        console.error(message.message);
        window.__prosperoError = message.message;
        rendering = false;
        setControlsEnabled(ready);
        setState("Error", "error");
      }
    };
    worker.onerror = error => {
      console.error(error);
      window.__prosperoError = error.message || String(error);
      rendering = false;
      setControlsEnabled(false);
      setState("Error", "error");
    };
    const queryParams = new URLSearchParams(window.location.search);
    const forceFallback = queryParams.has("fallback") || queryParams.get("threaded") === "0";
    worker.postMessage({ type: "init", threaded: window.crossOriginIsolated && !forceFallback });
    textarea.addEventListener("input", () => requestRender(TEXT_RENDER_DELAY));
    resetButton.addEventListener("click", () => {
      textarea.value = defaultText;
      setParams(defaultParams);
      requestRender(0);
      textarea.focus();
    });
    canvas.addEventListener("click", () => textarea.focus());
  }

  boot();
})();
