// Where the buttons take a click, with real mouse events. element.click() skips the hit test and never
// sees a press that goes astray, so here each point of each button is pressed and held through a status
// update, as a person's click is about one time in two. Then the button that frees the device, against
// another program that wants it. Plays digital silence, in exclusive mode if the output in ARPEGIO_DEVICE
// has it. Needs the silent album in SILENCE_FOLDER. With MUSIC_FOLDER it also presses during a scan.
import { spawnSync } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';

const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

const ALBUM = 'Pruebas de silencio';
const BAR_CONTROLS = [
  ['cover', '.bar-art', 5],
  ['favorite', '.bar-favorite', 7],
  ['shuffle', '.bar-buttons .icon-button:nth-child(1)', 7],
  ['previous', '.bar-buttons .icon-button:nth-child(2)', 7],
  ['play/pause', '.bar-toggle', 9],
  ['next', '.bar-buttons .icon-button:nth-child(4)', 7],
  ['repeat', '.bar-buttons .icon-button:nth-child(5)', 7],
  ['quality', '.bar-quality', 5],
  ['device', '.bar-device', 7],
  ['queue', '.bar-right .icon-button:last-child', 7],
];

// The centre of every button that is on screen must belong to that button.
const COVERED_BUTTONS = `(() => {
  const covered = [];
  for (const button of document.querySelectorAll('button')) {
    const box = button.getBoundingClientRect();
    const x = box.left + box.width / 2;
    const y = box.top + box.height / 2;
    const clip = button.closest('.scroller, .queue-scroll, .nav-playlists')?.getBoundingClientRect();
    if (!box.width || !box.height || x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) continue;
    if (clip && (y < clip.top || y >= clip.bottom)) continue;
    if (document.elementFromPoint(x, y)?.closest('button') !== button) covered.push(button.className || button.textContent);
  }
  return covered;
})()`;

