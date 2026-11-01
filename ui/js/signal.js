// The path the sound takes, stage by stage, in the terms an audio enthusiast expects to check.
import { h, icon } from './dom.js';
import { dsdName, formatLine, kilohertz } from './format.js';
import { selectedDevice } from './store.js';

const KIND_NAMES = { asio: 'ASIO', exclusive: 'WASAPI exclusivo', shared: 'WASAPI compartido' };

function processingLines(plan, track) {
  switch (plan.path) {
    case 'dsdNative': return ['Ninguno: el flujo DSD llega intacto'];
    case 'dop': return ['Empaquetado DoP, sin tocar los bits DSD'];
    default: break;
  }
  const lines = [];
  if (plan.path === 'dsdToPcm') {
    lines.push(`DSD a PCM: FIR ÷${plan.firDivider}, corte en ${kilohertz(plan.firCutoffHz)} kHz`);
    if (Math.abs(plan.gainDb) > 0.01) lines.push(`Ganancia de ${plan.gainDb > 0 ? '+' : ''}${plan.gainDb.toFixed(1)} dB`);
  }
  if (plan.resampleFrom) lines.push(`Remuestreo r8brain: ${kilohertz(plan.resampleFrom)} a ${kilohertz(plan.rate)} kHz`);
  if (plan.mixChannels) lines.push(`Mezcla de ${track?.channels ?? '?'} a ${plan.channels} canales`);
  if (plan.ditherBits) lines.push(`Dither TPDF a ${plan.ditherBits} bits`);
  return lines.length ? lines : ['Ninguno: las muestras llegan intactas'];
}

function outputLine(plan) {
  if (plan.path === 'dsdNative') return `${dsdName(plan.rate)} nativo · ${plan.channels} canales`;
  if (plan.path === 'dop') return `DoP sobre ${plan.encoding} · ${kilohertz(plan.rate)} kHz · ${plan.channels} canales`;
  const depth = plan.validBits && plan.validBits !== 32 ? `${plan.encoding}, ${plan.validBits} bits` : plan.encoding;
  return `${depth} · ${kilohertz(plan.rate)} kHz · ${plan.channels} canales`;
}

function stage(label, lines, state_) {
  return h('li.stage', { class: state_ ?? '' },
    h('span.stage-dot'),
    h('div.stage-label', label),
    lines.map((line) => h('div.stage-value', line)));
}

export function windowsVolumeWarning(gainDb) {
  if (!gainDb) return null;
  return h('div.notice.warn',
    icon('warning', 20),
    h('div',
      h('strong', `El volumen de Windows de este dispositivo está en ${gainDb.toFixed(1)} dB.`),
      h('p', 'Altera las muestras incluso en modo exclusivo: la salida deja de ser bit-perfect y el DAC no reconoce DoP. '
        + 'Ponlo al 100 % desde Configuración de sonido y usa el volumen del DAC.')));
}

export function signalPath(track, status) {
  const device = selectedDevice();
  const plan = status?.plan;
  if (!plan) {
    return h('section.signal',
      h('h2', 'Ruta de la señal'),
      h('ol.stages',
        track ? stage('Origen', [formatLine(track)]) : null,
        device ? stage('Salida', [KIND_NAMES[device.kind], device.name]) : stage('Salida', ['Sin dispositivo'], 'bad')),
      h('p.signal-idle', 'Al reproducir se muestra lo que recibe el dispositivo.'));
  }

  const lines = processingLines(plan, track);
  const untouched = plan.path === 'dsdNative' || plan.path === 'dop' || lines[0].startsWith('Ninguno');
  return h('section.signal',
    h('div.signal-head',
      h('h2', 'Ruta de la señal'),
      h('span.badge', { class: status.bitPerfect ? 'good' : 'warn' }, status.bitPerfect ? 'Bit-perfect' : 'Procesado')),
    h('ol.stages',
      stage('Origen', [track ? formatLine(track) : '']),
      stage('Proceso', lines, untouched ? 'good' : 'warn'),
      stage('Formato de salida', [outputLine(plan)]),
      stage('Dispositivo', device ? [KIND_NAMES[device.kind], device.name] : ['Desconocido'])),
    windowsVolumeWarning(status.windowsGainDb),
    h('dl.metrics',
      metric('Búfer', `${Math.round(status.buffer * 100)} %`),
      metric('Cortes', status.underruns, status.underruns > 0),
      metric('Eventos tardíos', status.late, status.late > 0),
      metric('Muestras recortadas', status.clipped, status.clipped > 0)));
}

function metric(label, value, bad = false) {
  return h('div.metric', { class: bad ? 'bad' : '' }, h('dt', label), h('dd', String(value)));
}
