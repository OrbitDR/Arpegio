// The kinds of file beside the search box. Needs a data folder with a catalogue, and it plays nothing.
const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

const KEYS = ['mqa', 'dsd', 'cd', 'hires', 'lossy'];

export default async function run(page, { sleep }) {
  await page.size(1320, 860);
  const albums = await page.call('library.albums');
  const links = () => page.eval(`[...document.querySelectorAll('.section-link')].map((link) => [link.textContent, link.classList.contains('active')])`);
  const heading = () => page.eval(`document.querySelector('.page h1')?.textContent`);
  const subtitle = () => page.eval(`document.querySelector('.page-sub')?.textContent ?? ''`);
  const press = async (index) => {
    await page.eval(`document.querySelectorAll('.section-link')[${index}].click()`);
    await sleep(500);
  };

  check((await links()).length === KEYS.length, 'every section has its button');
  check(albums.every((album) => KEYS.includes(album.section)), 'every album has a section');
  check((await links()).every(([, active]) => !active), 'none is lit on all the albums');

  let total = 0;
  for (const [index, key] of KEYS.entries()) {
    await press(index);
    const expected = albums.filter((album) => album.section === key).length;
    total += expected;
    const shown = await links();
    const number = Number((await subtitle()).replace(/\D/g, ''));
    check(number === expected, `${key} lists its ${expected} albums`);
    check(shown.every(([, active], at) => active === (at === index)), `${key} is the only one lit`);
    check((await heading()) === shown[index][0], `${key} names the page after itself`);
    await page.shot(`section-${key}`);
  }
  check(total === albums.length, `each of the ${albums.length} albums is in exactly one section`);

  await press(KEYS.length - 1);
  check((await heading()) === 'Álbumes', 'pressing the open section goes back to all the albums');
  check((await links()).every(([, active]) => !active), 'and nothing stays lit');

  await press(0);
  await page.eval(`document.querySelector('.card')?.click()`);
  await sleep(500);
  check((await page.eval(`document.querySelector('.album-format .chip')?.textContent ?? 'none'`)) !== 'none' || !albums.some((album) => album.mqa), 'an album of the section shows its label');
  await page.eval(`history.back()`);
  await sleep(600);
  check((await links())[0][1], 'going back returns to the section');

  const search = async (text) => {
    await page.eval(`(() => {
      const input = document.getElementById('search');
      input.value = ${JSON.stringify(text)};
      input.dispatchEvent(new Event('input'));
    })()`);
    await sleep(600);
  };
  await search('a');
  check((await links()).every(([, active]) => !active), 'searching leaves the section');
  await search('');
  check((await links())[0][1], 'and clearing the search comes back to it');

  // The button row must not push anything out of the window, even at its smallest with the queue open.
  await page.size(900, 600);
  await page.eval(`import('/js/store.js').then((s) => s.set('queueOpen', true))`);
  await sleep(400);
  const spill = `(() => {
    const bar = document.querySelector('.top-bar').getBoundingClientRect();
    return [...document.querySelectorAll('.section-link, .search')].every((item) => {
      const box = item.getBoundingClientRect();
      return box.left >= bar.left && box.right <= bar.right && box.bottom <= bar.bottom;
    });
  })()`;
  check(await page.eval(spill), 'the buttons fit at the smallest window with the queue open');
  await page.shot('section-narrow');
  await page.eval(`import('/js/store.js').then((s) => s.set('queueOpen', false))`);
  await sleep(300);
  check(await page.eval(spill), 'and at the smallest window with it closed');
  await page.size(1320, 860);
}
