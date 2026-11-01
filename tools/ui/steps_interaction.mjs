// Everything a person does with the mouse and keyboard, in the real application, with digital silence.
// Continues from what steps_outputs.mjs leaves behind: a queue of four tracks, paused on the last one.
import { execFileSync, spawn, spawnSync } from 'node:child_process';

const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

async function key(page, keyName, modifiers = 0, code = keyName) {
  const virtualCodes = { ' ': 32, f: 70, ArrowRight: 39, ArrowLeft: 37, Escape: 27 };
  const common = { key: keyName, code, modifiers, windowsVirtualKeyCode: virtualCodes[keyName] };
  await page.send('Input.dispatchKeyEvent', { type: 'keyDown', ...common, text: keyName === ' ' ? ' ' : undefined });
  await page.send('Input.dispatchKeyEvent', { type: 'keyUp', ...common });
}

export default async function run(page, { sleep }) {
  const status = () => page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  const queue = () => page.eval(`import('/js/store.js').then((s) => s.state.queue)`);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; const queue = s.state.queue; return ${test}; })`, timeout);
  const text = (selector) => page.eval(`document.querySelector(${JSON.stringify(selector)})?.textContent ?? null`);
  const count = (selector) => page.eval(`document.querySelectorAll(${JSON.stringify(selector)}).length`);

  await page.size(1320, 860);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);

  // What the previous run left behind.
  const restored = await status();
  const restoredQueue = await queue();
  check(restoredQueue.tracks.length === 4, `queue restored after a restart (${restoredQueue.tracks.length} tracks)`);
  check(restored.index === 3, `current track restored (index ${restored.index})`);
  check(restored.position > 5, `position restored (${restored.position.toFixed(1)} s)`);
  check(restored.state === 'stopped', 'nothing plays by itself at start');
  check((await text('.bar-title')) === restoredQueue.tracks[3].title, 'the bar shows the restored track');
  check(await page.eval(`document.querySelector('.seek').value > 5`), 'the seek bar shows the restored position');
  check((await page.eval(`import('/js/store.js').then((s) => s.state.devices.selected)`)).startsWith('asio:'), 'output restored');
  check((await text('.nav-playlists')).includes('Para probar'), 'playlist restored');

  // The empty-looking views must not show stray text.
  for (const route of [{ view: 'albums' }, { view: 'artists' }, { view: 'tracks' }, { view: 'favorites' }, { view: 'settings' }, { view: 'folders' }]) {
    await page.go(route);
    await sleep(200);
    check(!(await page.eval(`document.querySelector('.page').innerText.split('\\n').includes('null')`)), `no stray text in ${route.view}`);
  }
  const anyArtist = await page.eval(`import('/js/api.js').then(async (api) => (await api.call('library.artists')).find((a) => a.albums === 0 && a.tracks > 0))`);
  await page.go({ view: 'artist', id: anyArtist.id });
  await sleep(300);
  check(!(await page.eval(`document.querySelector('.page').innerText.split('\\n').includes('null')`)), 'no stray text for an artist without albums');

  // Long lists stay short in the page, and scrolling draws the right rows.
  await page.go({ view: 'tracks' });
  await sleep(400);
  const rows = await count('.track-row');
  await page.eval(`document.getElementById('scroller').scrollTop = 1e7`);
  await sleep(400);
  check(rows < 40 && (await count('.track-row')) < 40, `tracks stay virtual (${rows} rows in the page)`);
  const lastNumber = await page.eval(`Math.max(...[...document.querySelectorAll('.track-number')].map((n) => Number(n.textContent)))`);
  check(lastNumber > 5000, `the end of the list is reachable (row ${lastNumber})`);

  await page.eval(`[...document.querySelectorAll('.sort')].find((b) => b.textContent.startsWith('Duración')).click()`);
  await sleep(200);
  await page.eval(`document.getElementById('scroller').scrollTop = 0`);
  await sleep(300);
  const shortest = await page.eval(`document.querySelector('.track-row .track-time').textContent`);
  await page.eval(`[...document.querySelectorAll('.sort')].find((b) => b.textContent.startsWith('Duración')).click()`);
  await sleep(300);
  const longest = await page.eval(`document.querySelector('.track-row .track-time').textContent`);
  check(shortest !== longest, `sorting by duration flips the order (${shortest}, ${longest})`);

  await page.go({ view: 'albums' });
  await sleep(300);
  await page.eval(`document.getElementById('scroller').scrollTop = 3000`);
  await sleep(300);
  const firstCard = await text('.card .card-title');
  await page.eval(`document.querySelector('.card').click()`);
  await sleep(500);
  check((await count('.album-header')) === 1, 'clicking a card opens the album');
  await page.eval(`history.back()`);
  await sleep(600);
  const scrollTop = await page.eval(`document.getElementById('scroller').scrollTop`);
  check(scrollTop === 3000, `going back returns to the same place (${scrollTop})`);
  check((await text('.card .card-title')) === firstCard, 'and the same album cards');

  // Keyboard.
  await key(page, 'f', 2, 'KeyF');
  await sleep(100);
  check((await page.eval(`document.activeElement.id`)) === 'search', 'Ctrl+F focuses the search box');
  await page.send('Input.insertText', { text: 'silencio' });
  await sleep(700);
  check((await page.eval(`(history.state ?? {}).view`)) === 'search', 'typing opens the search results');
  check((await text('.page-header h1')) === '«silencio»', 'for what was typed');
  await key(page, 'Escape', 0, 'Escape');
  await sleep(100);
  await page.eval(`document.activeElement.blur()`);

  await page.call('queue.play', { tracks: restoredQueue.tracks.map((t) => t.id), index: 0 });
  await until(`status.state === 'playing' && status.position > 0.3`, 20000);
  await key(page, ' ', 0, 'Space');
  await until(`status.state === 'paused'`);
  check(true, 'space pauses');
  await key(page, ' ', 0, 'Space');
  await until(`status.state === 'playing'`);
  check(true, 'space resumes');
  await key(page, 'ArrowRight', 2, 'ArrowRight');
  await until(`status.index === 1`);
  check(true, 'Ctrl+Right goes to the next track');
  await key(page, 'ArrowRight', 8, 'ArrowRight');
  await until(`status.position >= 10`);
  check(true, 'Shift+Right skips ten seconds');

  // The seek bar, as a person drags it.
  await page.eval(`(() => {
    const seek = document.querySelector('.seek');
    seek.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    seek.value = 30;
    seek.dispatchEvent(new Event('input', { bubbles: true }));
    seek.dispatchEvent(new Event('change', { bubbles: true }));
  })()`);
  await until(`status.position >= 30`);
  check(true, 'dragging the seek bar moves the playback');

  // Repeat button.
  await page.click('.bar-buttons .icon-button:last-child');
  await until(`status.repeat === 'queue'`);
  await page.click('.bar-buttons .icon-button:last-child');
  await until(`status.repeat === 'track'`);
  await page.click('.bar-buttons .icon-button:last-child');
  await until(`status.repeat === 'off'`);
  check(true, 'the repeat button cycles through its modes');

  // Queue panel.
  await page.click('.bar-right .icon-button:last-child');
  await sleep(400);
  check((await count('.queue-row')) === 4, 'the queue panel lists the tracks');
  check((await text('.queue-row.current .queue-title')) === (await queue()).tracks[(await status()).index].title, 'and marks the current one');

  await page.eval(`(() => {
    const rows = document.querySelectorAll('.queue-row');
    const data = new DataTransfer();
    rows[3].dispatchEvent(new DragEvent('dragstart', { bubbles: true, dataTransfer: data }));
    rows[0].dispatchEvent(new DragEvent('dragover', { bubbles: true, cancelable: true, dataTransfer: data }));
    rows[0].dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer: data }));
  })()`);
  await until(`queue.tracks[0].codec === 'DSF'`);
  check((await status()).index === 2, 'dragging a track above the current one keeps the current track');

  await page.eval(`document.querySelectorAll('.queue-row')[3].dispatchEvent(new MouseEvent('dblclick', { bubbles: true }))`);
  await until(`status.index === 3`);
  check(true, 'double click on a queue row plays it');
  await page.click('.queue-row:first-child .queue-remove');
  await until(`queue.tracks.length === 3`);
  check((await count('.queue-row')) === 3, 'removing from the queue updates the panel');
  await page.click('.bar-right .icon-button:last-child');

  // Favourites and context menu.
  const favorites = await page.call('library.favorites');
  await page.go({ view: 'favorites' });
  await page.eval(`document.querySelector('.track-row .row-favorite').click()`);
  await sleep(300);
  check((await page.call('library.favorites')).length === favorites.length - 1, 'the heart removes a favourite');

  const playlists = await page.call('playlists.list');
  await page.go({ view: 'playlist', id: playlists[0].id });
  await sleep(300);
  await page.eval(`(() => {
    const row = document.querySelector('.track-row');
    const box = row.getBoundingClientRect();
    row.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: box.left + 100, clientY: box.top + 10 }));
  })()`);
  await sleep(200);
  check((await page.eval(`[...document.querySelectorAll('.menu-label')].map((m) => m.textContent).join()`)).includes('Quitar de esta lista'), 'a playlist row offers removal');
  await page.eval(`[...document.querySelectorAll('.menu-item')].find((m) => m.textContent.includes('Quitar de esta lista')).click()`);
  await sleep(500);
  check((await page.call('playlists.tracks', { id: playlists[0].id })).length === 1, 'and it removes the track');

  await page.eval(`(() => {
    const link = [...document.querySelectorAll('.nav-playlists .nav-link')][0];
    link.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 60, clientY: 330 }));
  })()`);
  await sleep(150);
  await page.eval(`[...document.querySelectorAll('.menu-item')].find((m) => m.textContent.includes('Cambiar nombre')).click()`);
  await sleep(200);
  check((await count('.dialog')) === 1, 'renaming asks for a name');
  await page.eval(`document.querySelector('.dialog input').value = 'Otra'; document.querySelector('.dialog').requestSubmit()`);
  await sleep(300);
  check((await text('.nav-playlists')).includes('Otra'), 'and the sidebar shows it');
  await page.call('playlists.delete', { id: playlists[0].id });

  // Tags from files must stay text, whatever they contain.
  const injected = await page.eval(`import('/js/parts.js').then((parts) => {
    const list = [{ id: 1, title: '<img src=x onerror=alert(1)>', artist: '<b>x</b>', albumArtist: 'y', seconds: 1, rate: 44100, bits: 16, codec: 'FLAC' }];
    const row = parts.trackRow(list, 0);
    return row.querySelector('img') === null && row.querySelector('b') === null && row.textContent.includes('<img');
  })`);
  check(injected, 'tags that look like HTML stay plain text');

  // The real folder dialog opens, and cancelling it changes nothing.
  const folders = (await page.call('library.folders')).length;
  await page.eval(`(() => {
    window.__picked = 'pending';
    import('/js/api.js').then((api) => api.call('library.pickFolder'))
      .then((r) => { window.__picked = r === null ? 'cancelled' : 'added'; })
      .catch((e) => { window.__picked = 'error: ' + e.message; });
    return 1;
  })()`);
  await sleep(1500);
  check((await page.eval(`window.__picked`)) === 'pending', 'the folder dialog waits for the user');
  const close = `
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Dlg {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static bool Close(uint pid) {
    IntPtr h = IntPtr.Zero;
    while ((h = FindWindowExW(IntPtr.Zero, h, "#32770", null)) != IntPtr.Zero) {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) { PostMessageW(h, 0x10, IntPtr.Zero, IntPtr.Zero); return true; }
    }
    return false;
  }
}
'@
[Dlg]::Close(${process.env.ARPEGIO_PID ?? 0})`;
  const pid = execFileSync('powershell', ['-NoProfile', '-Command',
    `(Get-Process arpegio | Where-Object { $_.Path -eq $env:ARPEGIO_EXE } | Select-Object -First 1).Id`], { encoding: 'utf8' }).trim();
  const closed = execFileSync('powershell', ['-NoProfile', '-Command', close.replace('[Dlg]::Close(0)', `[Dlg]::Close(${pid})`)], { encoding: 'utf8' }).trim();
  check(closed === 'True', `the folder dialog is a real window (${closed})`);
  await page.wait(`window.__picked !== 'pending'`);
  check((await page.eval(`window.__picked`)) === 'cancelled', 'closing it counts as cancelling');
  check((await page.call('library.folders')).length === folders, 'and adds nothing');

  // A second start only brings the first window forward.
  const second = spawnSync(process.env.ARPEGIO_EXE, ['--data', process.env.ARPEGIO_DATA], { timeout: 8000 });
  check(second.status === 0, `a second copy exits at once (status ${second.status})`);
  check((await status()).state !== undefined, 'and the first one keeps working');

  // Narrow window.
  await page.go({ view: 'tracks' });
  await page.size(900, 600);
  await sleep(500);
  await page.shot('40-narrow-tracks');
  check(await page.eval(`document.documentElement.scrollWidth <= innerWidth`), 'no sideways scrolling at the minimum size');
  await page.go({ view: 'now' });
  await sleep(300);
  await page.shot('41-narrow-now');
  await page.go({ view: 'settings' });
  await sleep(300);
  await page.shot('42-narrow-settings');
  await page.size(1320, 860);

  await page.call('queue.clear');
  await until(`status.state === 'stopped'`);
}
