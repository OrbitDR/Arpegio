// Pieces shared by the views: covers, album cards, track rows and page headers.
import { albumMenu, playAlbum, playTracks, setFavorite, trackMenu } from './actions.js';
import { h, icon, iconButton, setIcon } from './dom.js';
import { qualityChip, shortFormat, time } from './format.js';
import { go } from './router.js';
import { currentTrack } from './store.js';

export const ART_HOST = 'https://art.arpegio.example';

export function cover(art, { full = false, cls = '' } = {}) {
  const frame = h('div.cover', { class: cls });
  const empty = () => {
    frame.classList.add('empty');
    frame.replaceChildren(icon('note', 34));
  };
  if (!art) {
    empty();
    return frame;
  }
  frame.append(h('img', {
    src: `${ART_HOST}/${full ? 'full' : 'thumb'}/${art}`,
    alt: '',
    loading: 'lazy',
    decoding: 'async',
    draggable: false,
    onerror: empty,
  }));
  return frame;
}

export function albumCard(album) {
  const chip = qualityChip(album);
  return h('article.card', {
    tabIndex: 0,
    onclick: () => go({ view: 'album', id: album.id }),
    onkeydown: (event) => {
      if (event.key === 'Enter') go({ view: 'album', id: album.id });
    },
    oncontextmenu: (event) => albumMenu(event, album),
  },
  h('div.card-art',
    cover(album.art),
    chip ? h('span.chip.on-art', chip) : null,
    h('button.card-play', {
      type: 'button',
      title: 'Reproducir',
      ariaLabel: `Reproducir ${album.title}`,
      onclick: (event) => {
        event.stopPropagation();
        playAlbum(album.id);
      },
    }, icon('play', 26))),
  h('div.card-title', { title: album.title }, album.title),
  h('div.card-sub', { title: album.artist }, album.artist));
}

// Cards keep a minimum width and share out whatever the window leaves.
export function cardLayout(minWidth = 172, captionHeight = 54, gap = 22) {
  return (width) => {
    const columns = Math.max(1, Math.floor((width + gap) / (minWidth + gap)));
    const itemWidth = (width - gap * (columns - 1)) / columns;
    return { columns, itemWidth, itemHeight: itemWidth + captionHeight, gap };
  };
}

export function pageHeader(title, subtitle, ...controls) {
  return h('header.page-header',
    h('div.page-heading', h('h1', title), subtitle ? h('p.page-sub', subtitle) : null),
    controls.length ? h('div.page-controls', controls) : null);
}

export function emptyState(iconName, title, text, action) {
  return h('div.empty-state', icon(iconName, 44), h('h2', title), text ? h('p', text) : null, action);
}

function favoriteButton(track) {
  const button = iconButton(track.favorite ? 'favorite' : 'favoriteOutline', 'Favorito', async (event) => {
    event.stopPropagation();
    await setFavorite(track, !track.favorite);
    setIcon(button, track.favorite ? 'favorite' : 'favoriteOutline', 18);
    button.classList.toggle('on', track.favorite);
  }, { size: 18, cls: `row-favorite${track.favorite ? ' on' : ''}` });
  return button;
}

function linkTo(route, text) {
  return h('a.row-link', {
    href: '#',
    onclick: (event) => {
      event.preventDefault();
      event.stopPropagation();
      go(route);
    },
    ondblclick: (event) => event.stopPropagation(),
  }, text);
}

// columns: which cells the row shows; see the .tracks-* grids in the stylesheet.
export function trackRow(list, index, { columns = 'album', number, menu } = {}) {
  const track = list[index];
  const playing = currentTrack()?.id === track.id;
  const lead = number ?? (track.track || index + 1);
  const play = () => playTracks(list, index);
  const open = (event) => trackMenu(event, track, { list, index, ...menu });

  return h('div.track-row', {
    class: `tracks-${columns}${playing ? ' current' : ''}`,
    dataset: { trackId: track.id },
    role: 'row',
    tabIndex: 0,
    ondblclick: play,
    onkeydown: (event) => {
      if (event.key === 'Enter') play();
    },
    oncontextmenu: open,
  },
  h('div.track-lead',
    h('span.track-number', lead),
    iconButton('play', `Reproducir ${track.title}`, (event) => {
      event.stopPropagation();
      play();
    }, { size: 18, cls: 'track-play' })),
  h('div.track-main',
    h('div.track-title', { title: track.title }, track.title),
    columns === 'album' && track.artist !== track.albumArtist ? h('div.track-sub', track.artist) : null),
  columns === 'all' ? h('div.track-cell', linkTo({ view: 'artist', id: track.artistId }, track.artist)) : null,
  columns === 'all' ? h('div.track-cell', linkTo({ view: 'album', id: track.albumId }, track.album)) : null,
  columns === 'all' ? h('div.track-cell.numeric', track.year || '') : null,
  h('div.track-format', shortFormat(track)),
  h('div.track-time', time(track.seconds)),
  h('div.track-actions',
    favoriteButton(track),
    iconButton('more', 'Más opciones', open, { size: 18 })));
}

export function select(label, value, options, onchange) {
  return h('label.select', h('span', label), h('select', { onchange: (event) => onchange(event.target.value) },
    options.map(([key, text]) => h('option', { value: key, selected: key === value }, text))));
}
