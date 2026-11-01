// What the interface costs while music plays: CPU of the application and all its WebView2 processes. Needs
// ARPEGIO_EXE, the path of the running executable. With no process to read there is no measure, so it stops.
import { execFileSync } from 'node:child_process';

const SECONDS = 12;
const sample = `
$ErrorActionPreference = 'Stop'
$root = (Get-Process arpegio | Where-Object { $_.Path -eq $env:ARPEGIO_EXE } | Select-Object -First 1).Id
if (-not $root) { throw 'the application process was not found' }
$all = Get-CimInstance Win32_Process -Property ProcessId,ParentProcessId
$tree = @($root); $grew = $true
while ($grew) { $grew = $false; foreach ($p in $all) { if ($tree -contains $p.ParentProcessId -and $tree -notcontains $p.ProcessId) { $tree += $p.ProcessId; $grew = $true } } }
$total = 0.0; $memory = 0; foreach ($id in $tree) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($p) { $total += $p.TotalProcessorTime.TotalSeconds; $memory += $p.WorkingSet64 } }
"$total $memory $($tree.Count)"`;

function read() {
  const [cpu, memory, processes] = execFileSync('powershell', ['-NoProfile', '-Command', sample], { encoding: 'utf8' }).trim().split(' ');
  return { cpu: Number(cpu), memory: Number(memory), processes: Number(processes) };
}

export default async function run(page, { sleep }) {
  await page.size(1320, 860);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; return ${test}; })`, timeout);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);
  const cores = Number(execFileSync('powershell', ['-NoProfile', '-Command', '[Environment]::ProcessorCount'], { encoding: 'utf8' }));

  const measure = async (label) => {
    await sleep(2000);
    const before = read();
    const started = Date.now();
    await sleep(SECONDS * 1000);
    const after = read();
    const elapsed = (Date.now() - started) / 1000;
    const percent = ((after.cpu - before.cpu) / elapsed) * 100;
    console.log(`${label}: ${percent.toFixed(1)} % of one core (${(percent / cores).toFixed(2)} % of the machine), ` +
      `${Math.round(after.memory / 1048576)} MB in ${after.processes} processes`);
  };

  await page.go({ view: 'albums' });
  await measure('stopped, albums view');

  const albums = await page.call('library.albums');
  const album = albums.find((item) => item.title === 'Pruebas de silencio');
  const tracks = await page.call('library.albumTracks', { id: album.id });
  await page.call('queue.play', { tracks: tracks.map((track) => track.id), index: 2 });
  await until(`status.plan && status.position > 0.5`, 20000);
  const status = await page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  console.log('playing through', status.plan.path, status.plan.rate);

  await page.go({ view: 'albums' });
  await measure('playing, albums view');
  await page.call('player.setVolume', { db: -12 });
  await measure('playing with the volume down, albums view');
  await page.call('player.setVolume', { db: 0 });
  await page.go({ view: 'now' });
  await measure('playing, now playing view');

  const after = await page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  console.log(`underruns ${after.underruns}, late ${after.late}`);
  await page.call('queue.clear');
}
