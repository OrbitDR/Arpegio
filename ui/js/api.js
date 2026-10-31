// The bridge to the application: requests that return a promise, and events pushed from the native side.
const bridge = window.chrome.webview;
const pending = new Map();
const listeners = new Map();
let nextId = 1;

bridge.addEventListener('message', (message) => {
  const { id, event, data, result, error } = message.data;
  if (event) {
    for (const listener of listeners.get(event) ?? []) listener(data);
    return;
  }
  const request = pending.get(id);
  if (!request) return;
  pending.delete(id);
  if (error) request.reject(new Error(error));
  else request.resolve(result);
});

export function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    bridge.postMessage({ id, method, params });
  });
}

export function listen(event, listener) {
  if (!listeners.has(event)) listeners.set(event, new Set());
  listeners.get(event).add(listener);
}
