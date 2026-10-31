// Real outputs with digital silence: every route, driven only through the page.
const FOLDER = process.env.SILENCE_FOLDER;
const device = new RegExp(process.env.ARPEGIO_DEVICE ?? 'topping', 'i');

export default async function run(page, { sleep }) {
  const status = () => page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  const devices = () => page.eval(`import('/js/store.js').then((s) => s.state.devices)`);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; return ${test}; })`, timeout);
  const log = async (label) => {
    const s = await status();
    console.log(`${label}: ${s.state} index ${s.index} at ${s.position.toFixed(2)}/${s.duration.toFixed(1)} ` +
      `${s.plan ? `${s.plan.path} ${s.plan.encoding} ${s.plan.rate} ch${s.plan.channels}` : 'no plan'} ` +
      `bitPerfect=${s.bitPerfect} winGain=${s.windowsGainDb} underruns=${s.underruns} late=${s.late} error="${s.error}"`);
    return s;
  };

  await page.size(1320, 860);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);
  const folders = await page.call('library.folders');
  if (!folders.some((folder) => folder.path === FOLDER)) await page.call('library.addFolder', { path: FOLDER });
  // The next run needs a long list to scroll through.
  const music = process.env.MUSIC_FOLDER;
  if (music && !folders.some((folder) => folder.path === music)) await page.call('library.addFolder', { path: music });
  await page.wait(`import('/js/store.js').then((s) => !s.state.library.scanning)`, 120000);
  await page.wait(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).some((a) => a.title === 'Pruebas de silencio'))`, 30000);
  await sleep(600);

  const album = await page.eval(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).find((a) => a.title === 'Pruebas de silencio'))`);
  const tracks = await page.call('library.albumTracks', { id: album.id });
  console.log('album', album.title, album.tracks, album.codec, album.rate, 'art', album.art, tracks.map((t) => `${t.track}:${t.title}:${t.codec}`).join(', '));
  const ids = tracks.map((t) => t.id);

  console.log('selected', (await devices()).selected);
  await page.call('queue.play', { tracks: ids, index: 0 });
  await until(`status.plan && status.position > 0.5`);
  await log('shared');
  await page.go({ view: 'now' });
  await sleep(300);
  await page.shot('20-now-shared');

  await page.call('player.seek', { seconds: 20 });
  await until(`status.position >= 20 && status.position < 25`);
  await log('after seek');

  let t = Date.now();
  await page.call('player.next');
  await until(`status.index === 1 && status.position < 5`);
  console.log(`next took ${Date.now() - t} ms`);
  await sleep(4000);
  await log('track 2');
  await page.call('player.previous');
  await sleep(600);
  await log('previous restarts');
  await page.call('player.previous');
  await until(`status.index === 0`);
  await log('previous goes back');
  await page.call('player.next');
  await until(`status.index === 1`);

  await page.eval(`import('/js/store.js').then((s) => s.set('queueOpen', true))`);
  await sleep(400);
  await page.go({ view: 'album', id: album.id });
  await sleep(300);
  await page.shot('21-album-queue');

  await page.eval(`(() => {
    const row = document.querySelector('.track-row');
    const box = row.getBoundingClientRect();
    row.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: box.left + 260, clientY: box.top + 20 }));
  })()`);
  await sleep(250);
  await page.shot('22-menu');
  await page.eval(`document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }))`);
  await page.eval(`import('/js/store.js').then((s) => s.set('queueOpen', false))`);

  await page.go({ view: 'now' });
  const list = (await devices()).list;
  const exclusive = list.find((d) => d.kind === 'exclusive' && device.test(d.name));
  const asio = list.find((d) => d.kind === 'asio' && device.test(d.name));

  const before = await status();
  t = Date.now();
  await page.call('devices.select', { key: exclusive.key });
  console.log(`switching output took ${Date.now() - t} ms in the page thread`);
  await until(`status.plan && status.plan.path === 'pcm' && status.bitPerfect !== undefined && status.position > ${before.position}`);
  await sleep(1500);
  await log('exclusive');
  await sleep(300);
  await page.shot('23-now-exclusive');

  await page.call('player.jump', { index: 2 });
  await until(`status.index === 2 && status.plan && status.plan.rate === 96000`);
  await sleep(1500);
  await log('exclusive 96k');

  await page.call('devices.select', { key: asio.key });
  await sleep(500);
  await page.call('player.jump', { index: 3 });
  await until(`status.index === 3 && status.plan && status.plan.path === 'dsdNative'`, 20000);
  await sleep(2500);
  await log('asio native');
  await sleep(300);
  await page.shot('24-now-dsd-native');

  await page.call('devices.configure', { key: asio.key, settings: { dsd: 'dop' } });
  await until(`status.plan && status.plan.path === 'dop'`, 20000);
  await sleep(2500);
  await log('asio dop');

  await page.call('devices.configure', { key: asio.key, settings: { dsd: 'pcm' } });
  await until(`status.plan && status.plan.path === 'dsdToPcm'`, 20000);
  await sleep(2500);
  await log('asio dsd to pcm');
  await sleep(300);
  await page.shot('25-now-dsd-pcm');

  await page.call('devices.configure', { key: asio.key, settings: { dsd: 'auto' } });
  await until(`status.plan && status.plan.path === 'dsdNative'`, 20000);
  await sleep(800);

  await page.call('player.toggle');
  await until(`status.state === 'paused'`);
  await log('paused');
  await page.go({ view: 'settings' });
  await sleep(300);
  await page.shot('26-settings-asio');
  await page.eval(`document.getElementById('scroller').scrollTop = 99999`);
  await sleep(300);
  await page.shot('27-settings-library');

  await page.call('playlists.create', { name: 'Para probar', tracks: ids.slice(0, 2) });
  await page.call('library.setFavorite', { id: ids[1], favorite: true });
  await page.go({ view: 'favorites' });
  await page.shot('28-favorites');
  await page.go({ view: 'search', text: 'silencio' });
  await sleep(500);
  await page.shot('29-search');

  const folder = (await page.call('library.folders')).find((item) => item.path === FOLDER);
  await page.go({ view: 'folders', folder: folder.id, path: ['Pruebas de silencio'] });
  await sleep(300);
  await page.shot('30-folder');
}
