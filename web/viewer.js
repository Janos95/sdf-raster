(() => {
  const canvas = document.getElementById("viewport");
  const ctx = canvas.getContext("2d", { alpha: false });
  const textarea = document.getElementById("text-input");
  const renderButton = document.getElementById("render-button");
  const resetButton = document.getElementById("reset-button");
  const state = document.getElementById("state");
  const renderMs = document.getElementById("render-ms");
  const threads = document.getElementById("threads");
  const instructions = document.getElementById("instructions");
  const textLen = document.getElementById("text-len");

  let worker;
  let imageData;
  let defaultText = "";
  let renderTimer = 0;
  let nextSeq = 1;
  let latestSeq = 0;
  let ready = false;
  let rendering = false;

  function setState(label, kind = "") {
    state.textContent = label;
    state.className = `state ${kind}`.trim();
  }

  function setControlsEnabled(enabled) {
    textarea.disabled = !enabled;
    renderButton.disabled = !enabled || rendering;
    resetButton.disabled = !enabled;
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

  function requestRender(immediate = false) {
    if (!ready) return;
    window.clearTimeout(renderTimer);
    const delay = immediate ? 0 : 120;
    renderTimer = window.setTimeout(() => {
      rendering = true;
      setControlsEnabled(true);
      setState("Render", "busy");
      latestSeq = nextSeq++;
      worker.postMessage({ type: "render", seq: latestSeq, text: textarea.value });
    }, delay);
  }

  function handleFrame(frame) {
    if (frame.seq && frame.seq < latestSeq) return;
    if (typeof frame.text === "string" && textarea.value !== frame.text) {
      const cursor = textarea.selectionStart;
      textarea.value = frame.text;
      const nextCursor = Math.min(cursor, frame.text.length);
      textarea.setSelectionRange(nextCursor, nextCursor);
    }
    paintGray(frame.width, frame.height, frame.pixels);
    updateStats(frame);
    rendering = false;
    setControlsEnabled(true);
    setState("Ready");
  }

  function boot() {
    setControlsEnabled(false);
    worker = new Worker("prospero_worker.js");
    worker.onmessage = event => {
      const message = event.data;
      if (message.type === "ready") {
        ready = true;
        defaultText = message.defaultText;
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
    const params = new URLSearchParams(window.location.search);
    const forceFallback = params.has("fallback") || params.get("threaded") === "0";
    worker.postMessage({ type: "init", threaded: window.crossOriginIsolated && !forceFallback });
    textarea.addEventListener("input", () => requestRender(false));
    renderButton.addEventListener("click", () => requestRender(true));
    resetButton.addEventListener("click", () => {
      textarea.value = defaultText;
      requestRender(true);
      textarea.focus();
    });
    canvas.addEventListener("click", () => textarea.focus());
  }

  boot();
})();
