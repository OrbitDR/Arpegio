# Progreso de Arpegio

Punto de partida para retomar el desarrollo. Última sesión: 2026-10-30. El diseño y las decisiones técnicas están en [DESIGN.md](DESIGN.md).

## Dónde estamos

El motor de audio funciona desde línea de comandos con todos los formatos. Falta la escucha real, la biblioteca y la interfaz.

| Fase | Estado |
|---|---|
| 1. Base del proyecto y detección de dispositivos | Hecha |
| 2. Motor PCM: WASAPI, ASIO, gapless, seek, pausa | Hecha, falta escucha |
| 3. DSD: DSF, DFF, SACD ISO, DST, nativo, DoP, conversión | Hecha, falta escucha |
| 4. Biblioteca | **Siguiente** |
| 5. Interfaz en WebView2 | Sin empezar |
| 6. Cierre | Sin empezar |

El repositorio está versionado en la rama `main`, solo en local: todavía no tiene remoto. La identidad de git está configurada únicamente para este repositorio. Los commits se hacen solo si el usuario los pide.

## Cómo retomar

1. Pedir al usuario el resultado de la [prueba de escucha](#prueba-de-escucha), si aún no lo dio. Es el primer paso porque un fallo ahí va antes que cualquier trabajo nuevo.
2. Confirmar la [decisión pendiente](#decisión-pendiente) sobre la interfaz.
3. Compilar y probar:

   ```powershell
   cd arpegio
   python scripts\fetch_deps.py     # solo si falta third_party\
   scripts\build.bat release test
   ```

4. Seguir con la biblioteca.

## Decisión pendiente

Se recomendó WebView2 sin saber que `AuroraPlayer` ya tenía una interfaz Qt/QML aprobada por el usuario. El usuario debe decir si sigue con WebView2 o reutiliza esa interfaz. La elección cambia cuánto trabajo queda, no la calidad de audio. Si no responde, se sigue con WebView2, que fue la opción que eligió.

## Prueba de escucha

Cerrar foobar2000 y Audirvana, y bajar el volumen del DAC: en DSD nativo y DoP no hay volumen por software.

```powershell
cd build\release
$m = 'D:\Música'
$jazz = "$m\pista-dsd128.dsf"
$iso = "$m\disco-sacd.iso"

# 1. DSD128 nativo: la pantalla debe mostrar DSD
.\arpegio-cli.exe play $jazz --output asio --device topping --seconds 40
# 2. DSD256 nativo
.\arpegio-cli.exe play "$m\pista-dsd256.dsf" --output asio --device topping --seconds 40
# 3. SACD ISO: el paso de la pista 1 a la 2 no debe tener hueco
.\arpegio-cli.exe play $iso --output asio --device topping --start 225 --seconds 30
# 4. WASAPI exclusivo: no debe haber pausas
.\arpegio-cli.exe play $jazz --output exclusive --device topping --dsd pcm --seconds 40
# 5. DoP DSD128, que fallaba en AuroraPlayer: si hay ruido, parar con Ctrl+C
.\arpegio-cli.exe play $jazz --output exclusive --device topping --dsd dop --seconds 30
```

| Prueba | Si falla, mirar primero |
|---|---|
| 1 o 2: ruido o silencio | `ring_frames_` en `src/output/asio_output.cpp`, que asume `bufferSize / 8` bytes por canal en DSD |
| 3: no arranca en 225 s | `seek_frame` en `src/decode/sacd_iso.cpp`, sin probar con disco real |
| 3: hueco o clic entre pistas | Los límites por código de tiempo en `next_frame`, mismo archivo |
| 4: pausas | Probar `--period-ms 20` y `--period-ms 100`, y mirar los `late` que imprime al terminar |
| 5: ruido o silencio | En AuroraPlayer falló igual: ruido por ASIO, silencio por WASAPI. DSD64 por DoP sí sonaba. Audirvana informa DoP 1.1 hasta DSD128 en este DAC. El nativo por ASIO sigue siendo la ruta principal |

## Verificado

Los detalles y cifras están en [DESIGN.md](DESIGN.md#verificación-hecha-el-2026-10-30).

- 41 pruebas automáticas sin fallos.
- `LOOPBACK` del MiniFuse: coincidencia total a 24 bits en 44.1, 96 y 192 kHz.
- 11 FLAC reales coinciden con su MD5.
- DST idéntico a `libdstdec` en 18 000 frames, en 2 y 6 canales.
- Topping: WASAPI exclusivo, compartido y ASIO, PCM, DoP y DSD nativo hasta DSD256, sin eventos tardíos ni cortes, siempre con silencio o señal inaudible.
- APE, WavPack PCM, M4A, MP3, DFF sin comprimir, WAV y FLAC reales abren y decodifican.

## Implementado pero sin verificar

- Toda la escucha real.
- Seek en SACD ISO con disco real y paso sin hueco entre sus pistas.
- Reapertura tras un reset de ASIO o una desconexión USB (`reopen_output_if_needed`).
- Cambio de formato entre pistas en hardware real. Solo se probó con la salida simulada.
- DFF con DST y WavPack con DSD: no hay archivos así en la colección.
- AIFF y Ogg Vorbis: no hay archivos en `D:\Música`, aunque sí en otras carpetas del equipo.
- ALAC: se decodificó un `.m4a`, sin confirmar si era ALAC o AAC. Falta comprobar que ALAC sea exacto.
- APE: se decodificaron 20 s, sin comprobar contra un hash.
- Mezcla de 6 canales a estéreo: sin prueba automática ni escucha.
- Calidad medida de r8brain en esta integración: solo corrió con silencio.

## Limitaciones actuales del motor

- No hay volumen por software ni ReplayGain.
- `Player` reproduce por índice de cola, pero aún no tiene `next` ni `previous`.
- Periodo WASAPI de 40 ms y búfer ASIO preferido del driver, solo ajustables por línea de comandos.
- DoP automático depende de `--dop-confirmed`. Falta la prueba guiada y guardar ajustes por dispositivo.
- La precarga en RAM es el anillo: 20 s del formato de salida, con un mínimo de 32 MB.

## Siguiente trabajo

### Biblioteca

Portar desde `AuroraPlayer`, sin modificar ese proyecto:

| Origen en `src/core/` | Uso |
|---|---|
| `library/SqliteDatabase`, `LibraryStore` | Base de datos con migraciones y FTS5 |
| `library/LibraryScanner`, `FolderWatcher` | Importación y carpetas vigiladas |
| `library/MetadataReader`, `ArtworkCache`, `PathUtil` | Etiquetas con TagLib y carátulas |
| `cue/CueSheet`, `cue/CueTrackDecoder` | CUE con INDEX, PREGAP y varios archivos |

También sirven `docs/SCHEMA.sql` y las partes de biblioteca de `tests/test_core.cpp`. Ese código se probó con la colección real: 5 071 pistas, 485 álbumes y 739 artistas.

Cambios necesarios:

- Pasar del espacio de nombres `aurora` a `arpegio`.
- Adaptar `CueTrackDecoder` a la interfaz `Decoder`, incluidos rangos DSD en bytes por canal. Hace falta para los APE y WavPack de un solo archivo con CUE.
- Hacer que un `.iso` SACD genere una pista por cada entrada de `read_sacd_disc`, usando `QueueItem::open`. Decidir cómo elegir entre área estéreo y multicanal: el valor por defecto debe ser estéreo.
- Descartar los `.iso` que no sean SACD con `is_sacd_image`. La colección tiene 4, entre ellos una imagen de Windows.
- Compilar TagLib, SQLite y utfcpp, ya descargados en `third_party/`.
- AuroraPlayer guardó duraciones DSD erróneas antes de corregir un fallo, así que no hay que reutilizar su base de datos: conviene reimportar.

### Interfaz en WebView2

- Ejecutable propio en `src/app/`: ventana Win32 y WebView2, con `wrl.h` y sin ATL.
- Web en `ui/`, servida con `SetVirtualHostNameToFolderMapping`. Elegir framework al empezar.
- Mensajes JSON con nlohmann json por `PostWebMessageAsJson` y `WebMessageReceived`.
- Carátulas por `WebResourceRequested`.
- Estado desde `Player::status()`, consultado de 5 a 10 veces por segundo.
- Vistas: artistas, álbumes, carpetas, pistas, cola, reproducción con ruta de señal, y ajustes por dispositivo.

Si el usuario prefiere Qt, se reutiliza `src/ui/` de AuroraPlayer y se cambia `AudioEngine` por `Player` en `PlaybackController`.

### Cierre

- Prueba guiada de DoP con silencio DSD, mirando la pantalla del DAC.
- Ajustes guardados por dispositivo.
- Volumen por software y ReplayGain.
- libsoxr como segunda opción de remuestreo. Ya está descargado.
- Instalador.
- Opcional: Kernel Streaming, que Audirvana ofrece y el usuario no pidió.

## Mapa del código

| Carpeta | Contenido |
|---|---|
| `src/audio/` | Formatos de dispositivo, anillo sin bloqueos, cuantización y dither |
| `src/dsd/` | Tabla DSD a PCM, filtro FIR, DoP, decodificador DST |
| `src/decode/` | Interfaz `Decoder` y un decodificador por formato; SACD en `sacd_iso` |
| `src/engine/` | `render_plan` decide la ruta, `pipeline` convierte, `player` coordina |
| `src/output/` | ASIO, WASAPI e hilo STA |
| `src/formats/` | Lector DSF y escritor WAV |
| `cli/` | `arpegio-cli` y `arpegio-dsd2wav` |
| `tests/` | Pruebas automáticas, con una salida simulada en `test_player.cpp` |
| `tools/` | Generador DSD, informes y `measure/` |

## Reproducir las verificaciones

Los scripts de `tools/measure/` necesitan Python con numpy, scipy y soundfile. El entorno usado estaba en una carpeta temporal, así que puede hacer falta crearlo otra vez:

```powershell
python -m venv ..\arpegio-venv
..\arpegio-venv\Scripts\pip install numpy scipy soundfile
```

Desde `build\release`, con `$py` apuntando a ese Python y `$t` a `..\..\tools\measure`:

```powershell
# Filtro DSD frente al de FFmpeg
& $py $t\design_ntf.py 64 > ntf64.txt
.\arpegio-dsdgen.exe tone1k.dsf ntf64.txt 2822400 12 1000:-6.0206
& $py $t\ffmpeg_taps.py > ffmpeg_taps.txt
.\arpegio-dsd2wav.exe tone1k.dsf ours.wav --rate 176400
.\arpegio-dsd2wav.exe tone1k.dsf ffmpeg.wav --taps ffmpeg_taps.txt --divider 8
& $py $t\dsd_quality.py --tone 1000 ours.wav ffmpeg.wav

# Bit-perfect por LOOPBACK, sin salir por las salidas físicas
& $py $t\make_noise.py .
.\arpegio-cli.exe loopback noise_96000_24.wav --device minifuse --first-channel 2 --capture-channel 2

# FLAC contra su MD5
& $py $t\verify_flac.py .\arpegio-cli.exe "pista.flac"

# DST contra libdstdec: primero tools\measure\dst_compare.bat
.\dst_compare.exe "disco.iso" 0 0
```

Señal inaudible para probar hardware: `arpegio-dsdgen` sin tonos, por ejemplo `.\arpegio-dsdgen.exe quiet64.dsf ntf64.txt 2822400 20`. Para otras tasas, generar antes el diseño con `design_ntf.py 128` o `256`.

## Entorno y trampas conocidas

- Windows 11 x64, i5-12600K, 32 GB. VS Build Tools 2022 sin ATL, Windows SDK 10.0.26100, CMake 4.4, Ninja, Node 24, Rust 1.99, Python 3.14.
- DAC Topping DX1 II y Arturia MiniFuse 2. Música en `D:\Música`.
- Una compilación fallida deja el binario anterior, y las pruebas corren sobre él. Hay que mirar la salida de compilación antes de fiarse del resultado.
- `scripts\build.bat` imprime un aviso de `vswhere`. Es inofensivo.
- Evitar parches largos de Python en línea desde Bash: se rompen. Es mejor escribir el script en un archivo.
- La descarga del binario de FFmpeg fue bloqueada por el control de permisos. FFmpeg no hace falta y no se debe reintentar.
- `third_party/` no se versiona. `scripts/fetch_deps.py` lo recrea con versiones y SHA-256 fijos.
