// Audio output and library. Every change is saved and applied at once.
import { call } from '../api.js';
import { h, icon, iconButton } from '../dom.js';
import { openDialog, reportError, toast } from '../feedback.js';
import { count, decibels, dsdName, kilohertz } from '../format.js';
import { pageHeader, select } from '../parts.js';
import { windowsVolumeWarning } from '../signal.js';
import { set, state, watch } from '../store.js';

const GROUPS = [
  ['asio', 'ASIO', 'Acceso directo al driver del DAC. Es la única vía para DSD nativo.'],
  ['exclusive', 'WASAPI exclusivo', 'Windows cede el dispositivo por completo. Nada se mezcla con la música.'],
  ['shared', 'WASAPI compartido', 'Convive con los demás programas. Windows mezcla, así que no es bit-perfect.'],
];

const KIND_NAMES = Object.fromEntries(GROUPS.map(([kind, title]) => [kind, title]));

const DSD_MODES = {
  auto: ['Automático', 'Usa DSD nativo si el dispositivo lo admite, luego DoP si lo has confirmado, y si no convierte a PCM.'],
  native: ['Nativo', 'Envía el flujo DSD tal cual. Falla con las tasas que el driver no admite.'],
  dop: ['DoP', 'Empaqueta el DSD dentro de muestras PCM. Si el DAC no lo entiende se oye ruido.'],
  pcm: ['Convertir a PCM', 'Filtra el DSD a PCM de alta resolución. Funciona con cualquier DAC.'],
};
const DSD_VOLUME_NOTE = 'Con el volumen por debajo de 0 dB, el DSD se convierte siempre a PCM: de otro modo sonaría a nivel completo.';

const REPLAY_GAIN_MODES = {
  off: ['Desactivado', 'Las pistas suenan al nivel con que se grabaron.'],
  track: ['Por pista', 'Iguala el nivel de cada pista con sus etiquetas ReplayGain. La pista deja de ser bit-perfect.'],
  album: ['Por álbum', 'Iguala el nivel entre álbumes y respeta las diferencias dentro de cada uno. La pista deja de ser bit-perfect.'],
};

const DOP_POLL_MS = 400;

const send = (method, params) => call(method, params).catch(reportError);

// Sends silence as DoP and asks what the DAC shows, which is the only way to know: neither ASIO nor WASAPI tell.
function dopTest(device) {
  const { key } = device;
  let timer = 0;
  const stop = (answer) => {
    clearTimeout(timer);
    return send('devices.stopDopTest', answer === undefined ? { key } : { key, confirmed: answer });
  };
  const finish = (answer, message) => {
    dialog.close();
    stop(answer).then(() => toast(message));
  };
  const asking = [
    { label: 'No, indica PCM o nada', onclick: () => finish(false, 'DoP queda desactivado para este dispositivo.') },
    { label: 'Sí, indica DSD', primary: true, onclick: () => finish(true, 'DoP confirmado para este dispositivo.') },
  ];
  const closing = [{ label: 'Cerrar', primary: true, onclick: () => { dialog.close(); stop(); } }];

  const follow = async () => {
    const test = await call('devices.dopTestStatus').catch(() => null);
    if (test?.error) {
      dialog.update('Prueba de DoP', [`No se pudo enviar DoP: ${test.error}`], closing);
      return;
    }
    if (test?.sending) {
      const lines = [`Se está enviando silencio DSD64 por DoP, a ${kilohertz(test.rate)} kHz en ${test.encoding}. No debería oírse nada.`,
        'Mira la pantalla del DAC. ¿Qué indica?'];
      if (test.windowsGainDb) {
        lines.push(`El volumen de Windows de este dispositivo está en ${decibels(test.windowsGainDb)}, y eso impide reconocer DoP. Ponlo al 100 % y repite la prueba.`);
      }
      dialog.update('Prueba de DoP', lines, asking);
    }
    timer = setTimeout(follow, DOP_POLL_MS);
  };

  const dialog = openDialog({
    title: 'Prueba de DoP',
    text: ['Se detendrá la música y se enviará silencio al DAC, empaquetado como DoP. Si no lo entiende puede oírse un ruido bajo, así que baja su volumen antes de empezar.'],
    actions: [
      { label: 'Cancelar', onclick: () => dialog.close() },
      {
        label: 'Empezar',
        primary: true,
        onclick: async () => {
          dialog.update('Prueba de DoP', ['Abriendo el dispositivo…'], []);
          try {
            await call('devices.startDopTest', { key });
            follow();
          } catch (error) {
            dialog.update('Prueba de DoP', [error.message], closing);
          }
        },
      },
    ],
    onclose: () => stop(),
  });
}

