// Puts the page together.
import { call, listen } from './api.js';
import { h, icon, iconButton } from './dom.js';
import { initFeedback, reportError, toast } from './feedback.js';
import { createPlayerBar } from './playerbar.js';
import { createQueuePanel } from './queuepanel.js';
import { currentRoute, defineView, go, onRoute, refresh, setOutlet, start } from './router.js';
import { createSidebar } from './sidebar.js';
import { connect, forgetCatalogue, set, state, watch } from './store.js';
import { albumView, albumsView } from './views/albums.js';
import { artistView, artistsView } from './views/artists.js';
import { foldersView } from './views/folders.js';
import { nowView } from './views/now.js';
import { settingsView } from './views/settings.js';
import { favoritesView, playlistView, searchView, tracksView } from './views/tracks.js';

const SEARCH_DELAY_MS = 180;
const SCAN_REFRESH_MS = 4000;
const SEEK_STEP_SECONDS = 10;

const ENGINE_MESSAGES = [
  [/cannot take this DSD rate natively/, 'Esta salida no acepta esta tasa DSD de forma nativa. Cambia el modo DSD en Ajustes.'],
  [/cannot carry this DSD rate as DoP/, 'Esta salida no puede llevar esta tasa DSD por DoP. Cambia el modo DSD en Ajustes.'],
  [/no PCM rate available/, 'El dispositivo no tiene ninguna tasa PCM para convertir este DSD.'],
  [/audio device not found/, 'El dispositivo de salida no está conectado.'],
];

function engineMessage(text) {
  return ENGINE_MESSAGES.find(([pattern]) => pattern.test(text))?.[1] ?? `No se pudo reproducir: ${text}`;
}

function createTopBar() {
  let timer = 0;
  const input = h('input.search-input', {
    id: 'search',
    type: 'search',
    placeholder: 'Buscar en la biblioteca',
    spellcheck: false,
    autocomplete: 'off',
    ariaLabel: 'Buscar en la biblioteca',
    oninput: () => {
      clearTimeout(timer);
      timer = setTimeout(() => {
        const text = input.value.trim();
        const searching = currentRoute()?.view === 'search';
        if (text) go({ view: 'search', text }, { replace: searching });
        else if (searching) history.back();
      }, SEARCH_DELAY_MS);
    },
  });
  onRoute((route) => {
    if (route.view !== 'search' && document.activeElement !== input) input.value = '';
  });
  return h('div.top-bar',
    iconButton('back', 'Atrás', () => history.back()),
    iconButton('forward', 'Adelante', () => history.forward()),
    h('label.search', icon('search', 18), input));
}

function bindKeys() {
  document.addEventListener('keydown', (event) => {
    const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName) && event.target.type !== 'range';
    if ((event.ctrlKey && event.key.toLowerCase() === 'f') || (event.ctrlKey && event.key.toLowerCase() === 'k')) {
      event.preventDefault();
      document.getElementById('search').focus();
    } else if (typing) {
      if (event.key === 'Escape') event.target.blur();
    } else if (event.key === ' ' && event.target.tagName !== 'BUTTON') {
      event.preventDefault();
      call('player.toggle').catch(reportError);
    } else if (event.ctrlKey && event.key === 'ArrowRight') {
      call('player.next').catch(reportError);
    } else if (event.ctrlKey && event.key === 'ArrowLeft') {
      call('player.previous').catch(reportError);
    } else if (event.shiftKey && (event.key === 'ArrowRight' || event.key === 'ArrowLeft')) {
      const step = event.key === 'ArrowRight' ? SEEK_STEP_SECONDS : -SEEK_STEP_SECONDS;
      call('player.seek', { seconds: Math.max(0, (state.status?.position ?? 0) + step) }).catch(reportError);
    } else if (event.altKey && event.key === 'ArrowLeft') {
      history.back();
    } else if (event.altKey && event.key === 'ArrowRight') {
      history.forward();
    }
  });
  document.addEventListener('contextmenu', (event) => {
    if (!/^(INPUT|TEXTAREA)$/.test(event.target.tagName)) event.preventDefault();
  });
}

// Marks the row of the track being heard in whatever view is open.
function followPlayback() {
  let shown = null;
  watch('status', (status) => {
    if (status.trackId === shown) return;
    shown = status.trackId;
    for (const row of document.querySelectorAll('.track-row')) {
      row.classList.toggle('current', Number(row.dataset.trackId) === shown);
    }
  });

  let lastError = '';
  watch('status', (status) => {
    if (status.error && status.error !== lastError) toast(engineMessage(status.error), 'error');
    lastError = status.error;
  });
}

// While a first scan runs, the views fill up every few seconds instead of only at the end.
function followLibrary() {
  let lastRefresh = 0;
  let wasScanning = false;
  watch('catalogue', refresh);
  watch('library', (library) => {
    const startedNow = library.scanning && !wasScanning;
    wasScanning = library.scanning;
    if (startedNow && currentRoute()?.view === 'albums') refresh();
    if (!library.scanning || !library.done) return;
    forgetCatalogue();
    const now = performance.now();
    const view = currentRoute()?.view;
    if (now - lastRefresh > SCAN_REFRESH_MS && view === 'albums') {
      lastRefresh = now;
      refresh();
    }
  });
}

async function main() {
  const root = document.getElementById('app');
  const content = h('div.content');
  const scroller = h('main.scroller', { id: 'scroller' }, content);
  setOutlet(content, scroller);

  defineView('albums', albumsView);
  defineView('album', albumView);
  defineView('artists', artistsView);
  defineView('artist', artistView);
  defineView('tracks', tracksView);
  defineView('folders', foldersView);
  defineView('favorites', favoritesView);
  defineView('playlist', playlistView);
  defineView('search', searchView);
  defineView('now', nowView);
  defineView('settings', settingsView);

  root.append(
    createSidebar(),
    h('div.main-column', createTopBar(), scroller),
    createQueuePanel(),
    createPlayerBar());
  initFeedback(document.body);
  bindKeys();
  followPlayback();
  followLibrary();

  listen('error', ({ message }) => toast(message, 'error'));
  set('queueOpen', false);
  await connect();
  start({ view: 'albums' });
}

main().catch((error) => {
  document.body.textContent = `No se pudo iniciar la interfaz: ${error.message}`;
});
