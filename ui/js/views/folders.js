// The library as it sits on disk: one watched folder at a time, level by level.
import { call } from '../api.js';
import { playTracks } from '../actions.js';
import { fill, h, icon } from '../dom.js';
import { compareText, count } from '../format.js';
import { emptyState, pageHeader, trackRow } from '../parts.js';
import { go } from '../router.js';
import { cached, state } from '../store.js';

function buildTree(tracks) {
  const root = { folders: new Map(), tracks: [], total: 0 };
  for (const track of tracks) {
    let node = root;
    ++node.total;
    const parts = track.path.split('/');
    parts.pop();
    for (const part of parts) {
      if (!node.folders.has(part)) node.folders.set(part, { folders: new Map(), tracks: [], total: 0 });
      node = node.folders.get(part);
      ++node.total;
    }
    node.tracks.push(track);
  }
  return root;
}

function allTracks(node) {
  const collected = [...node.tracks];
  for (const child of node.folders.values()) collected.push(...allTracks(child));
  return collected;
}

function folderRow(name, node, route) {
  const open = () => go(route);
  return h('button.folder-row', { type: 'button', onclick: open },
    icon('folder', 22),
    h('span.folder-name', { title: name }, name),
    h('span.folder-count', count(node.total, 'pista', 'pistas')),
    icon('chevron', 18));
}

export async function foldersView(page, { folder, path = [] }) {
  if (!state.folders.length) {
    page.append(emptyState('folder', 'No hay carpetas en la biblioteca', 'Añádelas en Ajustes.',
      h('button.button.primary', { type: 'button', onclick: () => go({ view: 'settings' }) }, 'Ir a Ajustes')));
    return null;
  }

  if (!folder) {
    const trees = await Promise.all(state.folders.map(async (item) => [item, buildTree(await treeTracks(item.id))]));
    page.append(pageHeader('Carpetas', count(state.folders.length, 'carpeta vigilada', 'carpetas vigiladas')),
      h('div.folder-list', trees.map(([item, tree]) => folderRow(item.path, tree, { view: 'folders', folder: item.id }))));
    return null;
  }

  const root = state.folders.find((item) => item.id === folder);
  if (!root) {
    page.append(emptyState('folder', 'Esta carpeta ya no está en la biblioteca'));
    return null;
  }

  let node = buildTree(await treeTracks(folder));
  const crumbs = [h('button.crumb', { type: 'button', onclick: () => go({ view: 'folders' }) }, 'Carpetas')];
  const walked = [];
  crumbs.push(icon('chevron', 16), h('button.crumb', { type: 'button', onclick: () => go({ view: 'folders', folder }) }, root.name || root.path));
  for (const part of path) {
    node = node?.folders.get(part);
    walked.push(part);
    const route = { view: 'folders', folder, path: [...walked] };
    crumbs.push(icon('chevron', 16), h('button.crumb', { type: 'button', onclick: () => go(route) }, part));
  }
  if (!node) {
    page.append(emptyState('folder', 'Esta carpeta ya no existe'));
    return null;
  }

  const folders = [...node.folders.entries()].sort(([a], [b]) => compareText(a, b));
  const tracks = [...node.tracks].sort((a, b) => compareText(a.path, b.path) || a.part - b.part);
  const everything = allTracks(node);

  fill(page,
    h('nav.crumbs', { ariaLabel: 'Ruta' }, crumbs),
    pageHeader(path.at(-1) ?? root.name ?? root.path, count(node.total, 'pista', 'pistas'),
      h('button.button.primary', { type: 'button', onclick: () => playTracks(everything) }, icon('play', 20), 'Reproducir')),
    folders.length
      ? h('div.folder-list', folders.map(([name, child]) => folderRow(name, child, { view: 'folders', folder, path: [...path, name] })))
      : null,
    tracks.length
      ? h('section.track-list', tracks.map((_, index) => trackRow(tracks, index, { columns: 'album', number: index + 1 })))
      : null);
  return null;
}

function treeTracks(id) {
  return cached(`folder:${id}`, () => call('library.folderTracks', { id }));
}
