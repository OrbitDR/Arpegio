// The bar along the bottom: what is playing, the transport, and how it is reaching the device.
import { call } from './api.js';
import { setFavorite } from './actions.js';
import { fill, h, iconButton, setIcon } from './dom.js';
import { reportError } from './feedback.js';
import { decibels, formatDetail, formatTag, time } from './format.js';
import { ART_HOST, cover } from './parts.js';
import { currentRoute, go } from './router.js';
import { processingLines } from './signal.js';
import { currentTrack, selectedDevice, set, state, watch } from './store.js';

const SEEK_HOLD_MS = 900;  // lets the engine land before the bar follows it again
const REPEAT_ORDER = ['off', 'queue', 'track'];
const REPEAT_LABELS = { off: 'Repetir: desactivado', queue: 'Repetir: cola', track: 'Repetir: pista' };
const DEVICE_HELD = 'Liberar el dispositivo. Se detiene la música y Reproducir continúa desde aquí';
const DEVICE_FREE = 'Dispositivo libre. Se ocupa al reproducir';
const DEVICE_SHARED = 'Modo compartido: el dispositivo sigue disponible para los demás programas';
const VOLUME_FIXED = 'Con DSD nativo o DoP no hay volumen por software: usa el del DAC';
const VOLUME_WHEEL_DB = 1;
const QUALITY_HINT = 'Ver la ruta de la señal';

// The cover is drawn this small and stretched over the bar, which blurs it once instead of on every frame.
const BACKDROP_WIDTH = 64;
const BACKDROP_HEIGHT = 16;
const BACKDROP_FADE_MS = 700;
// How bright the cover may stay behind the text, as relative luminance, and the least it is darkened.
const BACKDROP_CEILING = 0.07;
const BACKDROP_MIN_SHADE = 0.25;
const BACKDROP_UNREADABLE_SHADE = 0.65;

const send = (method, params) => call(method, params).catch(reportError);

function luminance(red, green, blue) {
  const linear = (channel) => {
    const value = channel / 255;
    return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue);
}

// How much black to lay over the blurred cover so that its brightest part stays under the ceiling. The canvas
// blends in sRGB, where darkening by a share s lowers luminance by about (1 - s) ^ 2.2.
function shadeFor(context) {
  try {
    const { data } = context.getImageData(0, 0, BACKDROP_WIDTH, BACKDROP_HEIGHT);
    let brightest = 0;
    for (let i = 0; i < data.length; i += 4) brightest = Math.max(brightest, luminance(data[i], data[i + 1], data[i + 2]));
    if (brightest <= BACKDROP_CEILING) return BACKDROP_MIN_SHADE;
    return Math.max(BACKDROP_MIN_SHADE, 1 - (BACKDROP_CEILING / brightest) ** (1 / 2.2));
  } catch {
    return BACKDROP_UNREADABLE_SHADE;
  }
}

// The cover of what plays, blurred behind the bar, fading from one track's to the next.
function createBackdrop() {
  const layer = h('div.bar-backdrop', { ariaHidden: 'true' });
  let shown = null;
  let ticket = 0;

  const fadeOut = () => {
    for (const old of layer.querySelectorAll('canvas.shown')) {
      old.classList.remove('shown');
      setTimeout(() => old.remove(), BACKDROP_FADE_MS);
    }
  };

  function show(art) {
    if (art === shown) return;
    shown = art;
    const mine = ++ticket;
    if (!art) {
      fadeOut();
      return;
    }
    const image = new Image();
    image.crossOrigin = 'anonymous';  // the art host allows it, so the canvas can read how bright it is
    image.onload = () => {
      if (mine !== ticket) return;
      const canvas = h('canvas', { width: BACKDROP_WIDTH, height: BACKDROP_HEIGHT });
      const context = canvas.getContext('2d');
      context.imageSmoothingQuality = 'high';
      context.filter = 'blur(1.5px) saturate(1.4)';
      // Drawn past the edges, so the blur does not fade them out.
      context.drawImage(image, -8, -4, BACKDROP_WIDTH + 16, BACKDROP_HEIGHT + 8);
      context.filter = 'none';
      context.fillStyle = `rgb(0 0 0 / ${shadeFor(context).toFixed(3)})`;
      context.fillRect(0, 0, BACKDROP_WIDTH, BACKDROP_HEIGHT);
      fadeOut();
      layer.append(canvas);
      canvas.getBoundingClientRect();  // so the fade starts from transparent
      canvas.classList.add('shown');
    };
    image.onerror = () => {
      if (mine === ticket) fadeOut();
    };
    image.src = `${ART_HOST}/thumb/${art}`;
  }

  return { layer, show };
}

function qualityHint(track, status) {
  if (!track || !status.plan) return QUALITY_HINT;
  return [...processingLines(status.plan, track, status), '', QUALITY_HINT].join('\n');
}

