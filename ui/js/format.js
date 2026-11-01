// Numbers as people read them.
const collator = new Intl.Collator('es', { numeric: true, sensitivity: 'base' });
export const compareText = collator.compare;

export function time(seconds) {
  const total = Math.max(0, Math.floor(seconds || 0));
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor(total / 60) % 60;
  const rest = String(total % 60).padStart(2, '0');
  return hours ? `${hours}:${String(minutes).padStart(2, '0')}:${rest}` : `${minutes}:${rest}`;
}

export function longTime(seconds) {
  const minutes = Math.round((seconds || 0) / 60);
  if (minutes < 60) return `${minutes} min`;
  return `${Math.floor(minutes / 60)} h ${minutes % 60} min`;
}

export function count(number, one, many) {
  return `${number.toLocaleString('es-ES')} ${number === 1 ? one : many}`;
}

export function kilohertz(rate) {
  return (rate / 1000).toFixed(rate % 1000 ? 1 : 0).replace(/\.0$/, '');
}

export function dsdName(rate) {
  for (const base of [44100, 48000]) {
    if (rate % base === 0) return `DSD${rate / base}`;
  }
  return 'DSD';
}

const LOSSY = new Set(['MP3', 'AAC', 'Vorbis', 'WMA']);

export function isLossy(item) {
  return LOSSY.has(item.codec);
}

// The label on a cover. CD quality carries none, so the ones that appear mean something.
export function qualityChip(item) {
  if (item.mqa) return 'MQA';
  if (item.dsd) return dsdName(item.rate);
  if (isLossy(item) || !item.rate) return '';
  if (item.bits > 16 || item.rate > 48000) return `${item.bits}/${kilohertz(item.rate)}`;
  return '';
}

export function formatLine(track) {
  const channels = track.channels === 1 ? 'mono' : track.channels === 2 ? 'estéreo' : `${track.channels} canales`;
  if (track.dsd) return `${track.codec} · ${dsdName(track.rate)} · ${(track.rate / 1e6).toFixed(4)} MHz · ${channels}`;
  const depth = isLossy(track) ? `${track.bitrate} kbit/s` : `${track.bits} bits`;
  return `${track.mqa ? `${track.codec} MQA` : track.codec} · ${depth} · ${kilohertz(track.rate)} kHz · ${channels}`;
}

export function shortFormat(track) {
  if (track.dsd) return dsdName(track.rate);
  if (isLossy(track)) return `${track.codec} ${track.bitrate}`;
  return `${track.mqa ? 'MQA ' : ''}${track.bits}/${kilohertz(track.rate)}`;
}
