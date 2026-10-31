// The album grid and the page of one album.
import { call } from '../api.js';
import { addToQueue, albumMenu, playTracks } from '../actions.js';
import { h, icon } from '../dom.js';
import { reportError } from '../feedback.js';
import { compareText, count, formatLine, longTime, qualityChip } from '../format.js';
import { albumCard, cardLayout, cover, emptyState, pageHeader, select, trackRow } from '../parts.js';
import { go } from '../router.js';
import { cached, state } from '../store.js';
import { VirtualList } from '../virtual.js';

const SORTS = {
  title: ['Título', (a, b) => compareText(a.title, b.title)],
  artist: ['Artista', (a, b) => compareText(a.artist, b.artist) || a.year - b.year || compareText(a.title, b.title)],
  year: ['Año', (a, b) => b.year - a.year || compareText(a.title, b.title)],
  added: ['Añadido', (a, b) => b.added - a.added || compareText(a.title, b.title)],
};

let sortKey = 'artist';

export function loadAlbums() {
  return cached('albums', () => call('library.albums'));
}

function noLibrary() {
  const addFolder = () => call('library.pickFolder').catch(reportError);
  return emptyState('album', 'Tu biblioteca está vacía',
    'Elige la carpeta donde guardas tu música. Arpegio solo lee los archivos, nunca los modifica.',
    h('button.button.primary', { type: 'button', onclick: addFolder }, 'Añadir carpeta'));
}

export async function albumsView(page) {
  const albums = await loadAlbums();
  const scroller = document.getElementById('scroller');

  if (!albums.length) {
    if (state.library?.scanning) page.append(emptyState('album', 'Leyendo tu música…', 'Los álbumes aparecerán aquí en cuanto termine.'));
    else page.append(noLibrary());
    return null;
  }

  const sorted = [...albums].sort(SORTS[sortKey][1]);
  const list = new VirtualList({
    scroller,
    count: sorted.length,
    layout: cardLayout(),
    render: (index) => albumCard(sorted[index]),
  });

  page.append(pageHeader('Álbumes', count(albums.length, 'álbum', 'álbumes'),
    select('Ordenar por', sortKey, Object.entries(SORTS).map(([key, [label]]) => [key, label]), (value) => {
      sortKey = value;
      sorted.sort(SORTS[sortKey][1]);
      list.redraw();
      scroller.scrollTop = 0;
    })), list.element);
  return () => list.destroy();
}

export async function albumView(page, { id }) {
  const [albums, tracks] = await Promise.all([loadAlbums(), call('library.albumTracks', { id })]);
  const album = albums.find((item) => item.id === id);
  if (!album || !tracks.length) {
    page.append(emptyState('album', 'Este álbum ya no está en la biblioteca'));
    return null;
  }

  const discs = new Set(tracks.map((track) => track.disc || 1));
  const multiDisc = discs.size > 1;
  const credited = tracks.find((track) => track.artist === album.artist);
  const chip = qualityChip(album);
  const shuffled = () => [...tracks].sort(() => Math.random() - 0.5);

  const rows = [];
  let disc = null;
  tracks.forEach((track, index) => {
    if (multiDisc && (track.disc || 1) !== disc) {
      disc = track.disc || 1;
      rows.push(h('h3.disc-title', `Disco ${disc}`));
    }
    rows.push(trackRow(tracks, index, { columns: 'album' }));
  });

  page.append(
    h('header.album-header',
      cover(album.art, { full: true, cls: 'album-cover' }),
      h('div.album-info',
        h('p.eyebrow', 'Álbum'),
        h('h1', album.title),
        credited
          ? h('a.album-artist', {
            href: '#',
            onclick: (event) => {
              event.preventDefault();
              go({ view: 'artist', id: credited.artistId });
            },
          }, album.artist)
          : h('p.album-artist', album.artist),
        h('p.album-meta', [album.year || null, count(tracks.length, 'pista', 'pistas'), longTime(album.seconds)].filter(Boolean).join(' · ')),
        h('p.album-format', chip ? h('span.chip', chip) : null, formatLine(tracks[0])),
        h('div.album-actions',
          h('button.button.primary', { type: 'button', onclick: () => playTracks(tracks) }, icon('play', 20), 'Reproducir'),
          h('button.button', { type: 'button', onclick: () => playTracks(shuffled()) }, icon('shuffle', 18), 'Aleatorio'),
          h('button.button', { type: 'button', onclick: () => addToQueue(tracks) }, icon('playlist', 18), 'A la cola'),
          h('button.icon-button', { type: 'button', title: 'Más opciones', onclick: (event) => albumMenu(event, album) }, icon('more'))))),
    h('section.track-list', { role: 'table', ariaLabel: album.title }, rows));
  return null;
}
