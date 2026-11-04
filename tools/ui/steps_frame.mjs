// The window's own frame and the player bar's layout: the title bar the page draws, its buttons against the
// real window, and the transport in the middle whatever the sides hold. Plays nothing, so it can run with
// --no-devices while another program uses the DAC. Needs the silent album in SILENCE_FOLDER.
import { execFileSync } from 'node:child_process';

const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

const ALBUM = 'Pruebas de silencio';
const WS_CAPTION = 0x00C00000;
const WS_THICKFRAME = 0x00040000;
const WS_SYSMENU = 0x00080000;
const WS_MINIMIZEBOX = 0x00020000;
const WS_MAXIMIZEBOX = 0x00010000;
const HTCAPTION = 2;

// Finds the window by process, since the user's own Arpegio may be open beside this one.
const NATIVE = (pid) => `
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Frame {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int s);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr m, ref MONITORINFO i);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int Size; public RECT Monitor, Work; public uint Flags; }
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var s = new StringBuilder(64); GetClassNameW(h, s, 64);
      if (p == pid && s.ToString() == "ArpegioMainWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  static string Box(RECT r) { return "[" + r.Left + "," + r.Top + "," + r.Right + "," + r.Bottom + "]"; }
  public static string Info(IntPtr h) {
    RECT frame; DwmGetWindowAttribute(h, 9, out frame, 16);
    var monitor = new MONITORINFO(); monitor.Size = Marshal.SizeOf(monitor);
    GetMonitorInfoW(MonitorFromWindow(h, 2), ref monitor);
    var origin = new POINT(); ClientToScreen(h, ref origin);
    return "{\\"style\\":" + GetWindowLongPtr(h, -16).ToInt64() + ",\\"zoomed\\":" + (IsZoomed(h) ? "true" : "false") +
      ",\\"iconic\\":" + (IsIconic(h) ? "true" : "false") + ",\\"frame\\":" + Box(frame) + ",\\"work\\":" + Box(monitor.Work) +
      ",\\"origin\\":[" + origin.X + "," + origin.Y + "]}";
  }
  public static long Hit(int x, int y) {
    var p = new POINT { X = x, Y = y };
    return SendMessageW(WindowFromPoint(p), 0x84, IntPtr.Zero, (IntPtr)((y << 16) | (x & 0xFFFF))).ToInt64();
  }
}
'@
[void][Frame]::SetProcessDPIAware()
$h = [Frame]::Find(${pid})
if ($h -eq [IntPtr]::Zero) { throw 'the window was not found' }`;