function describe(device) {
  if (device.error) return `No disponible: ${device.error}`;
  const parts = [];
  if (device.kind === 'asio') parts.push(count(device.channels, 'salida', 'salidas'));
  if (device.rates.length) parts.push(`hasta ${kilohertz(Math.max(...device.rates))} kHz`);
  if (device.dsdRates.length) parts.push(`hasta ${dsdName(Math.max(...device.dsdRates))}`);
  return parts.join(' · ');
}

function deviceRow(device, selected) {
  const usable = !device.error;
  return h('label.device', { class: `${selected ? 'selected' : ''} ${usable ? '' : 'unusable'}` },
    h('input', {
      type: 'radio',
      name: 'device',
      checked: selected,
      disabled: !usable,
      onchange: () => send('devices.select', { key: device.key }),
    }),
    h('span.device-text', h('span.device-name', device.name), h('span.device-detail', describe(device))));
}

function deviceOptions(device) {
  const { settings: chosen, key, kind } = device;
  const configure = (settings) => send('devices.configure', { key, settings });
  const modes = kind === 'asio' ? ['auto', 'native', 'dop', 'pcm'] : kind === 'exclusive' ? ['auto', 'dop', 'pcm'] : [];

  const rows = [];
  if (modes.length) {
    const active = modes.includes(chosen.dsd) ? chosen.dsd : 'auto';
    rows.push(h('div.setting',
      h('div.setting-text', h('strong', 'Reproducción de DSD'), h('p', `${DSD_MODES[active][1]} ${DSD_VOLUME_NOTE}`)),
      h('div.segments', { role: 'radiogroup' }, modes.map((mode) => h('button.segment', {
        type: 'button',
        class: mode === active ? 'on' : '',
        role: 'radio',
        ariaChecked: mode === active,
        onclick: () => configure({ dsd: mode }),
      }, DSD_MODES[mode][0])))));
    rows.push(h('div.setting',
      h('label.setting-text.dop-check',
        h('strong', 'Mi DAC reconoce DoP'),
        h('p', 'Ni ASIO ni WASAPI permiten detectarlo. Pruébalo mirando la pantalla del DAC.')),
      h('button.button.dop-test', { type: 'button', onclick: () => dopTest(device) }, 'Probar'),
      h('input', {
        type: 'checkbox',
        ariaLabel: 'Mi DAC reconoce DoP',
        checked: chosen.dopConfirmed,
        onchange: (event) => configure({ dopConfirmed: event.target.checked }),
      })));
  }

  const gain = h('output.setting-value', `${chosen.dsdGainDb > 0 ? '+' : ''}${chosen.dsdGainDb.toFixed(1)} dB`);
  rows.push(h('div.setting',
    h('div.setting-text',
      h('strong', 'Ganancia al convertir DSD a PCM'),
      h('p', 'El 0 dB de SACD queda 6 dB por debajo del máximo de PCM. Con más de +3 dB, los discos con picos altos pueden recortar.')),
    h('div.setting-range', gain, h('input.slider', {
      type: 'range',
      min: 0,
      max: 6,
      step: 0.5,
      value: chosen.dsdGainDb,
      ariaLabel: 'Ganancia',
      oninput: (event) => {
        gain.textContent = `+${Number(event.target.value).toFixed(1)} dB`;
      },
      onchange: (event) => configure({ dsdGainDb: Number(event.target.value) }),
    }))));

  if (kind === 'exclusive') {
    rows.push(h('div.setting',
      h('div.setting-text', h('strong', 'Periodo del búfer'), h('p', 'Si oyes cortes, prueba un valor mayor.')),
      select('Periodo', String(chosen.periodMs), [['0', 'Predeterminado (40 ms)'], ['10', '10 ms'], ['20', '20 ms'], ['100', '100 ms']],
        (value) => configure({ periodMs: Number(value) }))));
  }

  if (kind === 'asio') {
    const sizes = [];
    for (let size = device.bufferMin; size > 0 && size <= device.bufferMax; size *= 2) sizes.push(size);
    if (sizes.length) {
      rows.push(h('div.setting',
        h('div.setting-text', h('strong', 'Tamaño del búfer ASIO'), h('p', 'Si oyes cortes, prueba un valor mayor.')),
        select('Tamaño', String(chosen.bufferFrames), [
          ['0', `Del driver (${device.bufferPreferred} muestras)`],
          ...sizes.map((size) => [String(size), `${size} muestras`]),
        ], (value) => configure({ bufferFrames: Number(value) }))));
    }
    if (device.outputs.length > 2) {
      const pairs = [];
      for (let first = 0; first + 1 < device.outputs.length; first += 2) {
        pairs.push([String(first), `${device.outputs[first]} / ${device.outputs[first + 1]}`]);
      }
      rows.push(h('div.setting',
        h('div.setting-text', h('strong', 'Salidas'), h('p', 'El par de canales del dispositivo por el que sale la música.')),
        select('Salidas', String(chosen.firstChannel), pairs, (value) => configure({ firstChannel: Number(value) }))));
    }
  }
  return h('div.settings-card', rows);
}

