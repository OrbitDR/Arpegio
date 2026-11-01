// The bar along the bottom: what is playing, the transport, and how it is reaching the device.
import { call } from './api.js';
import { setFavorite } from './actions.js';
import { h, icon, iconButton, setIcon } from './dom.js';
import { reportError } from './feedback.js';
import { shortFormat, time } from './format.js';
import { cover } from './parts.js';
import { currentRoute, go } from './router.js';
import { currentTrack, set, state, watch } from './store.js';

const SEEK_HOLD_MS = 900;  // lets the engine land before the bar follows it again
const REPEAT_ORDER = ['off', 'queue', 'track'];
const REPEAT_LABELS = { off: 'Repetir: desactivado', queue: 'Repetir: cola', track: 'Repetir: pista' };

const send = (method, params) => call(method, params).catch(reportError);

export function createPlayerBar() {
  let shownTrack = undefined;
  let shownQuality = '';
  let dragging = false;
  let holdUntil = 0;

  const art = h('button.bar-art', {
    type: 'button',
    title: 'Ver reproducción',
    onclick: () => {
      if (currentRoute()?.view === 'now') history.back();
      else go({ view: 'now' });
    },
  });
  const title = h('div.bar-title');
  const artist = h('div.bar-artist');
  const favorite = iconButton('favoriteOutline', 'Favorito', async () => {
    const track = currentTrack();
    if (!track) return;
    await setFavorite(track, !track.favorite);
    update();
  }, { size: 18, cls: 'bar-favorite' });

  const shuffle = iconButton('shuffle', 'Mezclar la cola', () => send('queue.shuffle'), { size: 18 });
  const previous = iconButton('previous', 'Anterior', () => send('player.previous'), { size: 24 });
  const toggle = iconButton('play', 'Reproducir', () => send('player.toggle'), { size: 30, cls: 'bar-toggle' });
  const next = iconButton('next', 'Siguiente', () => send('player.next'), { size: 24 });
  const repeat = iconButton('repeat', 'Repetir', () => {
    const mode = REPEAT_ORDER[(REPEAT_ORDER.indexOf(state.status.repeat) + 1) % REPEAT_ORDER.length];
    send('player.setRepeat', { mode });
  }, { size: 18 });

  const elapsed = h('span.bar-time');
  const remaining = h('span.bar-time.right');
  const seek = h('input.seek', {
    type: 'range',
    min: 0,
    max: 1,
    step: 0.1,
    value: 0,
    ariaLabel: 'Posición',
    onpointerdown: () => {
      dragging = true;
      // Not in onchange, which stays silent when the press leaves the value where it was.
      addEventListener('pointerup', () => { dragging = false; }, { once: true });
    },
    oninput: () => {
      paintSeek();
      elapsed.textContent = time(Number(seek.value));
    },
    onchange: () => {
      holdUntil = performance.now() + SEEK_HOLD_MS;
      send('player.seek', { seconds: Number(seek.value) });
    },
  });

  const quality = h('button.bar-quality', { type: 'button', title: 'Ver la ruta de la señal', onclick: () => go({ view: 'now' }) });
  const queueButton = iconButton('playlist', 'Cola de reproducción', () => set('queueOpen', !state.queueOpen));

  const bar = h('footer.player-bar',
    h('div.bar-left', art, h('div.bar-text', title, artist), favorite),
    h('div.bar-center',
      h('div.bar-buttons', shuffle, previous, toggle, next, repeat),
      h('div.bar-seek', elapsed, seek, remaining)),
    h('div.bar-right', quality, queueButton));

  function paintSeek() {
    const max = Number(seek.max) || 1;
    seek.style.setProperty('--fill', `${(Number(seek.value) / max) * 100}%`);
  }

  function update() {
    const status = state.status;
    const track = currentTrack();
    if (!status) return;

    const trackKey = track ? track.id : null;
    if (trackKey !== shownTrack) {
      shownTrack = trackKey;
      art.replaceChildren(cover(track?.art ?? 0));
      title.textContent = track?.title ?? 'Nada en la cola';
      artist.textContent = track ? track.artist : '';
    }
    favorite.disabled = !track;
    setIcon(favorite, track?.favorite ? 'favorite' : 'favoriteOutline', 18);
    favorite.classList.toggle('on', Boolean(track?.favorite));

    const playing = status.state === 'playing';
    setIcon(toggle, playing ? 'pause' : 'play', 30);
    toggle.title = playing ? 'Pausar' : 'Reproducir';
    toggle.ariaLabel = toggle.title;
    bar.classList.toggle('idle', !track);

    setIcon(repeat, status.repeat === 'track' ? 'repeatOne' : 'repeat', 18);
    repeat.classList.toggle('on', status.repeat !== 'off');
    repeat.title = REPEAT_LABELS[status.repeat];

    if (!dragging && performance.now() >= holdUntil) {
      seek.max = Math.max(status.duration, 0.1);
      seek.value = status.position;
      elapsed.textContent = time(status.position);
      paintSeek();
    }
    remaining.textContent = time(status.duration);
    seek.disabled = !track;

    const qualityKey = `${track ? shortFormat(track) : ''}|${status.plan ? status.bitPerfect : ''}`;
    if (qualityKey !== shownQuality) {
      shownQuality = qualityKey;
      quality.replaceChildren();
      if (track) {
        quality.append(h('span.bar-format', shortFormat(track)));
        if (status.plan) {
          quality.append(status.bitPerfect
            ? h('span.badge.good', icon('check', 14), 'Bit-perfect')
            : h('span.badge.warn', 'Procesado'));
        }
      }
    }
  }

  watch('status', update);
  watch('queue', () => {
    shownTrack = undefined;
    update();
  });
  watch('queueOpen', (open) => queueButton.classList.toggle('on', open));
  return bar;
}
