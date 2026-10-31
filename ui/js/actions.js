// What the user can do with tracks and albums, wherever they appear.
import { call } from './api.js';
import { askText, reportError, showMenu, toast } from './feedback.js';
import { go } from './router.js';
import { state } from './store.js';

const ids = (tracks) => tracks.map((track) => track.id);

function guarded(promise) {
  return promise.catch(reportError);
}

export function playTracks(tracks, index = 0) {
  return guarded(call('queue.play', { tracks: ids(tracks), index }));
}

export function playAlbum(albumId) {
  return guarded(call('library.albumTracks', { id: albumId }).then((tracks) => call('queue.play', { tracks: ids(tracks) })));
}

export function addToQueue(tracks) {
  return guarded(call('queue.add', { tracks: ids(tracks) }).then(() => {
    toast(tracks.length === 1 ? `Añadida a la cola: ${tracks[0].title}` : `${tracks.length} pistas añadidas a la cola`);
  }));
}

export function playNext(tracks) {
  return guarded(call('queue.playNext', { tracks: ids(tracks) }).then(() => {
    toast(tracks.length === 1 ? `A continuación: ${tracks[0].title}` : `${tracks.length} pistas a continuación`);
  }));
}

export async function setFavorite(track, favorite) {
  track.favorite = favorite;
  for (const queued of state.queue.tracks) {
    if (queued.id === track.id) queued.favorite = favorite;
  }
  await guarded(call('library.setFavorite', { id: track.id, favorite }));
}

async function newPlaylist(tracks) {
  const name = await askText({ title: 'Lista nueva', placeholder: 'Nombre de la lista', action: 'Crear' });
  if (!name) return;
  await guarded(call('playlists.create', { name, tracks: ids(tracks) }));
  toast(`Lista creada: ${name}`);
}

function playlistItems(tracks) {
  return [
    ...state.playlists.map((playlist) => ({
      label: playlist.name,
      icon: 'playlist',
      action: () => guarded(call('playlists.add', { id: playlist.id, tracks: ids(tracks) }).then(() => {
        toast(`Añadido a ${playlist.name}`);
      })),
    })),
    null,
    { label: 'Lista nueva…', icon: 'add', action: () => newPlaylist(tracks) },
  ];
}

export function trackMenu(event, track, { list, index, extra = [] } = {}) {
  event.preventDefault();
  event.stopPropagation();
  const items = [
    { label: 'Reproducir', icon: 'play', action: () => (list ? playTracks(list, index) : playTracks([track])) },
    { label: 'Reproducir a continuación', icon: 'next', action: () => playNext([track]) },
    { label: 'Añadir a la cola', icon: 'playlist', action: () => addToQueue([track]) },
    { label: 'Añadir a una lista', icon: 'add', items: playlistItems([track]) },
    null,
    {
      label: track.favorite ? 'Quitar de favoritos' : 'Añadir a favoritos',
      icon: track.favorite ? 'favorite' : 'favoriteOutline',
      action: () => setFavorite(track, !track.favorite),
    },
    { label: 'Ir al álbum', icon: 'album', action: () => go({ view: 'album', id: track.albumId }) },
    { label: 'Ir al artista', icon: 'person', action: () => go({ view: 'artist', id: track.artistId }) },
    ...extra,
  ];
  showMenu(event.clientX, event.clientY, items);
}

export function albumMenu(event, album) {
  event.preventDefault();
  event.stopPropagation();
  const withTracks = (action) => () => guarded(call('library.albumTracks', { id: album.id }).then(action));
  showMenu(event.clientX, event.clientY, [
    { label: 'Reproducir', icon: 'play', action: () => playAlbum(album.id) },
    { label: 'Reproducir a continuación', icon: 'next', action: withTracks(playNext) },
    { label: 'Añadir a la cola', icon: 'playlist', action: withTracks(addToQueue) },
    {
      label: 'Añadir a una lista',
      icon: 'add',
      action: withTracks((tracks) => showMenu(event.clientX, event.clientY, playlistItems(tracks))),
    },
  ]);
}
