// Every view that is mostly a list of tracks: all of them, favourites, playlists and search results.
import { call } from '../api.js';
import { playTracks } from '../actions.js';
import { fill, h, icon } from '../dom.js';
import { reportError } from '../feedback.js';
import { compareText, count, longTime } from '../format.js';
import { albumCard, emptyState, pageHeader, trackRow } from '../parts.js';
import { go } from '../router.js';
import { cached, state } from '../store.js';
import { VirtualList, rows } from '../virtual.js';
import { loadAlbums } from './albums.js';

const ROW_HEIGHT = 48;

const COLUMNS = [
  ['title', 'Título', (a, b) => compareText(a.title, b.title)],
  ['artist', 'Artista', (a, b) => compareText(a.artist, b.artist)],
  ['album', 'Álbum', (a, b) => compareText(a.album, b.album) || a.disc - b.disc || a.track - b.track],
  ['year', 'Año', (a, b) => a.year - b.year],
  ['format', 'Formato', (a, b) => a.dsd - b.dsd || a.rate - b.rate || a.bits - b.bits],
  ['seconds', 'Duración', (a, b) => a.seconds - b.seconds],
];

const sortState = { key: 'artist', descending: false };

function comparator() {
  const [, , compare] = COLUMNS.find(([key]) => key === sortState.key);
  const byAlbum = COLUMNS[2][2];
  const ordered = (a, b) => compare(a, b) || byAlbum(a, b);
  return sortState.descending ? (a, b) => ordered(b, a) : ordered;
}

function trackList(tracks, options = {}) {
  return new VirtualList({
    scroller: document.getElementById('scroller'),
    count: tracks.length,
    layout: rows(ROW_HEIGHT),
    render: (index) => trackRow(tracks, index, { columns: 'all', number: index + 1, ...options.row?.(index) }),
    cls: 'track-list',
  });
}

function playAll(tracks) {
  return h('button.button.primary', { type: 'button', onclick: () => playTracks(tracks) }, icon('play', 20), 'Reproducir');
}

export async function tracksView(page) {
  const all = await cached('tracks', () => call('library.tracks'));
  if (!all.length) {
    page.append(emptyState('list', 'Todavía no hay pistas'));
    return null;
  }

  const tracks = [...all].sort(comparator());
  const list = trackList(tracks);
  const header = h('div.table-head.tracks-all', { role: 'row' });

  function drawHeader() {
    const button = (key, label) => h('button.sort', {
      type: 'button',
      class: sortState.key === key ? 'active' : '',
      onclick: () => {
        sortState.descending = sortState.key === key && !sortState.descending;
        sortState.key = key;
        tracks.sort(comparator());
        drawHeader();
        list.redraw();
      },
    }, label, sortState.key === key ? (sortState.descending ? ' ↓' : ' ↑') : '');
    header.replaceChildren(
      h('div'),
      ...COLUMNS.map(([key, label]) => h('div', { class: key === 'year' ? 'numeric' : '' }, button(key, label))),
      h('div'));
  }
  drawHeader();

  page.append(pageHeader('Pistas', count(tracks.length, 'pista', 'pistas'), playAll(tracks)), header, list.element);
  return () => list.destroy();
}

export async function favoritesView(page) {
  const tracks = await call('library.favorites');
  if (!tracks.length) {
    page.append(emptyState('favorite', 'Aún no tienes favoritos', 'Marca una pista con el corazón para encontrarla aquí.'));
    return null;
  }
  const list = trackList(tracks);
  page.append(pageHeader('Favoritos', count(tracks.length, 'pista', 'pistas'), playAll(tracks)), list.element);
  return () => list.destroy();
}

export async function playlistView(page, { id }) {
  const tracks = await call('playlists.tracks', { id });
  const playlist = state.playlists.find((item) => item.id === id);
  if (!playlist) {
    page.append(emptyState('playlist', 'Esta lista ya no existe'));
    return null;
  }
  const total = tracks.reduce((sum, track) => sum + track.seconds, 0);
  const subtitle = tracks.length ? `${count(tracks.length, 'pista', 'pistas')} · ${longTime(total)}` : '';
  if (!tracks.length) {
    page.append(pageHeader(playlist.name, subtitle),
      emptyState('playlist', 'Esta lista está vacía', 'Abre el menú de cualquier pista o álbum y elige «Añadir a una lista».'));
    return null;
  }

  const list = trackList(tracks, {
    row: (index) => ({
      menu: {
        extra: [null, {
          label: 'Quitar de esta lista',
          icon: 'close',
          action: async () => {
            await call('playlists.remove', { id, index }).catch(reportError);
            go({ view: 'playlist', id }, { replace: true });
          },
        }],
      },
    }),
  });
  page.append(pageHeader(playlist.name, subtitle, playAll(tracks)), list.element);
  return () => list.destroy();
}

export async function searchView(page, { text }) {
  const [tracks, albums] = await Promise.all([call('library.search', { text }), loadAlbums()]);
  if (!tracks.length) {
    page.append(pageHeader(`«${text}»`), emptyState('search', 'Sin resultados', 'Prueba con otra palabra o con menos letras.'));
    return null;
  }

  // The album of every result, best match first, without repeating any.
  const known = new Map(albums.map((album) => [album.id, album]));
  const found = [...new Set(tracks.map((track) => track.albumId))].map((id) => known.get(id)).filter(Boolean);
  const artists = [...new Map(tracks.map((track) => [track.artistId, track.artist]))];

  const cards = found.slice(0, 12);
  const list = trackList(tracks);
  fill(page,
    pageHeader(`«${text}»`, count(tracks.length, 'pista encontrada', 'pistas encontradas')),
    artists.length <= 12
      ? h('div.pill-row', artists.map(([id, name]) => h('button.pill', {
        type: 'button',
        onclick: () => go({ view: 'artist', id }),
      }, icon('person', 16), name)))
      : null,
    cards.length ? h('h2.section-title', 'Álbumes') : null,
    cards.length ? h('div.card-row', cards.map(albumCard)) : null,
    h('h2.section-title', 'Pistas'),
    list.element);
  return () => list.destroy();
}
