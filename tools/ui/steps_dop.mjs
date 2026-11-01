// The guided DoP check in Ajustes, with real mouse events, on ASIO and WASAPI exclusive of the output in
// ARPEGIO_DEVICE. Sends DSD silence: nothing is audible, and the DAC should show DSD while the dialog is open.
const check = (ok, label) => {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) process.exitCode = 1;
};

const name = new RegExp(process.env.ARPEGIO_DEVICE ?? 'topping', 'i');

export default async function run(page, { sleep }) {
  const devices = () => page.eval(`import('/js/store.js').then((s) => s.state.devices)`);
  const mouse = (type, x, y, extra = {}) => page.send('Input.dispatchMouseEvent', { type, x, y, ...extra });
  const clickButton = async (text) => {
    const box = await page.eval(`(() => {
      const button = [...document.querySelectorAll('button')].find((item) => item.textContent === ${JSON.stringify(text)});
      button.scrollIntoView({ block: 'center' });
      const { left, top, width, height } = button.getBoundingClientRect();
      return { x: left + width / 2, y: top + height / 2 };
    })()`);
    await sleep(100);
    await mouse('mouseMoved', box.x, box.y);
    await mouse('mousePressed', box.x, box.y, { button: 'left', buttons: 1, clickCount: 1 });
    await sleep(80);
    await mouse('mouseReleased', box.x, box.y, { button: 'left', buttons: 0, clickCount: 1 });
  };
  const dialogText = `document.querySelector('.dialog')?.innerText ?? ''`;
  const confirmed = async (key) => (await devices()).list.find((item) => item.key === key).settings.dopConfirmed;

  await page.size(1320, 860);
  await page.wait(`import('/js/store.js').then((s) => s.state.devices.known)`, 30000);
  const list = (await devices()).list;
  const outputs = ['asio', 'exclusive'].map((kind) => list.find((item) => item.kind === kind && !item.error && name.test(item.name))).filter(Boolean);
  check(outputs.length === 2, `there are both outputs to try (${outputs.map((item) => item.kind)})`);

  for (const output of outputs) {
    await page.call('devices.select', { key: output.key });
    await page.go({ view: 'settings' });
    check(!(await confirmed(output.key)), `${output.kind}: starts unconfirmed`);

    await clickButton('Probar');
    await page.wait(`(${dialogText}).includes('Prueba de DoP')`);
    await page.shot(`60-dop-ask-${output.kind}`);
    check((await page.call('devices.dopTestStatus')).running === false, `${output.kind}: nothing is sent before Empezar`);

    await clickButton('Empezar');
    await page.wait(`(${dialogText}).includes('Mira la pantalla')`, 15000);
    const test = await page.call('devices.dopTestStatus');
    check(test.sending && test.rate === 176400, `${output.kind}: DoP is going out at ${test.rate} Hz in ${test.encoding}`);
    check(!test.error, `${output.kind}: without an error (${test.error})`);
    await page.shot(`61-dop-sending-${output.kind}`);
    await sleep(3000);
    check((await page.call('app.state')).status.state === 'stopped', `${output.kind}: the queue does not start`);

    await clickButton('Sí, indica DSD');
    await page.wait(`(${dialogText}) === ''`);
    await sleep(300);
    check(await confirmed(output.key), `${output.kind}: Sí confirms it`);
    check((await page.call('devices.dopTestStatus')).running === false, `${output.kind}: and the stream closes`);

    await clickButton('Probar');
    await clickButton('Empezar');
    await page.wait(`(${dialogText}).includes('Mira la pantalla')`, 15000);
    await page.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
    await page.wait(`(${dialogText}) === ''`);
    await sleep(300);
    check(await confirmed(output.key), `${output.kind}: Escape changes nothing`);
    check((await page.call('devices.dopTestStatus')).running === false, `${output.kind}: and closes the stream`);

    await clickButton('Probar');
    await clickButton('Empezar');
    await page.wait(`(${dialogText}).includes('Mira la pantalla')`, 15000);
    await clickButton('No, indica PCM o nada');
    await page.wait(`(${dialogText}) === ''`);
    await sleep(300);
    check(!(await confirmed(output.key)), `${output.kind}: No takes the confirmation away`);
  }
}