function outputSection() {
  const { devices, status } = state;
  const selected = devices.list.find((device) => device.key === devices.selected);
  const busy = status && status.state !== 'stopped';

  const body = !devices.known
    ? [h('p.muted', 'Buscando dispositivos…')]
    : GROUPS.map(([kind, title, text]) => {
      const list = devices.list.filter((device) => device.kind === kind);
      if (!list.length) return null;
      return h('div.device-group',
        h('h3', title),
        h('p.muted', text),
        list.map((device) => deviceRow(device, device.key === devices.selected)));
    });

  return h('section.settings-section',
    h('div.section-head',
      h('h2', 'Salida de audio'),
      iconButton('refresh', busy ? 'Detén la música para volver a buscar dispositivos' : 'Buscar dispositivos', () => {
        send('devices.refresh').then((result) => {
          if (result?.busy) toast('Detén la música para volver a buscar dispositivos.');
          else toast('Buscando dispositivos…');
        });
      }, { size: 18 })),
    selected ? windowsVolumeWarning(selected.gainDb) : null,
    selected ? h('h3.selected-title', selected.name, h('span.selected-kind', KIND_NAMES[selected.kind])) : null,
    selected ? deviceOptions(selected) : null,
    body);
}

function playbackSection() {
  const active = REPLAY_GAIN_MODES[state.replayGain] ? state.replayGain : 'off';
  const choose = (mode) => {
    state.replayGain = mode;
    send('player.setReplayGain', { mode });
    set('replayGain', mode);
  };
  return h('section.settings-section',
    h('div.section-head', h('h2', 'Reproducción')),
    h('div.settings-card',
      h('div.setting',
        h('div.setting-text',
          h('strong', 'ReplayGain'),
          h('p', `${REPLAY_GAIN_MODES[active][1]} Las pistas sin etiquetas no cambian, y una ganancia nunca lleva el pico por encima del máximo.`)),
        h('div.segments', { role: 'radiogroup' }, Object.entries(REPLAY_GAIN_MODES).map(([mode, [label]]) => h('button.segment', {
          type: 'button',
          class: mode === active ? 'on' : '',
          role: 'radio',
          ariaChecked: mode === active,
          onclick: () => choose(mode),
        }, label))))));
}

