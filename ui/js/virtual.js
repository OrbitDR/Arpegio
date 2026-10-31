// A long list that only keeps the visible rows in the page, so a collection of any size scrolls smoothly.
// `layout(width)` returns { columns, itemWidth, itemHeight, gap }; `render(index)` builds one item.
const OVERSCAN = 4;

export class VirtualList {
  constructor({ scroller, count, layout, render, cls = '' }) {
    this.scroller = scroller;
    this.count = count;
    this.layoutFor = layout;
    this.renderItem = render;
    this.items = new Map();
    this.element = document.createElement('div');
    this.element.className = `virtual ${cls}`;

    this.onScroll = () => this.schedule();
    scroller.addEventListener('scroll', this.onScroll, { passive: true });
    this.observer = new ResizeObserver(() => {
      const width = this.element.clientWidth;
      if (width !== this.width) this.relayout(width);
    });
    this.observer.observe(this.element);
  }

  relayout(width) {
    this.width = width;
    this.metrics = this.layoutFor(width);
    const { columns, itemHeight, gap } = this.metrics;
    const rows = Math.ceil(this.count / columns);
    this.element.style.height = `${Math.max(0, rows * (itemHeight + gap) - gap)}px`;
    this.clear();
    this.update();
  }

  schedule() {
    if (this.frame) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.update();
    });
  }

  update() {
    if (!this.metrics || !this.element.isConnected) return;
    const { columns, itemWidth, itemHeight, gap } = this.metrics;
    const rowStep = itemHeight + gap;
    const top = this.element.getBoundingClientRect().top - this.scroller.getBoundingClientRect().top;
    const firstRow = Math.max(0, Math.floor(-top / rowStep) - OVERSCAN);
    const lastRow = Math.floor((this.scroller.clientHeight - top) / rowStep) + OVERSCAN;
    const first = firstRow * columns;
    const last = Math.min(this.count - 1, (lastRow + 1) * columns - 1);

    for (const [index, item] of this.items) {
      if (index < first || index > last) {
        item.remove();
        this.items.delete(index);
      }
    }
    for (let index = first; index <= last; ++index) {
      if (this.items.has(index)) continue;
      const item = this.renderItem(index);
      const column = index % columns;
      const row = Math.floor(index / columns);
      item.style.position = 'absolute';
      item.style.width = `${itemWidth}px`;
      item.style.height = `${itemHeight}px`;
      item.style.transform = `translate(${column * (itemWidth + gap)}px, ${row * rowStep}px)`;
      this.element.append(item);
      this.items.set(index, item);
    }
  }

  clear() {
    for (const item of this.items.values()) item.remove();
    this.items.clear();
  }

  // The data behind the visible items changed.
  redraw() {
    this.clear();
    this.update();
  }

  setCount(count) {
    this.count = count;
    if (this.width) this.relayout(this.width);
  }

  scrollToIndex(index) {
    if (!this.metrics) return;
    const { columns, itemHeight, gap } = this.metrics;
    const row = Math.floor(index / columns);
    this.scroller.scrollTop += this.element.getBoundingClientRect().top - this.scroller.getBoundingClientRect().top
      + row * (itemHeight + gap) - this.scroller.clientHeight / 2;
  }

  destroy() {
    cancelAnimationFrame(this.frame);
    this.observer.disconnect();
    this.scroller.removeEventListener('scroll', this.onScroll);
  }
}

export function rows(height) {
  return (width) => ({ columns: 1, itemWidth: width, itemHeight: height, gap: 0 });
}
