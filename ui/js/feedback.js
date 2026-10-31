// Short messages, the context menu and the one small dialog the application needs.
import { h, icon } from './dom.js';

const TOAST_MS = 6000;
let toasts;
let openMenu = null;

export function initFeedback(root) {
  toasts = h('div.toasts', { role: 'status' });
  root.append(toasts);
  document.addEventListener('pointerdown', (event) => {
    if (openMenu && !openMenu.contains(event.target)) closeMenu();
  });
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') closeMenu();
  });
  window.addEventListener('blur', closeMenu);
}

export function toast(message, kind = 'info') {
  const item = h('div.toast', { class: kind }, kind === 'error' ? icon('warning', 18) : null, h('span', message));
  toasts.append(item);
  setTimeout(() => item.remove(), TOAST_MS);
}

export function reportError(error) {
  toast(error.message ?? String(error), 'error');
}

export function closeMenu() {
  openMenu?.remove();
  openMenu = null;
}

// items: { label, icon, action, danger, items } or null for a separator.
export function showMenu(x, y, items) {
  closeMenu();
  const menu = h('div.menu', { role: 'menu' });
  for (const item of items) {
    if (!item) {
      menu.append(h('div.menu-line'));
      continue;
    }
    menu.append(h('button.menu-item', {
      type: 'button',
      role: 'menuitem',
      class: item.danger ? 'danger' : '',
      onclick: (event) => {
        event.stopPropagation();
        if (item.items) {
          showMenu(x, y, item.items);
          return;
        }
        closeMenu();
        item.action();
      },
    }, item.icon ? icon(item.icon, 18) : h('span.menu-gap'), h('span.menu-label', item.label), item.items ? icon('chevron', 16) : null));
  }
  document.body.append(menu);
  const box = menu.getBoundingClientRect();
  menu.style.left = `${Math.min(x, innerWidth - box.width - 8)}px`;
  menu.style.top = `${Math.min(y, innerHeight - box.height - 8)}px`;
  menu.querySelector('button')?.focus({ preventScroll: true });
  openMenu = menu;
}

export function askText({ title, placeholder = '', value = '', action }) {
  return new Promise((resolve) => {
    const finish = (result) => {
      overlay.remove();
      resolve(result);
    };
    const input = h('input.text-input', { type: 'text', placeholder, value, spellcheck: false });
    const form = h('form.dialog', {
      onsubmit: (event) => {
        event.preventDefault();
        finish(input.value.trim() || null);
      },
    },
    h('h2', title),
    input,
    h('div.dialog-actions',
      h('button.button', { type: 'button', onclick: () => finish(null) }, 'Cancelar'),
      h('button.button.primary', { type: 'submit' }, action)));
    const overlay = h('div.overlay', {
      onkeydown: (event) => {
        if (event.key === 'Escape') finish(null);
      },
      onpointerdown: (event) => {
        if (event.target === overlay) finish(null);
      },
    }, form);
    document.body.append(overlay);
    input.focus();
    input.select();
  });
}