function folderRow(folder) {
  return h('div.library-folder',
    icon('folder', 20),
    h('span.library-path', { title: folder.path }, folder.path),
    iconButton('delete', 'Quitar de la biblioteca', async () => {
      await send('library.removeFolder', { path: folder.path });
      toast('Carpeta quitada. Tus archivos siguen donde estaban.');
    }, { size: 18 }));
}

function scanStatus(library) {
  const share = library.total ? library.done / library.total : 0;
  return h('div.scan-status',
    h('p', library.total ? `Leyendo ${library.done.toLocaleString('es-ES')} de ${library.total.toLocaleString('es-ES')} archivos` : 'Buscando música…'),
    h('div.progress', h('i', { style: { width: `${share * 100}%` } })));
}

function librarySection() {
  const { folders, library } = state;
  const addFolder = async () => {
    try {
      await call('library.pickFolder');
    } catch (error) {
      reportError(error);
    }
  };

  return h('section.settings-section',
    h('div.section-head', h('h2', 'Biblioteca')),
    h('p.muted', 'Arpegio vigila estas carpetas y solo lee los archivos: nunca los modifica.'),
    h('div.settings-card',
      folders.length ? folders.map(folderRow) : h('p.muted.pad', 'Todavía no hay carpetas.'),
      h('div.library-actions',
        h('button.button.primary', { type: 'button', onclick: addFolder }, icon('add', 18), 'Añadir carpeta'),
        h('button.button', { type: 'button', disabled: !folders.length, onclick: () => send('library.rescan') }, 'Buscar cambios'),
        h('button.button', {
          type: 'button',
          disabled: !folders.length,
          title: 'Vuelve a leer todos los archivos, aunque no hayan cambiado',
          onclick: () => send('library.rescan', { force: true }),
        }, 'Releer todo'))),
    library?.scanning ? scanStatus(library) : null,
    library?.error ? h('div.notice.bad', icon('warning', 20), h('p', library.error)) : null,
    library?.warnings?.length
      ? h('details.warnings',
        h('summary', count(library.warnings.length, 'aviso del último escaneo', 'avisos del último escaneo')),
        h('ul', library.warnings.slice(0, 100).map((warning) => h('li', warning))))
      : null,
    h('div.settings-card',
      h('label.setting.check',
        h('div.setting-text',
          h('strong', 'Área multicanal de SACD'),
          h('p', 'Cataloga la capa multicanal de los discos que también tienen estéreo. Al cambiarlo se relee la biblioteca.')),
        h('input', {
          type: 'checkbox',
          checked: state.sacdMultichannel,
          onchange: (event) => {
            state.sacdMultichannel = event.target.checked;
            send('library.setSacdMultichannel', { enabled: event.target.checked });
          },
        }))));
}

export function settingsView(page) {
  page.append(pageHeader('Ajustes'), h('div.settings'));
  const body = page.querySelector('.settings');
  const draw = () => {
    const open = [...body.querySelectorAll('details[open]')].length > 0;
    body.replaceChildren(outputSection(), playbackSection(), librarySection(),
      h('section.settings-section.about', h('p.muted', 'Arpegio 0.1 · Software libre bajo licencia GPLv3')));
    if (open) body.querySelector('details')?.setAttribute('open', '');
  };
  draw();

  // Controls must not be replaced under the pointer, so a running scan only redraws its own progress.
  let wasScanning = state.library?.scanning;
  const stops = [
    watch('devices', draw),
    watch('folders', draw),
    watch('replayGain', draw),
    watch('library', (library) => {
      if (wasScanning !== library.scanning) draw();
      else if (library.scanning) body.querySelector('.scan-status')?.replaceWith(scanStatus(library));
      wasScanning = library.scanning;
    }),
  ];
  return () => stops.forEach((stop) => stop());
}