export function createPlayerBar() {
  let shownTrack = undefined;
  let shownQuality = '';
  let dragging = false;
  let draggingVolume = false;
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

  const quality = h('button.bar-quality', { type: 'button', title: QUALITY_HINT, onclick: () => go({ view: 'now' }) });
  const backdrop = createBackdrop();
  const device = iconButton('device', DEVICE_FREE, () => send('devices.release'), { cls: 'bar-device' });
  const queueButton = iconButton('playlist', 'Cola de reproducción', () => set('queueOpen', !state.queueOpen));

  const mute = iconButton('volume', 'Silenciar', () => send('player.setMuted', { muted: !state.status.muted }), { cls: 'bar-mute' });
  const volume = h('input.slider.bar-volume', {
    type: 'range',
    min: -60,
    max: 0,
    step: 0.5,
    value: 0,
    ariaLabel: 'Volumen',
    onpointerdown: () => {
      draggingVolume = true;
      addEventListener('pointerup', () => { draggingVolume = false; }, { once: true });
    },
    oninput: () => {
      paintVolume();
      send('player.setVolume', { db: Number(volume.value) });
    },
    onwheel: (event) => {
      event.preventDefault();
      if (volume.disabled) return;
      const db = Number(volume.value) + (event.deltaY < 0 ? VOLUME_WHEEL_DB : -VOLUME_WHEEL_DB);
      volume.value = db;
      paintVolume();
      send('player.setVolume', { db: Number(volume.value) });
    },
  });

  // The full format when it fits and the short one when not, measured in fractions of a pixel: whole ones miss
  // the sliver that already brings the ellipsis. The column's width comes from the grid, not from what is in
  // it, so changing the text cannot resize what is being watched.
  const fitDetail = () => {
    const detail = quality.querySelector('.format-detail');
    if (!detail) return;
    detail.classList.remove('short');
    const needed = detail.querySelector('.detail-long').getBoundingClientRect().width;
    detail.classList.toggle('short', needed > detail.getBoundingClientRect().width + 0.01);
  };

  const right = h('div.bar-right', quality, device, mute, volume, queueButton);
  new ResizeObserver(fitDetail).observe(right);

  const bar = h('footer.player-bar',
    backdrop.layer,
    h('div.bar-left', art, h('div.bar-text', title, artist), favorite),
    h('div.bar-center',
      h('div.bar-buttons', shuffle, previous, toggle, next, repeat),
      h('div.bar-seek', elapsed, seek, remaining)),
    right);

  function paintSeek() {
    const max = Number(seek.max) || 1;
    seek.style.setProperty('--fill', `${(Number(seek.value) / max) * 100}%`);
  }

  function paintVolume() {
    const floor = Number(volume.min);
    volume.style.setProperty('--fill', `${((Number(volume.value) - floor) / -floor) * 100}%`);
  }

  function updateVolume(status) {
    const fixed = status.plan !== null && !status.plan.acceptsGain;
    mute.disabled = fixed;
    volume.disabled = fixed;
    if (!draggingVolume) {
      volume.value = status.volumeDb;
      paintVolume();
    }
    const silent = status.muted || status.volumeDb <= status.volumeFloorDb;
    setIcon(mute, silent ? 'volumeOff' : status.volumeDb < -20 ? 'volumeLow' : 'volume');
    mute.classList.toggle('on', status.muted);
    const level = fixed ? VOLUME_FIXED : silent ? 'Volumen: silencio' : `Volumen: ${decibels(status.volumeDb)}`;
    volume.title = level;
    mute.title = fixed ? VOLUME_FIXED : status.muted ? 'Quitar el silencio' : 'Silenciar';
    mute.ariaLabel = mute.title;
  }

  function update() {
    const status = state.status;
    const track = currentTrack();
    if (!status) return;

    const trackKey = track ? track.id : null;
    if (trackKey !== shownTrack) {
      shownTrack = trackKey;
      art.replaceChildren(cover(track?.art ?? 0));
      backdrop.show(track?.art ?? 0);
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

    const signal = status.plan ? (status.bitPerfect ? 'good' : 'warn') : '';
    const qualityKey = track ? `${formatTag(track)}|${formatDetail(track)[0]}|${signal}` : '';
    if (qualityKey !== shownQuality) {
      shownQuality = qualityKey;
      quality.replaceChildren();
      if (track) {
        const [detail, shortDetail] = formatDetail(track);
        fill(quality,
          h('span.bar-format',
            h('span.format-tag', { class: track.mqa ? 'mqa' : '' }, formatTag(track)),
            h('span.format-detail', h('span.detail-long', detail), h('span.detail-short', shortDetail))),
          signal ? h('span.bar-status', { class: signal }, status.bitPerfect ? 'Bit-perfect' : 'Procesado') : null);
        fitDetail();
      }
    }
    const hint = qualityHint(track, status);
    if (quality.title !== hint) quality.title = hint;

    device.disabled = !status.deviceHeld;
    device.classList.toggle('on', status.deviceHeld);
    device.title = status.deviceHeld ? DEVICE_HELD : selectedDevice()?.kind === 'shared' ? DEVICE_SHARED : DEVICE_FREE;
    device.ariaLabel = device.title;
    updateVolume(status);
  }

  watch('status', update);
  watch('devices', update);
  watch('queue', () => {
    shownTrack = undefined;
    update();
  });
  watch('queueOpen', (open) => queueButton.classList.toggle('on', open));
  return bar;
}
