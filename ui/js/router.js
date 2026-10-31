// Moves between views. The browser's history gives back, forward and the mouse's side buttons for free.
const views = new Map();
const scrollMemory = new Map();
let outlet;
let scroller;
let current = null;
let cleanup = null;
let renderTicket = 0;
const listeners = new Set();

export function defineView(name, render) {
  views.set(name, render);
}

export function setOutlet(element, scrollElement) {
  outlet = element;
  scroller = scrollElement;
}

export function onRoute(listener) {
  listeners.add(listener);
}

export function currentRoute() {
  return current;
}

function routeKey(route) {
  return JSON.stringify(route);
}

async function render(route, { keepScroll = false } = {}) {
  const ticket = ++renderTicket;
  if (current) scrollMemory.set(routeKey(current), scroller.scrollTop);
  cleanup?.();
  cleanup = null;
  current = route;
  for (const listener of listeners) listener(route);

  const page = document.createElement('div');
  page.className = 'page';
  const finish = await views.get(route.view)(page, route);
  if (ticket !== renderTicket) {
    finish?.();  // another route won while this one was loading
    return;
  }
  outlet.replaceChildren(page);
  cleanup = finish ?? null;
  const place = keepScroll ? scroller.scrollTop : scrollMemory.get(routeKey(route)) ?? 0;
  scroller.scrollTop = place;
  // Virtual lists get their height in the next layout, and only then can the page scroll that far.
  requestAnimationFrame(() => requestAnimationFrame(() => {
    if (ticket === renderTicket) scroller.scrollTop = place;
  }));
}

export function go(route, { replace = false } = {}) {
  if (replace) history.replaceState(route, '');
  else history.pushState(route, '');
  render(route);
}

// Draws the same view again with fresh data, keeping its place.
export function refresh() {
  if (current) render(current, { keepScroll: true });
}

export function start(route) {
  history.replaceState(route, '');
  window.addEventListener('popstate', (event) => {
    if (event.state) render(event.state);
  });
  render(route);
}
