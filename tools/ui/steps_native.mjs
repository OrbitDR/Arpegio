// What the page cannot reach by itself: media keys, minimizing, the saved window and a first scan from nothing.
// Needs an empty data folder and MUSIC_FOLDER, a folder of real music that is only catalogued, never played.
import { execFileSync } from 'node:child_process';

const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

// Find() lives in C# because PowerShell would turn a null window title into an empty string.
const WINDOW = `
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Host {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int s);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find() { return FindWindowW("ArpegioMainWindow", null); }
}
'@
$h = [Host]::Find()
if ($h -eq [IntPtr]::Zero) { throw 'the window was not found' }`;

const powershell = (script) => execFileSync('powershell', ['-NoProfile', '-Command', `${WINDOW}\n${script}`], { encoding: 'utf8' }).trim();
const mediaKey = (code) => powershell(`[Host]::keybd_event(${code}, 0, 0, [UIntPtr]::Zero); [Host]::keybd_event(${code}, 0, 2, [UIntPtr]::Zero)`);

// What Windows sends when a button under the taskbar thumbnail is pressed: WM_COMMAND with THBN_CLICKED.
const THUMB_PREVIOUS = 1;
const THUMB_TOGGLE = 2;
const THUMB_NEXT = 3;
const thumbnailButton = (id) => powershell(`[void][Host]::SendMessageW($h, 0x111, [IntPtr](0x18000000 + ${id}), [IntPtr]::Zero)`);

export default async function run(page, { sleep }) {
  const status = () => page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; return ${test}; })`, timeout);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);

  // A first scan: the view must not stay on the empty message while the library fills.
  check((await page.eval(`document.querySelector('.page').innerText`)).includes('biblioteca está vacía'), 'an empty library says so');
  await page.call('library.addFolder', { path: process.env.MUSIC_FOLDER });
  await sleep(1500);
  check((await page.eval(`document.querySelector('.page').innerText`)).includes('Leyendo tu música'), 'during the first scan the view says it is reading');
  await page.wait(`document.querySelectorAll('.card').length > 0`, 30000);
  const partial = await page.eval(`import('/js/store.js').then((s) => s.state.library.scanning)`);
  console.log(`albums appeared ${partial ? 'while the scan was still running' : 'after the scan'}`);
  await page.wait(`import('/js/store.js').then((s) => !s.state.library.scanning && s.state.library.revision > 0)`, 120000);
  await sleep(1000);
  await page.wait(`document.querySelectorAll('.card').length > 0`);
  check(true, 'the grid is complete after the scan');

  // Silence through the Windows default output.
  await page.call('library.addFolder', { path: process.env.SILENCE_FOLDER });
  await page.wait(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).some((a) => a.title === 'Pruebas de silencio'))`, 30000);
  const albums = await page.call('library.albums');
  const album = albums.find((item) => item.title === 'Pruebas de silencio');
  const tracks = await page.call('library.albumTracks', { id: album.id });
  await page.call('queue.play', { tracks: tracks.map((track) => track.id), index: 0 });
  await until(`status.state === 'playing' && status.position > 0.3`, 20000);

  mediaKey('0xB3');
  try {
    await until(`status.state === 'paused'`, 2500);
    check(true, 'the play/pause media key pauses');
    mediaKey('0xB3');
    await until(`status.state === 'playing'`, 2500);
    mediaKey('0xB0');
    await until(`status.index === 1`, 2500);
    check(true, 'the next media key skips');
    mediaKey('0xB1');
    await sleep(600);
    mediaKey('0xB1');
    await until(`status.index === 0`, 2500);
    check(true, 'the previous media key goes back');
  } catch {
    check(false, 'media keys: another program may own them');
  }

  thumbnailButton(THUMB_TOGGLE);
  await until(`status.state === 'paused'`, 2500);
  thumbnailButton(THUMB_TOGGLE);
  await until(`status.state === 'playing'`, 2500);
  thumbnailButton(THUMB_NEXT);
  await until(`status.index === 1`, 2500);
  thumbnailButton(THUMB_PREVIOUS);
  await sleep(600);
  thumbnailButton(THUMB_PREVIOUS);
  await until(`status.index === 0`, 2500);
  check(true, 'the taskbar thumbnail buttons reach the application');

  // Music goes on behind a minimized window, and the page comes back up to date.
  powershell('[void][Host]::ShowWindow($h, 6)');
  await sleep(500);
  check(powershell('[Host]::IsIconic($h)') === 'True', 'the window minimizes');
  const before = await status();
  await sleep(3000);
  const after = await status();
  check(after.position > before.position + 2, `playback goes on while minimized (${before.position.toFixed(1)} to ${after.position.toFixed(1)} s)`);
  powershell('[void][Host]::ShowWindow($h, 9)');
  await sleep(500);
  check(powershell('[Host]::IsIconic($h)') === 'False', 'the window restores');
  check((await status()).underruns === 0, 'without a single underrun');

  // The title follows the track.
  const title = powershell(`
    Add-Type -Namespace T -Name U -MemberDefinition '[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(System.IntPtr h, System.Text.StringBuilder s, int n);'
    $s = New-Object Text.StringBuilder 256; [void][T.U]::GetWindowTextW($h, $s, 256); $s.ToString()`);
  check(title.includes('Silencio') && title.includes('Arpegio'), `the window title names the track (${title})`);

}
