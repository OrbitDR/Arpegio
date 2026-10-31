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
2. DoP, por ASIO o WASAPI exclusivo, si el usuario confirmó que su DAC lo entiende. Ni ASIO ni WASAPI permiten detectarlo. Por WASAPI exige además que el volumen de hardware esté en 0 dB.
3. Conversión a PCM. WASAPI compartido siempre usa esta ruta.

El volumen de hardware del endpoint llega a las muestras incluso en modo exclusivo. En el Topping DX1 II, a −4 dB el DAC deja de reconocer DoP y lo reproduce como PCM, con un ruido bajo. Por eso cualquier valor distinto de 0 dB también impide declarar bit-perfect una salida WASAPI. La escucha debe hacerse con Windows al 100 % y el volumen físico del DAC.

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

Un archivo se identifica por su contenido antes que por su extensión. Dos casos reales de la colección obligan a mirar más allá del byte 0:

- Etiquetas ID3v2 delante de un FLAC: el formato real se busca detrás de la etiqueta.
- Híbridos `.iso.wv`: una imagen ISO 9660 cuya carga es WavPack. Con esa extensión se entregan a libwavpack, que encuentra el flujo por sí mismo.

## Biblioteca

Código en `src/library/`. La aplicación usa `Library`; lo demás son sus piezas.

```
FolderWatcher ──> Library ──> hilo de escaneo ──> scan_folder ──> LibraryStore ──> SQLite
                     │                                 │
                     └ store(): lectura en otro hilo   └ metadata (TagLib), artwork (WIC), cue_sheet
```

- **Solo lectura sobre la música.** TagLib abre un flujo de solo lectura, así que escanear no puede modificar ningún archivo. Base de datos y portadas viven aparte.
- **Una conexión por hilo, en modo WAL.** Los archivos se leen fuera de cualquier transacción y se escriben por lotes de 128 pistas, de modo que navegar sigue fluido durante un escaneo.
- **Escaneo incremental.** Un archivo sin cambios de tamaño y fecha no se vuelve a leer. Una hoja CUE se relee si cambia ella o cualquiera de sus archivos de audio.
- **Una carpeta inaccesible no borra nada.** Si la carpeta no se puede listar, el escaneo falla sin tocar el catálogo, para que un disco desconectado no parezca una biblioteca eliminada. Un escaneo cancelado conserva lo escrito y no elimina nada.
- **Identidad de pista: carpeta, ruta y parte.** La parte es 0 para un archivo entero, y el número de pista para CUE y SACD. Así una pista conserva su id, y con él favoritos y listas, al reescanear.
- **Identidad de álbum: título y carpeta.** Ni el año ni el artista sirven: varían entre las pistas de un recopilatorio, y un artista puede tener varios discos homónimos. Las subcarpetas `CD1`, `Disc 2` y similares cuentan como su carpeta padre. Dos ediciones de un disco en carpetas distintas son dos álbumes.
- **Crédito del álbum: mayoría.** Es el artista que las etiquetas nombran en más de la mitad de las pistas; si ninguno llega, `Various Artists`. El año es el más reciente de sus pistas. La portada, la de la primera pista que tenga.
- **Rangos CUE en unidades del decodificador.** Frames, o bytes por canal en DSD. `make_range_decoder` presenta el trozo como una fuente más, y dos pistas contiguas del mismo formato siguen en el mismo anillo sin hueco.
- **Hojas CUE tolerantes.** El nombre de `FILE` puede ser el `.wav` original aunque el archivo sea ya `.flac`, o el temporal del ripeador. Si dos hojas cubren un audio, gana la primera por ruta. También se lee la hoja incrustada en la etiqueta `CUESHEET`. Se aceptan tiempos con más de 74 frames, porque rechazar la hoja deja el álbum como una sola pista.
- **SACD.** Una pista por entrada de la tabla de contenidos. Se cataloga el área estéreo salvo que `ScanOptions::sacd_multichannel` pida la multicanal. Un `.iso` que no sea SACD se ignora.
- **Búsqueda con FTS5.** `rowid` es el id de pista y un disparador borra sus filas, incluso en borrados en cascada. Ignora acentos y busca por prefijo. Lo que el tokenizador descarta, como el título `÷`, cae en `LIKE`.
- **Portadas con Windows Imaging Component,** sin dependencias. Original y miniatura JPEG de 512 px, direccionados por contenido. Orden: imagen incrustada, nombre habitual en la carpeta, imagen única en la carpeta, y lo mismo en subcarpetas como `Covers` o `Artwork`. Con varias imágenes sin nombre reconocible no se adivina.
- **Carpetas vigiladas.** `ReadDirectoryChangesW`, con 2 s de calma antes de reescanear, para que copiar un álbum dé un solo escaneo y no se lean archivos a medio escribir.
- **Carpetas solapadas rechazadas.** Una carpeta dentro de otra listaría los mismos archivos dos veces.

## Verificación hecha el 2026-10-30

- 41 pruebas automáticas, sin fallos en ejecuciones repetidas. Cubren gapless exacto, seek, DSD nativo intacto y DoP continuo.
- Por `LOOPBACK` del MiniFuse por ASIO: 44.1, 96 y 192 kHz coinciden al 100 % a 24 bits, sin muestras perdidas. La diferencia máxima es de 7 unidades en escala de 32 bits, por debajo del bit 24, y parece venir de la mezcla interna del driver.
- FLAC: 11 archivos reales de 16 y 24 bits coinciden con su MD5 interno.
- DST: 18 000 frames reales de dos discos, en 2 y 6 canales, idénticos a `libdstdec`. Velocidad de 17× en estéreo y 6× en 6 canales.
- Topping por WASAPI exclusivo: 44.1, 176.4 y 352.8 kHz sin eventos tardíos, hueco máximo de 42 ms con periodo de 40 ms.
- Topping por ASIO: PCM, DSD64, DSD128 y DSD256 nativos, y DoP, sin eventos tardíos ni cortes.

Estas pruebas en el Topping usaron silencio o señal inaudible. La escucha con música real se hizo el 2026-10-31 y sus resultados están en [PROGRESS.md](PROGRESS.md#prueba-de-escucha).

## Verificación de la biblioteca, 2026-10-31

Sobre `D:\Música`, con la colección en el estado de ese día:

- 4 280 archivos dan 5 184 pistas, 476 álbumes y 382 artistas, 353.9 horas. Ningún archivo de la colección cambió.
- Escaneo completo en 13.4 s con la caché del sistema caliente, y 0.06 s si nada cambió.
- `arpegio-library verify` abre las 5 184 pistas y lee al principio, en medio y al final de cada una, sin fallos. Incluye 1 239 pistas CUE y 110 SACD.
- 954 pistas CUE empiezan en su muestra exacta, comparadas con una lectura secuencial del archivo: 870 en FLAC, 57 en WavPack, 18 en APE y 9 en DSF.
- `PRAGMA integrity_check` y `foreign_key_check` limpios, con una fila de búsqueda por pista.
- 4 992 pistas con portada. Los 24 álbumes sin ella son, sobre todo, carpetas sin imágenes o con varias sin nombre reconocible.

AuroraPlayer daba 5 071 pistas, 485 álbumes y 739 artistas. Las diferencias son los híbridos `.iso.wv`, los FLAC y hojas CUE que antes fallaban, los compositores, que ya no cuentan como artistas, y los álbumes que antes se partían.

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
