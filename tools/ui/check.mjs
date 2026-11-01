// Drives the real application through the Chrome DevTools Protocol: takes screenshots of each view, runs
// checks inside the page and reports console errors. Needs Node 22 or later for its WebSocket client.
//
//   node tools/ui/check.mjs [--exe build/release/arpegio.exe] [--data FOLDER] [--out FOLDER] [--port 9333]
//                           [--steps FILE.mjs] [--keep-open]
//
// A step file exports `default async function (page)`, using page.go, page.shot, page.eval and page.wait.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { isAbsolute, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

function option(name, fallback) {
  const at = process.argv.indexOf(`--${name}`);
  return at > 0 ? process.argv[at + 1] : fallback;
}

const port = Number(option('port', 9333));
const exe = resolve(option('exe', 'build/release/arpegio.exe'));
const data = resolve(option('data', join(tmpdir(), 'arpegio-ui-check')));
const out = resolve(option('out', join(tmpdir(), 'arpegio-ui-check-shots')));
const steps = option('steps');
const keepOpen = process.argv.includes('--keep-open');

mkdirSync(out, { recursive: true });
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

const app = spawn(exe, ['--data', data, '--devtools'], {
  env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` },
  stdio: 'ignore',
});

async function findTarget() {
  for (let attempt = 0; attempt < 120; ++attempt) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      const page = targets.find((target) => target.type === 'page' && target.url.includes('arpegio.example'));
      if (page) return page;
    } catch {
      // Not listening yet.
    }
    await sleep(250);
  }
  throw new Error('the application page never appeared');
}

class Page {
  constructor(socket) {
    this.socket = socket;
    this.nextId = 1;
    this.pending = new Map();
    this.problems = [];
    socket.addEventListener('message', (message) => this.receive(JSON.parse(message.data)));
  }

  receive(message) {
    if (message.id) {
      const request = this.pending.get(message.id);
      this.pending.delete(message.id);
      if (message.error) request.reject(new Error(message.error.message));
      else request.resolve(message.result);
      return;
    }
    if (message.method === 'Runtime.exceptionThrown') {
      const { exceptionDetails: details } = message.params;
      this.problems.push(`exception: ${details.exception?.description ?? details.text}`);
    } else if (message.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(message.params.type)) {
      this.problems.push(`console: ${message.params.args.map((arg) => arg.value ?? arg.description).join(' ')}`);
    } else if (message.method === 'Log.entryAdded' && message.params.entry.level === 'error') {
      this.problems.push(`log: ${message.params.entry.text} ${message.params.entry.url ?? ''}`);
    }
  }

  send(method, params = {}) {
    return new Promise((resolve_, reject) => {
      const id = this.nextId++;
      this.pending.set(id, { resolve: resolve_, reject });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  async eval(expression) {
    const { result, exceptionDetails } = await this.send('Runtime.evaluate', {
      expression,
      awaitPromise: true,
      returnByValue: true,
    });
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
    return result.value;
  }

  // Sends a request to the application, as the page itself would.
  call(method, params = {}) {
    return this.eval(`import('/js/api.js').then((api) => api.call(${JSON.stringify(method)}, ${JSON.stringify(params)}))`);
  }

  go(route) {
    return this.eval(`import('/js/router.js').then((router) => router.go(${JSON.stringify(route)}))`).then(() => sleep(500));
  }

  async wait(expression, timeoutMs = 15000) {
    const started = Date.now();
    while (Date.now() - started < timeoutMs) {
      if (await this.eval(expression)) return true;
      await sleep(100);
    }
    throw new Error(`timed out waiting for: ${expression}`);
  }

  async size(width, height) {
    await this.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
    await sleep(300);
  }

  async shot(name) {
    const { data: image } = await this.send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(join(out, `${name}.png`), Buffer.from(image, 'base64'));
    console.log(`shot ${name}`);
  }

  async click(selector) {
    await this.eval(`document.querySelector(${JSON.stringify(selector)}).click()`);
    await sleep(200);
  }
}

async function main() {
  const target = await findTarget();
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ready, failed) => {
    socket.addEventListener('open', ready);
    socket.addEventListener('error', failed);
  });
  const page = new Page(socket);
  await page.send('Page.enable');
  await page.send('Runtime.enable');
  await page.send('Log.enable');
  await page.wait(`document.querySelector('.page') !== null`);

  if (steps) {
    const path = isAbsolute(steps) ? steps : resolve(steps);
    const { default: run } = await import(pathToFileURL(path).href);
    await run(page, { sleep, out, data });
  } else {
    await page.size(1320, 860);
    await page.shot('start');
  }

  const problems = [...new Set(page.problems)];
  console.log(problems.length ? `PROBLEMS (${problems.length}):\n${problems.join('\n')}` : 'no console errors');
  return problems.length;
}

main()
  .catch((error) => {
    console.error(error.message);
    return 1;
  })
  .then(async (failures) => {
    // Step files record a failed check in process.exitCode, which an explicit exit would overwrite.
    failures ||= process.exitCode ?? 0;
    if (keepOpen) process.exit(failures ? 1 : 0);

    // Closes it as the user would, which also proves that it shuts down by itself.
    const started = Date.now();
    const closed = new Promise((done) => app.once('exit', done));
    spawn('taskkill', ['/pid', String(app.pid)], { stdio: 'ignore' });
    const code = await Promise.race([closed, sleep(10000).then(() => 'timeout')]);
    console.log(`closed in ${Date.now() - started} ms, exit code ${code}`);
    if (code === 'timeout') app.kill();
    process.exit(failures || code !== 0 ? 1 : 0);
  });
