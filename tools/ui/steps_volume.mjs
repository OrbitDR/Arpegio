// The volume in the bar and ReplayGain in Ajustes, with real mouse and keyboard events. Plays digital silence
// through whatever output is selected. Needs the silent album in SILENCE_FOLDER. With ARPEGIO_DEVICE it also goes
// through ASIO on that device, where DSD must give up native playback while the volume is down.
const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

const ALBUM = 'Pruebas de silencio';
const WIDTHS = [900, 1081, 1241, 1320, 1600];

export default async function run(page, { sleep }) {
  const status = () => page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; return ${test}; })`, timeout);

  const mouse = (type, x, y, extra = {}) => page.send('Input.dispatchMouseEvent', { type, x, y, ...extra });
  const down = (x, y) => mouse('mousePressed', x, y, { button: 'left', buttons: 1, clickCount: 1 });
  const up = (x, y) => mouse('mouseReleased', x, y, { button: 'left', buttons: 0, clickCount: 1 });
  const box = (selector) => page.eval(`(() => {
    const { left, top, width, height } = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
    return { left, top, width, height };
  })()`);
  const click = async (x, y) => {
    await mouse('mouseMoved', x, y);
    await down(x, y);
    await sleep(100);
    await up(x, y);
  };
  const clickOn = async (selector) => {
    const { left, top, width, height } = await box(selector);
    await click(left + width / 2, top + height / 2);
  };
  const key = (name, code, modifiers = 0) => page.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: name, code: name, windowsVirtualKeyCode: code, modifiers });
  const nextUpdate = async () => {
    const seen = await page.eval('window.__ticks');
    await page.wait(`window.__ticks > ${seen}`, 3000);
  };

  await page.size(1320, 860);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);
  await page.eval(`import('/js/store.js').then((s) => { window.__ticks = 0; s.watch('status', () => ++window.__ticks); })`);

  await page.call('library.addFolder', { path: process.env.SILENCE_FOLDER });
  await page.wait(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).some((a) => a.title === ${JSON.stringify(ALBUM)}))`, 60000);
  const album = (await page.call('library.albums')).find((item) => item.title === ALBUM);
  const tracks = await page.call('library.albumTracks', { id: album.id });
  await page.call('player.setRepeat', { mode: 'queue' });
  await page.call('queue.play', { tracks: tracks.slice(0, 2).map((track) => track.id), index: 0 });
  await until(`status.plan && status.position > 0.5`, 20000);

  // A status arrives every 200 ms and must not pull the slider out from under the pointer.
  check((await status()).volumeDb === 0, 'the volume starts at 0 dB');
  const slider = await box('.bar-volume');
  const y = slider.top + slider.height / 2;
  const at = (share) => slider.left + slider.width * share;
  await mouse('mouseMoved', at(0.5), y);
  await down(at(0.5), y);
  await nextUpdate();
  await nextUpdate();
  const held = await page.eval(`Number(document.querySelector('.bar-volume').value)`);
  await mouse('mouseMoved', at(0.75), y, { button: 'left', buttons: 1 });
  await nextUpdate();
  await nextUpdate();
  const moved = await page.eval(`Number(document.querySelector('.bar-volume').value)`);
  await up(at(0.75), y);
  check(held < -20 && held > -40, `pressing the middle of the slider stays there through a status update (${held} dB)`);
  check(moved > held && moved < 0, `dragging goes on through a status update (${moved} dB)`);
  await until(`status.volumeDb === ${moved}`);
  check(true, 'the application follows the slider');

  await page.go({ view: 'now' });
  const signal = await page.eval(`document.querySelector('.signal').innerText`);
  check(signal.includes('Volumen por software'), 'the signal path names the software volume');
  check(signal.includes('Dither TPDF'), 'and its dither');
  check((await page.eval(`document.querySelector('.bar-status').textContent`)) === 'Procesado', 'the bar says it is processed');
  await page.shot('50-volume-down');

  const volumeBox = await box('.bar-volume');
  await mouse('mouseWheel', volumeBox.left + 10, volumeBox.top + 5, { deltaX: 0, deltaY: -100 });
  await until(`status.volumeDb === ${moved + 1}`);
  check(true, 'the wheel raises it 1 dB');

  await page.eval(`document.activeElement?.blur()`);
  await key('ArrowDown', 40, 2);
  await until(`status.volumeDb === ${moved}`);
  await key('ArrowUp', 38, 2);
  await until(`status.volumeDb === ${moved + 1}`);
  check(true, 'Ctrl+Up and Ctrl+Down move it 1 dB');

  await clickOn('.bar-mute');
  await until(`status.muted`);
  check((await page.eval(`document.querySelector('.bar-mute').dataset.icon`)) === 'volumeOff', 'the speaker shows silence');
  await clickOn('.bar-mute');
  await until(`!status.muted`);
  check((await status()).volumeDb === moved + 1, 'silencing keeps the level');

  await page.call('player.setVolume', { db: 0 });
  await until(`status.volumeDb === 0`);
  const signalAtZero = await page.eval(`document.querySelector('.signal').innerText`);
  check(!signalAtZero.includes('Volumen por software'), 'at 0 dB the signal path does not mention it');

  // Nothing in the bar may be cut or pushed off, at any window size.
  for (const width of WIDTHS) {
    await page.size(width, 700);
    const fits = await page.eval(`(() => {
      const bar = document.querySelector('.bar-right');
      const limit = bar.getBoundingClientRect();
      return bar.scrollWidth <= bar.clientWidth + 1 && [...bar.children].every((child) => {
        const box = child.getBoundingClientRect();
        return getComputedStyle(child).display === 'none' || (box.left >= limit.left - 1 && box.right <= limit.right + 1);
      }) && limit.right <= innerWidth;
    })()`);
    check(fits, `the right of the bar fits at ${width} px`);
    await page.shot(`51-bar-${width}`);
  }
  await page.size(1320, 860);

  await page.go({ view: 'settings' });
  const segment = (text) => `[...document.querySelectorAll('.segment')].find((item) => item.textContent === ${JSON.stringify(text)})`;
  const clickSegment = async (text) => {
    await page.eval(`${segment(text)}.scrollIntoView({ block: 'center' })`);
    await sleep(100);
    const { left, top, width, height } = await page.eval(`(() => {
      const { left, top, width, height } = ${segment(text)}.getBoundingClientRect();
      return { left, top, width, height };
    })()`);
    await click(left + width / 2, top + height / 2);
  };
  await clickSegment('Por álbum');
  await page.wait(`${segment('Por álbum')}.classList.contains('on')`);
  check((await page.call('app.state')).replayGain === 'album', 'ReplayGain changes from Ajustes');
  await page.shot('52-settings-replaygain');
  await clickSegment('Desactivado');
  await page.wait(`${segment('Desactivado')}.classList.contains('on')`);
  check((await page.call('app.state')).replayGain === 'off', 'and goes back');

  const name = process.env.ARPEGIO_DEVICE;
  if (name) {
    const devices = await page.eval(`import('/js/store.js').then((s) => s.state.devices)`);
    const asio = devices.list.find((item) => item.kind === 'asio' && new RegExp(name, 'i').test(item.name));
    if (asio) {
      await page.call('devices.select', { key: asio.key });
      await page.call('queue.play', { tracks: tracks.map((track) => track.id), index: 3 });
      await until(`status.plan && status.plan.path === 'dsdNative' && status.position > 0.3`, 20000);
      check(await page.eval(`document.querySelector('.bar-volume').disabled`), 'with native DSD the slider is disabled');
      let refused = false;
      await page.call('player.setVolume', { db: -6 }).catch(() => { refused = true; });
      check(refused, 'and the application refuses a change from elsewhere');

      await page.call('player.jump', { index: 2 });
      await until(`status.plan && status.plan.path === 'pcm' && status.bitPerfect && status.position > 0.3`, 20000);
      check(!(await page.eval(`document.querySelector('.bar-volume').disabled`)), 'with PCM it works again');
      await page.call('player.setVolume', { db: -6 });
      await page.call('player.jump', { index: 3 });
      await until(`status.plan && status.plan.path === 'dsdToPcm' && status.position > 0.3`, 20000);
      check(true, 'with the volume down, DSD is converted and does not come in at full level');
      await page.call('player.setVolume', { db: 0 });
      await sleep(500);
      check((await status()).plan.path === 'dsdToPcm', 'raising it leaves the track that is sounding as it is');
      await page.call('devices.release');
      await page.call('player.toggle');
      await until(`status.plan && status.plan.path === 'dsdNative' && status.position > 0.3`, 20000);
      check(true, 'at 0 dB the next start is native again');
      check((await status()).underruns === 0, 'without a single underrun');
    }
  }

  await page.call('player.setRepeat', { mode: 'off' });
  await page.call('queue.clear');
}
