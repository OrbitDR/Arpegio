// The title bar the page draws in place of the one from Windows: the name, room to move the window, and its
// three buttons. Windows moves the window from the bar itself, marked with `app-region: drag` in the stylesheet.
import { call } from './api.js';
import { h } from './dom.js';
import { reportError } from './feedback.js';
import { watch } from './store.js';

// The glyphs of Windows' own buttons, from Segoe Fluent Icons, or Segoe MDL2 Assets on Windows 10.
const GLYPHS = { minimize: '', maximize: '', restore: '', close: '' };

// Out of the tab order, as in any window: the keyboard has Alt+F4 and Windows+arrows.
function captionButton(glyph, label, method, cls) {
  return h('button.caption-button', {
    type: 'button',
    title: label,
    ariaLabel: label,
    tabIndex: -1,
    class: cls,
    onclick: () => call(method).catch(reportError),
  }, glyph);
}

// The window has no border above the page, so the page's own top edge starts the resize, corners included.
function resizeEdge(edge) {
  return h('div.resize-edge', {
    class: edge,
    ariaHidden: 'true',
    onpointerdown: (event) => {
      if (event.button !== 0) return;
      event.preventDefault();
      call('window.resize', { edge }).catch(reportError);
    },
  });
}

export function createTitleBar() {
  const maximize = captionButton(GLYPHS.maximize, 'Maximizar', 'window.maximize', 'maximize');
  const bar = h('header.title-bar',
    h('div.title-brand', h('img', { src: 'mark.svg', alt: '', width: 16, height: 16 }), h('span', 'Arpegio')),
    h('div.caption-buttons',
      captionButton(GLYPHS.minimize, 'Minimizar', 'window.minimize', 'minimize'),
      maximize,
      captionButton(GLYPHS.close, 'Cerrar', 'window.close', 'close')),
    resizeEdge('topleft'),
    resizeEdge('top'),
    resizeEdge('topright'));

  watch('window', ({ maximized, active, backdrop }) => {
    const root = document.documentElement;
    root.classList.toggle('solid', !backdrop);
    root.classList.toggle('inactive', !active);
    root.classList.toggle('maximized', maximized);
    const label = maximized ? 'Restaurar' : 'Maximizar';
    if (maximize.title === label) return;
    maximize.textContent = maximized ? GLYPHS.restore : GLYPHS.maximize;
    maximize.title = label;
    maximize.ariaLabel = label;
  });
  return bar;
}
