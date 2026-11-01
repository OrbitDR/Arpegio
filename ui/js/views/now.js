// What is playing, with the cover large and the route the sound takes beside it.
import { h } from '../dom.js';
import { formatLine } from '../format.js';
import { ART_HOST, cover, emptyState } from '../parts.js';
import { go } from '../router.js';
import { currentTrack, state, watch } from '../store.js';
import { signalPath } from '../signal.js';

const REDRAW_KEYS = ['state', 'plan', 'bitPerfect', 'volumeDb', 'muted', 'trackGainDb', 'windowsGainDb', 'underruns', 'late', 'clipped'];

function signature(status) {
  const buffer = Math.round((status?.buffer ?? 0) * 20);
  return JSON.stringify([buffer, ...REDRAW_KEYS.map((key) => status?.[key])]);
}

export function nowView(page) {
  const backdrop = h('div.now-backdrop');
  const main = h('div.now-main');
  const details = h('div.now-details');
  page.classList.add('now-page');
  page.append(backdrop, main, details);

  let shownTrack;
  let shownSignal = '';

  function drawTrack() {
    const track = currentTrack();
    shownTrack = track?.id;
    backdrop.replaceChildren();
    if (!track) {
      main.replaceChildren(emptyState('note', 'No hay nada en la cola', 'Elige un álbum para empezar.'));
      return;
    }
    if (track.art) backdrop.append(h('img', { src: `${ART_HOST}/thumb/${track.art}`, alt: '' }));
    main.replaceChildren(
      cover(track.art, { full: true, cls: 'now-cover' }),
      h('div.now-text',
        h('h1', track.title),
        h('button.now-link', { type: 'button', onclick: () => go({ view: 'artist', id: track.artistId }) }, track.artist),
        h('button.now-link.dim', { type: 'button', onclick: () => go({ view: 'album', id: track.albumId }) },
          track.year ? `${track.album} · ${track.year}` : track.album),
        h('p.now-format', formatLine(track))));
  }

  function drawSignal() {
    shownSignal = signature(state.status);
    details.replaceChildren(signalPath(currentTrack(), state.status));
  }

  drawTrack();
  drawSignal();

  const stops = [
    watch('status', (status) => {
      if (currentTrack()?.id !== shownTrack) drawTrack();
      if (signature(status) !== shownSignal) drawSignal();
    }),
    watch('queue', drawTrack),
    watch('devices', drawSignal),
  ];
  return () => stops.forEach((stop) => stop());
}