export default async function run(page, { sleep }) {
  const status = () => page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; return ${test}; })`, timeout);

  const mouse = (type, x, y, extra = {}) => page.send('Input.dispatchMouseEvent', { type, x, y, ...extra });
  const down = (x, y) => mouse('mousePressed', x, y, { button: 'left', buttons: 1, clickCount: 1 });
  const up = (x, y) => mouse('mouseReleased', x, y, { button: 'left', buttons: 0, clickCount: 1 });
  const center = (selector) => page.eval(`(() => {
    const box = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
    return [box.left + box.width / 2, box.top + box.height / 2];
  })()`);

  // Holds the button down until `counter` in the page has gone up, or for a moment if it is not given.
  const press = async (x, y, counter) => {
    await mouse('mouseMoved', x, y);
    const count = counter ? await page.eval(counter) : 0;
    await down(x, y);
    if (counter) await page.wait(`${counter} > ${count}`, 3000);
    else await sleep(120);
    await up(x, y);
  };
  const ticks = 'window.__ticks';
  const clicksOn = (selector) => page.eval(`window.__clicks.filter((button) => button?.matches(${JSON.stringify(selector)})).length`);
  const countClicks = (on) => page.eval(`document.${on ? 'add' : 'remove'}EventListener('click', window.__count, true)`);

  await page.size(1320, 860);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);

  const folder = process.env.SILENCE_FOLDER;
  const folders = await page.call('library.folders');
  if (folder && !folders.some((item) => item.path === folder)) await page.call('library.addFolder', { path: folder });
  await page.wait(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).some((a) => a.title === ${JSON.stringify(ALBUM)}))`, 60000);

  const album = (await page.call('library.albums')).find((item) => item.title === ALBUM);
  const tracks = await page.call('library.albumTracks', { id: album.id });

  const deviceText = process.env.ARPEGIO_DEVICE ?? 'topping';
  const devices = await page.eval(`import('/js/store.js').then((s) => s.state.devices)`);
  const exclusive = devices.list.find((item) => item.kind === 'exclusive' && !item.error && new RegExp(deviceText, 'i').test(item.name));
  if (exclusive) await page.call('devices.select', { key: exclusive.key });

  // Two tracks of one format: the stream never reopens, so the device button stays enabled.
  await page.call('player.setRepeat', { mode: 'queue' });
  await page.call('queue.play', { tracks: tracks.slice(0, 2).map((track) => track.id), index: 0 });
  await until(`status.plan && status.position > 0.5`, 20000);
  const held = (await status()).deviceHeld;
  check(held === Boolean(exclusive), `the device counts as held only in exclusive mode (${held})`);

  for (const route of [{ view: 'albums' }, { view: 'tracks' }, { view: 'album', id: album.id }, { view: 'now' }, { view: 'settings' }]) {
    await page.go(route);
    await sleep(300);
    const covered = await page.eval(COVERED_BUTTONS);
    check(covered.length === 0, `nothing covers the buttons of ${route.view} ${covered.join(', ')}`);
  }
  await page.go({ view: 'albums' });

  await page.eval(`import('/js/store.js').then((s) => {
    window.__ticks = 0;
    window.__clicks = [];
    s.watch('status', () => { window.__ticks += 1; });
    window.__count = (event) => {
      window.__clicks.push(event.target.closest('button'));
      event.stopPropagation();
      event.preventDefault();
    };
    return true;
  })`);
  await countClicks(true);

  await page.eval(`window.__parts = [...document.querySelectorAll('.bar-toggle svg, .bar-buttons svg, .bar-favorite svg, .bar-quality span')]`);
  await page.wait(`${ticks} > 3`);
  check(await page.eval(`window.__parts.length > 7 && window.__parts.every((part) => part.isConnected)`), 'a status update leaves what is inside the buttons in place');

  for (const [name, selector, side] of BAR_CONTROLS) {
    if (name === 'device' && !held) continue;
    const found = await page.eval(`(() => {
      const element = document.querySelector(${JSON.stringify(selector)});
      if (!element) return null;
      const box = element.getBoundingClientRect();
      return { left: box.left, top: box.top, width: box.width, height: box.height, round: getComputedStyle(element).borderRadius === '50%' };
    })()`);
    if (!found) {
      check(false, `${name} is on the page`);
      continue;
    }

    const lines = [];
    let inside = 0;
    let taken = 0;
    for (let row = 0; row < side; ++row) {
      let line = '';
      for (let column = 0; column < side; ++column) {
        const x = found.left + (found.width * (column + 0.5)) / side;
        const y = found.top + (found.height * (row + 0.5)) / side;
        const dx = (x - found.left) / found.width - 0.5;
        const dy = (y - found.top) / found.height - 0.5;
        if (found.round && Math.hypot(dx, dy) > 0.48) {
          line += '  ';
          continue;
        }
        ++inside;
        const before = await clicksOn(selector);
        await press(x, y, ticks);
        const hit = (await clicksOn(selector)) > before;
        if (hit) ++taken;
        line += hit ? '# ' : '. ';
      }
      lines.push(`     ${line.trimEnd()}`);
    }
    check(taken === inside, `${name}: ${taken} of ${inside} points take the click`);
    if (taken !== inside) console.log(lines.join('\n'));
  }
  await mouse('mouseMoved', 1, 1);

  // The icon can also change for a good reason between the press and the release.
  const [toggleX, toggleY] = await center('.bar-toggle');
  const beforeSwap = await clicksOn('.bar-toggle');
  await mouse('mouseMoved', toggleX, toggleY);
  await down(toggleX, toggleY);
  await page.eval(`import('/js/api.js').then((api) => api.call('player.toggle'))`);
  await page.wait(`document.querySelector('.bar-toggle').dataset.icon === 'play'`);
  await up(toggleX, toggleY);
  check((await clicksOn('.bar-toggle')) > beforeSwap, 'a click survives the icon changing during the press');
  await page.eval(`import('/js/api.js').then((api) => api.call('player.toggle'))`);
  await until(`status.state === 'playing'`);
  await countClicks(false);

  check((await status()).state === 'playing', 'the sweep changed nothing');
  await press(toggleX, toggleY, ticks);
  await until(`status.state === 'paused'`);
  check(true, 'a real click on the icon pauses');
  await press(toggleX, toggleY);
  await until(`status.state === 'playing'`);
  check(true, 'and the next one resumes');

  const [repeatX, repeatY] = await center('.bar-buttons .icon-button:nth-child(5)');
  await press(repeatX, repeatY, ticks);
  await until(`status.repeat === 'track'`);
  check(true, 'a real click on the repeat icon changes the mode');
  await page.call('player.setRepeat', { mode: 'queue' });

  // Pressing the seek thumb without moving it is a press that sends no change event.
  const seekState = `(() => {
    const seek = document.querySelector('.seek');
    const box = seek.getBoundingClientRect();
    return { value: Number(seek.value), max: Number(seek.max), left: box.left, width: box.width, y: box.top + box.height / 2 };
  })()`;
  const seek = await page.eval(seekState);
  await press(seek.left + 6 + (seek.width - 12) * (seek.value / seek.max), seek.y, ticks);
  await mouse('mouseMoved', 1, 1);
  await sleep(1600);
  const following = await page.eval(seekState);
  check(Math.abs(following.value - (await status()).position) < 1, `the bar follows the music after a press on its thumb (${following.value})`);

  const target = following.max * 0.75;
  await press(seek.left + 6 + (seek.width - 12) * 0.75, seek.y, ticks);
  await until(`status.position > ${target - 2} && status.position < ${target + 4}`);
  check(true, 'a real click on the seek bar moves the playback');
  await mouse('mouseMoved', 1, 1);

  // A new track marks its row without replacing the rows, which are under the pointer.
  await page.eval(`import('/js/store.js').then((s) => s.set('queueOpen', true))`);
  await sleep(400);
  await page.eval(`window.__row = document.querySelector('.queue-row.current')`);
  const index = (await status()).index;
  await page.call('player.next');
  await until(`status.index !== ${index}`);
  await sleep(300);
  check(await page.eval(`window.__row.isConnected && !window.__row.classList.contains('current')`), 'the queue moves its mark in place');
  check((await page.eval(`document.querySelectorAll('.queue-row.current').length`)) === 1, 'and one row is marked');
  await page.eval(`import('/js/store.js').then((s) => s.set('queueOpen', false))`);
  await sleep(300);

  const deviceState = `(() => {
    const button = document.querySelector('.bar-device');
    return { on: button.classList.contains('on'), disabled: button.disabled, title: button.title };
  })()`;

  const cli = join(dirname(resolve(process.env.ARPEGIO_EXE ?? 'build/release/arpegio.exe')), 'arpegio-cli.exe');
  const anotherProgram = (output = 'exclusive') => spawnSync(cli, ['play', join(folder, ALBUM, '01.wav'), '--output', output, '--device', deviceText, '--seconds', '1'],
    { encoding: 'utf8', timeout: 30000 });

  if (held) {
    await page.shot('50-device-held');
    const refused = anotherProgram();
    check(refused.status !== 0, `another program cannot open the device while it is held (${(refused.stderr || '').trim()})`);
    const shown = await page.eval(deviceState);
    check(shown.on && !shown.disabled, 'the button shows it held');

    await page.call('player.setRepeat', { mode: 'off' });
    const before = await status();
    const [deviceX, deviceY] = await center('.bar-device');
    await press(deviceX, deviceY, ticks);
    await until(`status.state === 'stopped' && !status.deviceHeld`);
    await mouse('mouseMoved', 1, 1);
    await sleep(400);
    const released = await status();
    check(Math.abs(released.position - before.position) < 2, `a real click frees it and keeps the place (${before.position.toFixed(1)} to ${released.position.toFixed(1)})`);
    check(released.index === before.index, 'on the same track');
    const free = await page.eval(deviceState);
    check(!free.on && free.disabled, 'the button shows it free');
    check((await page.eval(`document.querySelector('.bar-quality .badge') === null`)), 'there is no signal path to judge');
    await page.shot('51-device-free');

    const allowed = anotherProgram();
    check(allowed.status === 0, `another program opens the device once it is free ${(allowed.stderr || '').trim()}`);
    await sleep(1000);
    check((await status()).state === 'stopped', 'and nothing takes it back meanwhile');

    await press(toggleX, toggleY);
    await until(`status.deviceHeld && status.position > ${released.position}`, 20000);
    check(true, 'a real click on play takes it back from the same place');
    check((await status()).index === before.index, 'on the same track');
    check(anotherProgram().status !== 0, 'and keeps the other program out again');
    await sleep(300);
    const back = await page.eval(deviceState);
    check(back.on && !back.disabled, 'the button shows it held again');
  }

  // Native DSD only goes through ASIO. Nothing may try the driver while it is held: a second copy of one
  // in use can bring it down.
  const asio = devices.list.find((item) => item.kind === 'asio' && !item.error && new RegExp(deviceText, 'i').test(item.name));
  const dsd = tracks.find((track) => track.dsd);
  if (asio && dsd) {
    await page.call('devices.select', { key: asio.key });
    await page.call('queue.play', { tracks: [dsd.id] });
    await until(`status.deviceHeld && status.plan.path === 'dsdNative' && status.position > 1`, 20000);
    const before = await status();
    const [deviceX, deviceY] = await center('.bar-device');
    await press(deviceX, deviceY, ticks);
    await until(`status.state === 'stopped' && !status.deviceHeld`);
    await mouse('mouseMoved', 1, 1);
    const released = await status();
    check(Math.abs(released.position - before.position) < 2, `a real click frees the ASIO driver during native DSD (${before.position.toFixed(1)} to ${released.position.toFixed(1)})`);

    const allowed = anotherProgram('asio');
    check(allowed.status === 0, `another program plays PCM through that driver ${(allowed.stderr || '').trim()}`);

    // Space, because the button that kept the focus was disabled under it.
    const space = { key: ' ', code: 'Space', windowsVirtualKeyCode: 32 };
    await page.send('Input.dispatchKeyEvent', { type: 'keyDown', ...space, text: ' ' });
    await page.send('Input.dispatchKeyEvent', { type: 'keyUp', ...space });
    await until(`status.deviceHeld && status.plan.path === 'dsdNative' && status.position > ${released.position}`, 20000);
    check(true, 'and Space goes back to native DSD from the same place');
    await sleep(1500);
    const after = await status();
    check(after.underruns === 0 && after.late === 0, `without cuts (${after.underruns} underruns, ${after.late} late)`);
  }

  // The bar must keep its place with the longest labels, down to the smallest window.
  for (const width of [1320, 1081, 1080, 900]) {
    await page.size(width, 700);
    await sleep(300);
    const spill = await page.eval(`(() => {
      document.querySelector('.bar-format')?.replaceChildren('MQA 24/352.8');
      const right = document.querySelector('.bar-right').getBoundingClientRect();
      const center = document.querySelector('.bar-center').getBoundingClientRect();
      const quality = document.querySelector('.bar-quality');
      const device = document.querySelector('.bar-device').getBoundingClientRect();
      const queue = document.querySelector('.bar-right .icon-button:last-child').getBoundingClientRect();
      return right.left >= center.right && quality.scrollWidth <= quality.clientWidth && device.right <= queue.left && queue.right <= innerWidth;
    })()`);
    check(spill, `the right of the bar fits at ${width} px`);
  }
  await page.size(1320, 860);

  // A running scan used to redraw all of Ajustes.
  const music = process.env.MUSIC_FOLDER;
  if (music) {
    await page.go({ view: 'settings' });
    if ((await page.call('library.folders')).some((item) => item.path === music)) await page.call('library.rescan', { force: true });
    else await page.call('library.addFolder', { path: music });
    await page.wait(`import('/js/store.js').then((s) => s.state.library.scanning && s.state.library.done > 0)`, 30000);
    await page.eval(`import('/js/store.js').then((s) => {
      window.__scans = 0;
      s.watch('library', () => { window.__scans += 1; });
      return true;
    })`);
    const rescan = '.library-actions .button:nth-child(2)';
    await page.eval(`document.querySelector(${JSON.stringify(rescan)}).scrollIntoView({ block: 'center' })`);
    await sleep(200);
    await countClicks(true);
    const [rescanX, rescanY] = await center(rescan);
    const pressed = await clicksOn(rescan);
    for (let i = 0; i < 8; ++i) await press(rescanX, rescanY, 'window.__scans');
    await countClicks(false);
    const taken = (await clicksOn(rescan)) - pressed;
    check(taken === 8, `Ajustes takes every click during a scan (${taken} of 8)`);
    await mouse('mouseMoved', 1, 1);
    check((await page.eval(`document.querySelector('.scan-status p')?.textContent ?? ''`)).startsWith('Leyendo'), 'and its progress keeps moving');
    await page.wait(`import('/js/store.js').then((s) => !s.state.library.scanning)`, 180000);
    await sleep(400);
    check((await page.eval(`document.querySelector('.scan-status')`)) === null, 'the progress goes away at the end');
  }

  await page.call('player.setRepeat', { mode: 'off' });
  await page.call('queue.clear');
  await until(`status.state === 'stopped'`);
  await sleep(300);
  check(!(await page.eval(deviceState)).on, 'an empty queue leaves the device free');
  await page.call('devices.select', { key: devices.selected });
}
