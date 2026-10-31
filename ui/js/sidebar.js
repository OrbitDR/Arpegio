// Navigation on the left, with the playlists and the state of the library scan.
import { call } from './api.js';
import { h, icon, iconButton } from './dom.js';
import { askText, reportError, showMenu } from './feedback.js';
import { go, onRoute } from './router.js';
import { state, watch } from './store.js';

const LINKS = [
  ['albums', 'album', 'Álbumes'],
  ['artists', 'person', 'Artistas'],
  ['tracks', 'list', 'Pistas'],
  ['folders', 'folder', 'Carpetas'],
  ['favorites', 'favorite', 'Favoritos'],
];

const ROOT_OF = { album: 'albums', artist: 'artists' };

export function createSidebar() {
  const links = new Map();
  const navLink = (view, iconName, label, route = { view }) => {
    const link = h('button.nav-link', { type: 'button', onclick: () => go(route) }, icon(iconName, 20), h('span', label));
    links.set(JSON.stringify(route), link);
    return link;
  };

  const playlists = h('div.nav-playlists');
  const scan = h('div.nav-scan', { hidden: true });
  const settings = navLink('settings', 'settings', 'Ajustes');

  const createPlaylist = async () => {
    const name = await askText({ title: 'Lista nueva', placeholder: 'Nombre de la lista', action: 'Crear' });
    if (name) call('playlists.create', { name }).catch(reportError);
  };

  const sidebar = h('nav.sidebar', { ariaLabel: 'Secciones' },
    h('div.brand', h('img', { src: 'mark.svg', alt: '', width: 26, height: 26 }), h('span', 'Arpegio')),
    h('div.nav-group', LINKS.map(([view, iconName, label]) => navLink(view, iconName, label))),
    h('div.nav-title', h('span', 'Listas'), iconButton('add', 'Lista nueva', createPlaylist, { size: 18 })),
    playlists,
    h('div.nav-bottom', scan, settings));

  function drawPlaylists() {
    for (const key of links.keys()) {
      if (key.includes('"playlist"')) links.delete(key);
    }
    playlists.replaceChildren(...state.playlists.map((playlist) => {
      const route = { view: 'playlist', id: playlist.id };
      const link = navLink('playlist', 'playlist', playlist.name, route);
      link.addEventListener('contextmenu', (event) => {
        event.preventDefault();
        showMenu(event.clientX, event.clientY, [
          {
            label: 'Cambiar nombre',
            icon: 'settings',
            action: async () => {
              const name = await askText({ title: 'Cambiar nombre', value: playlist.name, action: 'Guardar' });
              if (name) call('playlists.rename', { id: playlist.id, name }).catch(reportError);
            },
          },
          {
            label: 'Eliminar lista',
            icon: 'delete',
            danger: true,
            action: () => call('playlists.delete', { id: playlist.id }).catch(reportError),
          },
        ]);
      });
      return link;
    }));
    mark(currentRouteKey);
  }

  let currentRouteKey = '';
  function mark(key) {
    currentRouteKey = key;
    for (const [linkKey, link] of links) link.classList.toggle('active', linkKey === key);
  }

  onRoute((route) => {
    const root = ROOT_OF[route.view];
    mark(JSON.stringify(root ? { view: root } : route.view === 'playlist' ? { view: 'playlist', id: route.id } : { view: route.view }));
  });

  watch('playlists', drawPlaylists);
  watch('library', (library) => {
    scan.hidden = !library.scanning;
    if (!library.scanning) return;
    const share = library.total ? library.done / library.total : 0;
    scan.replaceChildren(
      h('div.nav-scan-text', library.total ? `Leyendo la biblioteca… ${Math.round(share * 100)} %` : 'Buscando música…'),
      h('div.progress', h('i', { style: { width: `${share * 100}%` } })));
  });
  return sidebar;
}
