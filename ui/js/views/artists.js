// Artists, as a grid and one by one.
import { call } from '../api.js';
import { addToQueue, playTracks } from '../actions.js';
import { fill, h, icon } from '../dom.js';
import { compareText, count } from '../format.js';
import { albumCard, cardLayout, cover, emptyState, pageHeader, trackRow } from '../parts.js';
import { go } from '../router.js';
import { cached } from '../store.js';
import { VirtualList, rows } from '../virtual.js';

const TRACK_ROW_HEIGHT = 48;

function artistCard(artist) {
  const open = () => go({ view: 'artist', id: artist.id });
  return h('article.card.artist', {
    tabIndex: 0,
    onclick: open,
    onkeydown: (event) => {
      if (event.key === 'Enter') open();
    },
  },
  h('div.card-art', cover(artist.art, { cls: 'round' })),
  h('div.card-title', { title: artist.name }, artist.name),
  h('div.card-sub', artist.albums ? count(artist.albums, 'álbum', 'álbumes') : count(artist.tracks, 'pista', 'pistas')));
}

function loadArtists() {
  return cached('artists', () => call('library.artists'));
}

export async function artistsView(page) {
  const artists = (await loadArtists()).filter((artist) => artist.tracks > 0 || artist.albums > 0);
  if (!artists.length) {
    page.append(emptyState('person', 'Todavía no hay artistas'));
    return null;
  }
  artists.sort((a, b) => compareText(a.name, b.name));
  const list = new VirtualList({
    scroller: document.getElementById('scroller'),
    count: artists.length,
    layout: cardLayout(150, 54),
    render: (index) => artistCard(artists[index]),
  });
  page.append(pageHeader('Artistas', count(artists.length, 'artista', 'artistas')), list.element);
  return () => list.destroy();
}

export async function artistView(page, { id }) {
  const [artists, detail] = await Promise.all([loadArtists(), call('library.artist', { id })]);
  const artist = artists.find((item) => item.id === id);
  if (!artist) {
    page.append(emptyState('person', 'Este artista ya no está en la biblioteca'));
    return null;
  }
  const { albums, tracks } = detail;
  const list = new VirtualList({
    scroller: document.getElementById('scroller'),
    count: tracks.length,
    layout: rows(TRACK_ROW_HEIGHT),
    render: (index) => trackRow(tracks, index, { columns: 'all', number: index + 1 }),
    cls: 'track-list',
  });

  fill(page,
    h('header.artist-header',
      cover(artist.art, { cls: 'round artist-photo' }),
      h('div.album-info',
        h('p.eyebrow', 'Artista'),
        h('h1', artist.name),
        h('p.album-meta', [count(albums.length, 'álbum', 'álbumes'), count(tracks.length, 'pista', 'pistas')].join(' · ')),
        tracks.length
          ? h('div.album-actions',
            h('button.button.primary', { type: 'button', onclick: () => playTracks(tracks) }, icon('play', 20), 'Reproducir'),
            h('button.button', { type: 'button', onclick: () => addToQueue(tracks) }, icon('playlist', 18), 'A la cola'))
          : null)),
    albums.length ? h('h2.section-title', 'Álbumes') : null,
    albums.length ? h('div.card-row', albums.map(albumCard)) : null,
    tracks.length ? h('h2.section-title', 'Pistas') : null,
    list.element);
  return () => list.destroy();
}
