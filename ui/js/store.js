// What the page knows about the application, and who wants to hear when it changes.
import { call, listen } from './api.js';

export const state = {
  status: null,
  queue: { tracks: [], index: 0 },
  library: null,
  folders: [],
  playlists: [],
  devices: { known: false, selected: '', list: [] },
  queueOpen: false,
};

const watchers = new Map();
const catalogue = new Map();

export function watch(key, watcher) {
  if (!watchers.has(key)) watchers.set(key, new Set());
  watchers.get(key).add(watcher);
  return () => watchers.get(key).delete(watcher);
}

export function set(key, value) {
  state[key] = value;
  for (const watcher of watchers.get(key) ?? []) watcher(value);
}

// Lists of the catalogue stay in memory until it changes.
export function cached(key, load) {
  if (!catalogue.has(key)) {
    const loading = load().catch((error) => {
      catalogue.delete(key);
      throw error;
    });
    catalogue.set(key, loading);
  }
  return catalogue.get(key);
}

export function forgetCatalogue() {
  catalogue.clear();
}

export function currentTrack() {
  const { status, queue } = state;
  return status ? queue.tracks[status.index] ?? null : null;
}

export function selectedDevice() {
  return state.devices.list.find((device) => device.key === state.devices.selected);
}

export async function connect() {
  listen('status', (status) => set('status', status));
  listen('queue', (queue) => {
    // The next status is up to a tick away, and meanwhile the old index would mark the wrong track.
    if (state.status) {
      state.status = { ...state.status, index: queue.index, trackId: queue.tracks[queue.index]?.id ?? 0 };
    }
    set('queue', queue);
  });
  listen('devices', (devices) => set('devices', devices));
  listen('playlists', (playlists) => set('playlists', playlists));
  listen('library', (library) => {
    const changed = library.revision !== state.library?.revision;
    if (changed) forgetCatalogue();
    set('library', library);
    if (changed) {
      call('library.folders').then((folders) => set('folders', folders));
      set('catalogue', library.revision);
    }
  });

  const initial = await call('app.state');
  state.sacdMultichannel = initial.sacdMultichannel;
  state.replayGain = initial.replayGain;
  state.resampler = initial.resampler;
  for (const key of ['queue', 'status', 'library', 'folders', 'playlists', 'devices']) set(key, initial[key]);
}
