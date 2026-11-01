// The media session Windows sees, read and driven from outside the application, through the same API as the
// flyout and the lock screen. Plays digital silence through the selected output. Needs SILENCE_FOLDER.
import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT = join(dirname(fileURLToPath(import.meta.url)), 'media_session.ps1');
const APP = 'Arpegio';
const ALBUM = 'Pruebas de silencio';

const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

const session = (action = 'read', seconds = 0) => JSON.parse(execFileSync('powershell',
  ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', SCRIPT, '-App', APP, '-Action', action, '-Seconds', String(seconds)],
  { encoding: 'utf8' }).trim());

export default async function run(page, { sleep }) {
  const status = () => page.eval(`import('/js/store.js').then((s) => s.state.status)`);
  const until = (test, timeout = 15000) => page.wait(`import('/js/store.js').then((s) => { const status = s.state.status; return ${test}; })`, timeout);

  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);
  await page.call('library.addFolder', { path: process.env.SILENCE_FOLDER });
  await page.wait(`import('/js/api.js').then(async (api) => (await api.call('library.albums')).some((a) => a.title === ${JSON.stringify(ALBUM)}))`, 60000);
  const album = (await page.call('library.albums')).find((item) => item.title === ALBUM);
  const tracks = await page.call('library.albumTracks', { id: album.id });

  await page.call('queue.play', { tracks: tracks.slice(0, 3).map((track) => track.id), index: 0 });
  await until(`status.plan && status.position > 0.5`, 20000);
  await sleep(1500);

  let seen = session();
  console.log(JSON.stringify(seen));
  check(seen.found, `Windows lists a session for the application (${seen.app ?? seen.apps})`);
  if (!seen.found) return;
  check(seen.title === tracks[0].title, `with the title (${seen.title})`);
  check(seen.artist === tracks[0].artist, `the artist (${seen.artist})`);
  check(seen.album === ALBUM, `the album (${seen.album})`);
  check(seen.status === 'Playing', `and the state (${seen.status})`);
  check(tracks[0].art !== 0, 'the silent album has a cover');
  check(seen.thumbnail, `and Windows gets it (${seen.thumbnail})`);
  check(Math.abs(seen.end - tracks[0].seconds) < 1, `the length (${seen.end} s)`);
  check(seen.next && seen.previous, 'next and previous are on offer');
  check(seen.seekable, 'and so is seeking');

  seen = session('pause');
  await until(`status.state === 'paused'`, 3000);
  check(seen.accepted, 'pause from outside reaches the application');
  await sleep(500);
  check(session().status === 'Paused', 'Windows shows it paused');

  session('play');
  await until(`status.state === 'playing'`, 3000);
  check(true, 'play from outside too');

  session('next');
  await until(`status.index === 1 && status.position > 0.2`, 5000);
  await sleep(500);
  check(session().title === tracks[1].title, 'next changes what Windows shows');

  session('previous');
  await sleep(700);
  session('previous');
  await until(`status.index === 0`, 5000);
  check(true, 'previous goes back');

  session('seek', 20);
  await until(`status.position >= 20 && status.position < 26`, 5000);
  await sleep(500);
  const moved = session();
  check(moved.position >= 19 && moved.position < 26, `a seek from outside lands, and Windows follows it (${moved.position} s)`);

  // The position the application gives must stay close to the one Windows extrapolates.
  await sleep(6000);
  const later = session();
  const { position } = await status();
  check(Math.abs(later.position - position) < 7, `the clock stays with the music (${later.position} against ${position.toFixed(1)} s)`);

  // At the last track there is nowhere to go.
  await page.call('player.jump', { index: 2 });
  await until(`status.index === 2`, 5000);
  await sleep(600);
  check(!session().next, 'next stops being offered on the last track');
  await page.call('player.setRepeat', { mode: 'queue' });
  await sleep(600);
  check(session().next, 'unless the queue repeats');
  await page.call('player.setRepeat', { mode: 'off' });

  await page.call('queue.clear');
  await sleep(800);
  check(!session().found, 'an empty queue closes the session');
}
