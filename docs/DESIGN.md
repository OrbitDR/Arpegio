# Diseño de Arpegio

Reproductor de música para Windows x64 con motor C++ e interfaz web en WebView2. La referencia funcional es Audirvana. El nombre es provisional. Licencia GPLv3.

## Prioridad

La calidad de audio manda. Calidad aquí significa lo que se puede medir: salida bit-perfect, cero cortes y ruido bajo. No se promete mejora audible por técnicas que no cambian los datos, como precargar en RAM.

## Arquitectura

```
Decoder ──> Pipeline ──> RingBuffer ──> FrameSource::pull ──> Output (WASAPI o ASIO)
  │            │             │                  │
  └ hilo de carga: disco, DSP, cuantización     └ hilo de tiempo real: solo copia bytes
```

- **Un proceso de 64 bits.** Los dos drivers ASIO del equipo tienen versión de 64 bits, así que no hace falta el host separado del plan de foobar2000.
- **El anillo guarda bytes finales del dispositivo.** Toda conversión ocurre antes, en el hilo de carga. El callback no decodifica, no reserva memoria y no toma bloqueos.
- **Doubles como formato intermedio de PCM.** Un double representa exacto cualquier entero de 32 bits, así que la ruta sin procesado conserva cada muestra. Lo comprueban `tests/test_quantizer.cpp` y `tools/measure/verify_flac.py`.
- **Sesión por formato.** Si dos pistas producen el mismo `RenderPlan`, siguen en el mismo anillo sin hueco. Si cambia el formato de dispositivo, el anillo se drena y el dispositivo se reabre.
- **Marcadores DoP en tiempo real.** Los pone `FrameSource::pull`, no el cargador, para que la secuencia 05/FA siga continua tras un seek o durante el silencio.
- **Pausa y seek sin parar el dispositivo.** Se emite el silencio del formato: `0` en PCM, `0x69` en DSD. Tras un seek hay silencio limpio hasta recargar 200 ms.

## Salidas

- **ASIO:** interfaces oficiales del SDK de Steinberg (`asio.h`, `asiosys.h`, `iasiodrv.h`) con `IASIO*` directo. No se compilan `asio.cpp` ni `asiodrivers.cpp`: descargan el driver activo al consultar otro y truncan nombres ANSI. Todas las llamadas ocurren en un hilo STA propio, porque los drivers son `ThreadingModel=Apartment`.
- **WASAPI exclusivo:** por eventos. Cada evento llena el búfer completo y `GetCurrentPadding` no se consulta, como pide Microsoft. Periodo por defecto de 40 ms.
- **WASAPI compartido:** formato de mezcla de Windows. El remuestreo lo hace r8brain, no Windows.

## DSD

Orden automático:

1. Nativo, solo por ASIO y si el driver lo declara con `kAsioCanDoIoFormat`.
2. DoP, por ASIO o WASAPI exclusivo, si el usuario confirmó que su DAC lo entiende. Ni ASIO ni WASAPI permiten detectarlo.
3. Conversión a PCM. WASAPI compartido siempre usa esta ruta.

| Origen | PCM preferido | División | Corte −3 dB | Si el DAC no llega |
|---|---|---|---|---|
| DSD64 | 176.4 kHz | ÷16 | 50 kHz | 88.2, 44.1 |
| DSD128 | 352.8 kHz | ÷16 | 70 kHz | 176.4, 88.2, 44.1 |
| DSD256 | 352.8 kHz | ÷32 | 90 kHz | 176.4, 88.2, 44.1 |
| DSD512 | 352.8 kHz | ÷64 | 90 kHz | 176.4, 88.2, 44.1 |

- La familia ×48 usa 192, 384, 96 y 48 kHz.
- El corte se limita al 40 % de la tasa de salida.
- Por debajo de 88.2 kHz, o sin tasas de la misma familia, el FIR entrega 176.4 kHz o más y r8brain completa el resto.
- Ganancia de +3 dB, porque el 0 dB de SACD está 6 dB bajo el máximo PCM y +6 dB recorta discos con picos altos.
- Filtro: FIR de fase lineal en 64 bits, Kaiser, 160 dB de rechazo, plano hasta 24 kHz. Es corto, unos 120 µs de energía útil en DSD64, y casi no resuena.
- Dither TPDF a 24 bits al salir.

## Formatos

| Formato | Decodificador |
|---|---|
| FLAC, Ogg FLAC | libFLAC |
| WAV, RF64, AIFF, AIFF-C | propios |
| APE | SDK oficial de Monkey's Audio |
| WavPack, incluido DSD | libwavpack |
| MP3 | minimp3, con gapless de LAME |
| Ogg Vorbis | libvorbisfile |
| AAC, ALAC, WMA | Media Foundation |
| DSF, DFF | propios |
| SACD ISO | propio, con DST |

DST es un port de `dstdec.c` de FFmpeg. FFmpeg no se usa como dependencia: la versión estable 9.0.2 no entrega DSD crudo desde DST.

MQA queda fuera porque es propietario y requiere licencia.

## Verificación hecha el 2026-10-30

- 41 pruebas automáticas, sin fallos en ejecuciones repetidas. Cubren gapless exacto, seek, DSD nativo intacto y DoP continuo.
- Por `LOOPBACK` del MiniFuse por ASIO: 44.1, 96 y 192 kHz coinciden al 100 % a 24 bits, sin muestras perdidas. La diferencia máxima es de 7 unidades en escala de 32 bits, por debajo del bit 24, y parece venir de la mezcla interna del driver.
- FLAC: 11 archivos reales de 16 y 24 bits coinciden con su MD5 interno.
- DST: 18 000 frames reales de dos discos, en 2 y 6 canales, idénticos a `libdstdec`. Velocidad de 17× en estéreo y 6× en 6 canales.
- Topping por WASAPI exclusivo: 44.1, 176.4 y 352.8 kHz sin eventos tardíos, hueco máximo de 42 ms con periodo de 40 ms.
- Topping por ASIO: PCM, DSD64, DSD128 y DSD256 nativos, y DoP, sin eventos tardíos ni cortes.

Estas pruebas en el Topping usaron silencio o señal inaudible. **Falta la escucha con música real.**

## Medición del filtro DSD frente al de FFmpeg

Se aplicaron los coeficientes exactos del filtro de 96 tomas de FFmpeg dentro de `arpegio-dsd2wav`, no el binario de FFmpeg, y no se midió su segunda etapa `swresample`.

| Prueba | Banda | FFmpeg | Arpegio |
|---|---|---|---|
| Tono 1 kHz, DSD64 sintético | 20 Hz–20 kHz | −143.3 dBFS | −143.3 dBFS |
| | 50–100 kHz | −25.5 dBFS | −60.5 dBFS (50–88 kHz) |
| Música DSD128 | 20 kHz–50 kHz | igual | igual |
| | 50–100 kHz | −63.5 dBFS | −78.4 dBFS |
| | 100 kHz en adelante | −29.5 dBFS | −106.1 dBFS |
| Música DSD256 | 50–88 kHz o 50–100 kHz | −49.8 dBFS | −54.1 dBFS |
| | 200 kHz en adelante | −30.3 dBFS | no existe a 176.4 kHz |

FFmpeg siempre entrega DSD/8, es decir 352.8, 705.6 o 1411.2 kHz, y las dos últimas requieren otra conversión.

Reproducir la medición: `tools/measure/`.

## Pendiente

El estado, la prueba de escucha y el trabajo siguiente están en [PROGRESS.md](PROGRESS.md).
