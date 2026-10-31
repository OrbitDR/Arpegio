// The queue, in a panel beside the page. Rows can be dragged to reorder.
import { call } from './api.js';
import { trackMenu } from './actions.js';
import { h, icon, iconButton } from './dom.js';
import { reportError } from './feedback.js';
import { count, longTime, time } from './format.js';
import { cover } from './parts.js';
import { state, watch } from './store.js';
import { VirtualList, rows } from './virtual.js';

const ROW_HEIGHT = 58;
const send = (method, params) => call(method, params).catch(reportError);

export function createQueuePanel() {
  const heading = h('div.queue-sub');
  const scroller = h('div.queue-scroll');
  let list = null;
  let shownIndex = -1;
  let dragFrom = null;

  const panel = h('aside.queue-panel', { hidden: true, ariaLabel: 'Cola de reproducción' },
    h('header.queue-head',
      h('div', h('h2', 'Cola'), heading),
      iconButton('delete', 'Vaciar la cola', () => send('queue.clear'), { size: 18 })),
    scroller);

  function row(index) {
    const track = state.queue.tracks[index];
    const current = index === state.status?.index;
    const jump = () => send('player.jump', { index });
    const element = h('div.queue-row', {
      class: current ? 'current' : '',
      draggable: true,
      tabIndex: 0,
      ondblclick: jump,
      onkeydown: (event) => {
        if (event.key === 'Enter') jump();
        if (event.key === 'Delete') send('queue.remove', { index });
      },
      oncontextmenu: (event) => trackMenu(event, track, {
        extra: [null, { label: 'Quitar de la cola', icon: 'close', action: () => send('queue.remove', { index }) }],
      }),
      ondragstart: (event) => {
        dragFrom = index;
        event.dataTransfer.effectAllowed = 'move';
        event.dataTransfer.setData('text/plain', '');
      },
      ondragover: (event) => {
        event.preventDefault();
        element.classList.toggle('drop-before', index <= dragFrom);
        element.classList.toggle('drop-after', index > dragFrom);
      },
      ondragleave: () => element.classList.remove('drop-before', 'drop-after'),
      ondrop: (event) => {
        event.preventDefault();
        element.classList.remove('drop-before', 'drop-after');
        if (dragFrom !== null && dragFrom !== index) send('queue.move', { from: dragFrom, to: index });
        dragFrom = null;
      },
    },
    cover(track.art, { cls: 'small' }),
    h('div.queue-text',
      h('div.queue-title', { title: track.title }, track.title),
      h('div.queue-artist', track.artist)),
    h('span.queue-time', time(track.seconds)),
    iconButton('close', 'Quitar', (event) => {
      event.stopPropagation();
      send('queue.remove', { index });
    }, { size: 16, cls: 'queue-remove' }));
    return element;
  }

  function rebuild() {
    const { tracks } = state.queue;
    const total = tracks.reduce((sum, track) => sum + track.seconds, 0);
    heading.textContent = tracks.length ? `${count(tracks.length, 'pista', 'pistas')} · ${longTime(total)}` : '';
    if (!tracks.length) {
      list?.destroy();
      list = null;
      scroller.replaceChildren(h('div.queue-empty', icon('playlist', 36), h('p', 'La cola está vacía.')));
      return;
    }
    // The same list stays, so removing a track far down does not send the panel back to the top.
    if (list) {
      list.setCount(tracks.length);
      return;
    }
    list = new VirtualList({ scroller, count: tracks.length, layout: rows(ROW_HEIGHT), render: row });
    scroller.replaceChildren(list.element);
  }

  watch('queue', () => {
    if (!panel.hidden) rebuild();
  });
  watch('status', (status) => {
    if (status.index !== shownIndex) {
      shownIndex = status.index;
      if (!panel.hidden) list?.redraw();
    }
  });
  watch('queueOpen', (open) => {
    panel.hidden = !open;
    if (open) {
      rebuild();
      requestAnimationFrame(() => list?.scrollToIndex(state.status?.index ?? 0));
    }
  });
  return panel;
}