export default async function run(page, { sleep, pid }) {
  const powershell = (script) => execFileSync('powershell', ['-NoProfile', '-Command', `${NATIVE(pid)}\n${script}`], { encoding: 'utf8' }).trim();
  const info = () => JSON.parse(powershell('[Frame]::Info($h)'));
  const box = (selector) => page.eval(`(() => {
    const element = document.querySelector(${JSON.stringify(selector)});
    if (!element) return null;
    const { left, top, right, bottom, width, height } = element.getBoundingClientRect();
    return { left, top, right, bottom, width, height, x: (left + right) / 2, y: (top + bottom) / 2 };
  })()`);
  const click = async (selector) => {
    const { x, y } = await box(selector);
    await page.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    await page.send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1 });
    await page.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1 });
    await page.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 1, y: 300 });
  };
  const waitNative = async (test, label) => {
    for (let attempt = 0; attempt < 30; ++attempt) {
      const state = info();
      if (test(state)) return state;
      await sleep(200);
    }
    check(false, label);
    return info();
  };
  const html = () => page.eval('document.documentElement.className');

  // The real window, without the size the other steps emulate.
  await page.send('Emulation.clearDeviceMetricsOverride');
  await sleep(300);

  const start = info();
  check((start.style & WS_CAPTION) === 0, 'the window has no caption of its own, so Windows draws no buttons over the page');
  const kept = WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
  check((start.style & kept) === kept, 'and keeps the resizing border, the system menu, minimize and maximize');
  const frame = await page.call('window.state');
  console.log(`backdrop ${frame.backdrop}, active ${frame.active}, maximized ${frame.maximized}`);
  check((await html()).includes('solid') === !frame.backdrop, 'the page is clear only where Windows draws its material');

  const captions = await page.eval(`[...document.querySelectorAll('.caption-button')].map((b) => b.title)`);
  check(captions.join(',') === 'Minimizar,Maximizar,Cerrar', `the title bar has its three buttons (${captions.join(', ')})`);

  // Where the title bar is free, Windows takes the press as a caption; on the buttons, the page does.
  const ratio = await page.eval('devicePixelRatio');
  const toScreen = ({ x, y }) => [Math.round(start.origin[0] + x * ratio), Math.round(start.origin[1] + y * ratio)];
  const title = await box('.title-bar');
  const [freeX, freeY] = toScreen({ x: title.width / 2, y: title.height / 2 });
  const [buttonX, buttonY] = toScreen(await box('.caption-button.maximize'));
  const freeHit = Number(powershell(`[Frame]::Hit(${freeX}, ${freeY})`));
  const buttonHit = Number(powershell(`[Frame]::Hit(${buttonX}, ${buttonY})`));
  check(freeHit === HTCAPTION, `the free title bar moves the window (hit test ${freeHit})`);
  check(buttonHit !== HTCAPTION, `the maximize button stays a button (hit test ${buttonHit})`);

  const edge = await page.eval(`(() => {
    const element = document.querySelector('.resize-edge.top');
    const { top, height } = element.getBoundingClientRect();
    return { top, height, cursor: getComputedStyle(element).cursor };
  })()`);
  check(edge.top === 0 && edge.height >= 4 && edge.cursor === 'n-resize', `the top edge resizes (${JSON.stringify(edge)})`);

  // Without the button held, a resize request must do nothing, or the window would follow the pointer.
  await page.call('window.resize', { edge: 'top' });
  await sleep(400);
  const still = info();
  check(JSON.stringify(still.frame) === JSON.stringify(start.frame), 'a resize request with no button held leaves the window alone');
  check((await page.eval('1 + 1')) === 2, 'and the page still answers');

  await click('.caption-button.maximize');
  const big = await waitNative((state) => state.zoomed, 'the maximize button maximizes');
  check(JSON.stringify(big.frame) === JSON.stringify(big.work), `maximized, it fills the work area and leaves the taskbar alone (${big.frame} in ${big.work})`);
  await page.wait(`document.documentElement.classList.contains('maximized')`, 3000);
  check((await page.eval(`document.querySelector('.caption-button.maximize').title`)) === 'Restaurar', 'the button then offers to restore');
  check((await page.eval(`getComputedStyle(document.querySelector('.resize-edge.top')).display`)) === 'none', 'and the top edge does not resize');

  await click('.caption-button.maximize');
  await waitNative((state) => !state.zoomed, 'the same button restores');
  await page.wait(`!document.documentElement.classList.contains('maximized')`, 3000);
  const restored = info();
  check(JSON.stringify(restored.frame) === JSON.stringify(start.frame), 'to the same place and size');

  await click('.caption-button.minimize');
  await waitNative((state) => state.iconic, 'the minimize button minimizes');
  powershell('[void][Frame]::ShowWindow($h, 9)');
  await waitNative((state) => !state.iconic, 'and the window comes back');
  check(true, 'minimize and restore');

  // The transport in the middle of the window, whatever the sides hold.
  const folder = process.env.SILENCE_FOLDER;
  const folders = await page.call('library.folders');
  if (folder && !folders.some((item) => item.path === folder)) await page.call('library.addFolder', { path: folder });
  await page.wait(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).some((a) => a.title === ${JSON.stringify(ALBUM)}))`, 60000);
  const album = (await page.call('library.albums')).find((item) => item.title === ALBUM);
  const tracks = await page.call('library.albumTracks', { id: album.id });
  await page.call('queue.clear');
  await page.call('queue.add', { tracks: tracks.map((track) => track.id) });
  await page.wait(`document.querySelector('.bar-title').textContent === ${JSON.stringify(tracks[0].title)}`);
  await page.wait(`document.querySelector('.bar-backdrop canvas.shown') !== null`, 5000);
  check(true, 'the cover lies behind the bar');
  check((await page.eval(`document.querySelector('.format-tag').textContent`)) === 'WAV', 'the format tag names the file');

  const layout = `(() => {
    const middle = (selector) => {
      const { left, right } = document.querySelector(selector).getBoundingClientRect();
      return (left + right) / 2;
    };
    const right = document.querySelector('.bar-right');
    const detail = document.querySelector('.format-detail');
    const shown = detail.querySelector(detail.classList.contains('short') ? '.detail-short' : '.detail-long');
    return {
      half: innerWidth / 2,
      toggle: middle('.bar-toggle'),
      seek: middle('.seek'),
      fits: right.scrollWidth <= right.clientWidth + 1 && right.getBoundingClientRect().right <= innerWidth,
      clipped: shown.getBoundingClientRect().width > detail.getBoundingClientRect().width + 0.01,
      short: detail.classList.contains('short'),
      volume: getComputedStyle(document.querySelector('.bar-volume')).display !== 'none',
    };
  })()`;
  const long = 'Un título larguísimo que no cabría nunca en el lado izquierdo de la barra de reproducción';
  for (const width of [1600, 1320, 1081, 1080, 900]) {
    for (const queueOpen of [false, true]) {
      for (const title of [tracks[0].title, long]) {
        await page.size(width, 700);
        await page.eval(`import('/js/store.js').then((s) => { s.set('queueOpen', ${queueOpen}); document.querySelector('.bar-title').textContent = ${JSON.stringify(title)}; return true; })`);
        await sleep(250);
        const shown = await page.eval(layout);
        const label = `${width} px, queue ${queueOpen ? 'open' : 'closed'}, ${title === long ? 'long' : 'short'} title`;
        check(Math.abs(shown.toggle - shown.half) < 1 && Math.abs(shown.seek - shown.half) < 1,
          `the transport is centred at ${label} (${shown.toggle.toFixed(1)} of ${shown.half})`);
        check(shown.fits && !shown.clipped, `the format fits whole at ${label}${shown.short ? ', short' : ''}`);
        check(shown.volume === width >= 1000, `the volume slider ${width >= 1000 ? 'stays' : 'goes'} at ${label}`);
      }
    }
  }
  await page.eval(`import('/js/store.js').then((s) => { s.set('queueOpen', false); return true; })`);
  await page.size(1320, 860);
  await page.call('queue.clear');
}
