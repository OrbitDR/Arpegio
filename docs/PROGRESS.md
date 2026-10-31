# Progreso de Arpegio

Punto de partida para retomar el desarrollo. Última sesión: 2026-10-31. El diseño y las decisiones técnicas están en [DESIGN.md](DESIGN.md).

## Dónde estamos

El motor de audio y la biblioteca funcionan desde línea de comandos con todos los formatos. La escucha real pasó con DSD nativo, SACD y WASAPI exclusivo, y el DAC reconoce DoP con el volumen de Windows al 100 %. Falta la interfaz.

| Fase | Estado |
|---|---|
| 1. Base del proyecto y detección de dispositivos | Hecha |
| 2. Motor PCM: WASAPI, ASIO, gapless, seek, pausa | Hecha, falta escuchar PCM directo |
| 3. DSD: DSF, DFF, SACD ISO, DST, nativo, DoP, conversión | Hecha, escucha superada; DoP reconocido, falta oírlo |
| 4. Biblioteca | Hecha |
| 5. Interfaz en WebView2 | **Siguiente** |
| 6. Cierre | Sin empezar |

El repositorio está versionado en la rama `main`, solo en local: todavía no tiene remoto. La identidad de git está configurada únicamente para este repositorio. El usuario dejó el control de versiones a criterio del asistente; publicar en un remoto sigue requiriendo su confirmación.

## Cómo retomar

1. Oír DoP con música, si aún no se hizo. La [prueba de escucha](#prueba-de-escucha) confirmó que el DAC lo reconoce, pero con el volumen físico al mínimo.
2. Confirmar la [decisión pendiente](#decisión-pendiente) sobre la interfaz.
3. Compilar y probar:

   ```powershell
   cd arpegio
   python scripts\fetch_deps.py     # solo si falta third_party\
   scripts\build.bat release test
   ```

4. Seguir con la interfaz.

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
# 5. DoP DSD128: requiere el Topping al 100 % en Windows. Si hay ruido, parar con Ctrl+C
.\arpegio-cli.exe play $jazz --output exclusive --device topping --dsd dop --seconds 30
```

| Prueba | Si falla, mirar primero |
|---|---|
| 1 o 2: ruido o silencio | `ring_frames_` en `src/output/asio_output.cpp`, que asume `bufferSize / 8` bytes por canal en DSD |
| 3: no arranca en 225 s | `seek_frame` en `src/decode/sacd_iso.cpp`, sin probar con disco real |
| 3: hueco o clic entre pistas | Los límites por código de tiempo en `next_frame`, mismo archivo |
| 4: pausas | Probar `--period-ms 20` y `--period-ms 100`, y mirar los `late` que imprime al terminar |
| 5: ruido o silencio, y el DAC marca PCM 352.8 | El volumen de `Altavoces (TOPPING USB DAC)` en Windows debe estar al 100 %. Se cambia con `control mmsys.cpl`: el icono de la barra de tareas controla la salida predeterminada, que puede ser otra |

Resultado del usuario el 2026-10-31, con el Topping DX1 II:

| Prueba | Resultado |
|---|---|
| 1. DSD128 nativo | Bien, y la pantalla del DAC coincide |
| 2. DSD256 nativo | Bien, y la pantalla del DAC coincide |
| 3. SACD ISO | Bien: arranca en 225 s y pasa de la pista 1 a la 2 sin hueco |
| 4. WASAPI exclusivo, DSD a PCM | Bien, sin pausas |
| 5. DoP DSD128 por WASAPI exclusivo | Con Windows al 77 %: **falla**, el DAC marca PCM 352.8 y solo hay un ruido bajo. Con Windows al 100 %: el DAC marca DSD128 |

En todas, 0 cortes y 0 eventos tardíos.

Causa del fallo de DoP: el volumen de `Altavoces (TOPPING USB DAC)` es de hardware y estaba a −4 dB. Altera las muestras incluso en WASAPI exclusivo, cambia los bytes de marcador y el DAC reproduce el flujo como PCM. Fue la misma prueba, con el mismo archivo y formato, la que pasó de PCM 352.8 a DSD128 al subirlo a 0 dB.

Con el volumen al 100 % y el físico al mínimo, el DAC mostró DSD en las 8 combinaciones probadas: DSD128 y DSD64, por ASIO y por WASAPI con S32, S32 de 24 bits y S24. Los contenedores no influyen. Solo se miró la pantalla: falta oír DoP con música.

Por eso `DeviceInfo::hardware_gain_db` lee ese volumen en WASAPI. Si no es neutro, el modo automático no usa DoP, ninguna ruta se declara bit-perfect y `arpegio-cli` avisa. `--wire s32-24|s24` fuerza el contenedor WASAPI para repetir estas comprobaciones.

## Verificado

Los detalles y cifras están en [DESIGN.md](DESIGN.md#verificación-hecha-el-2026-10-30).

- 88 pruebas automáticas sin fallos. La suite de 86 pasó 25 ejecuciones seguidas, y cinco fallos introducidos a propósito en la biblioteca fueron detectados.
- `LOOPBACK` del MiniFuse: coincidencia total a 24 bits en 44.1, 96 y 192 kHz.
- 11 FLAC reales coinciden con su MD5.
- DST idéntico a `libdstdec` en 18 000 frames, en 2 y 6 canales.
- Topping: WASAPI exclusivo, compartido y ASIO, PCM, DoP y DSD nativo hasta DSD256, sin eventos tardíos ni cortes, siempre con silencio o señal inaudible.
- APE, WavPack PCM, M4A, MP3, DFF sin comprimir, WAV y FLAC reales abren y decodifican.
- Biblioteca sobre la colección real: 5 184 pistas, 476 álbumes y 382 artistas. Las 5 184 pistas abren y se leen al principio, en medio y al final, también con las áreas multicanal de SACD. Reescanear sin cambios tarda 0.06 s y ningún archivo de la colección se modificó.
- 954 pistas CUE empiezan en su muestra exacta en FLAC, WavPack, APE y DSF.

## Implementado pero sin verificar

- Escucha de PCM directo: la prueba 4 solo cubre DSD convertido a PCM.
- Paso sin hueco en SACD: comprobado de oído en un disco y una transición, sin medir.
- DoP con música audible: el DAC lo reconoce, pero nadie lo ha escuchado todavía.
- DoP por ASIO con el volumen de Windows bajo: ASIO no consulta ese volumen, y no se sabe si le afecta.
- Aviso de volumen: la lógica está cubierta por pruebas, pero no se vio en pantalla con el Topping por debajo del 100 %. Se lee al listar dispositivos, no durante la reproducción.
- Reapertura tras un reset de ASIO o una desconexión USB (`reopen_output_if_needed`).
- Cambio de formato entre pistas en hardware real. Solo se probó con la salida simulada.
- DFF con DST y WavPack con DSD: no hay archivos así en la colección.
- AIFF y Ogg Vorbis: no hay archivos en `D:\Música`, aunque sí en otras carpetas del equipo.
- ALAC: TagLib identifica los 37 `.m4a` de la colección como AAC, así que no hay ningún ALAC real con el que probar que sea exacto.
- APE: se decodificaron 20 s, sin comprobar contra un hash.
- Mezcla de 6 canales a estéreo: sin prueba automática ni escucha.
- Calidad medida de r8brain en esta integración: solo corrió con silencio.
- Paso sin hueco entre pistas CUE en el reproductor. Los rangos están comprobados en los decodificadores y el paso sin hueco con la salida simulada, pero nunca juntos ni en hardware real.
- Vigilancia de carpetas sobre la colección real: solo se probó con carpetas temporales.
- Hoja CUE en el bloque `CUESHEET` nativo de FLAC: solo se lee la etiqueta del mismo nombre.

## Problema abierto

Una ejecución de `arpegio-library verify` sobre el catálogo multicanal se quedó parada, sin usar CPU, tras 33 s de CPU, y hubo que matarla. No se repitió en 17 ejecuciones posteriores, seis de ellas en las mismas condiciones, y las 5 184 pistas abren bien. No se encontró la causa, ni se sabe si fue en un decodificador o al cerrar el proceso. Si vuelve a ocurrir, capturar los hilos del proceso antes de matarlo.

## Limitaciones actuales del motor

- No hay volumen por software ni ReplayGain.
- `Player` reproduce por índice de cola, pero aún no tiene `next` ni `previous`.
- Periodo WASAPI de 40 ms y búfer ASIO preferido del driver, solo ajustables por línea de comandos.
- DoP automático depende de `--dop-confirmed`. Falta la prueba guiada y guardar ajustes por dispositivo.
- La precarga en RAM es el anillo: 20 s del formato de salida, con un mínimo de 32 MB.

## Limitaciones actuales de la biblioteca

- Mover o renombrar un archivo lo convierte en una pista nueva, y pierde favoritos y listas.
- Un archivo modificado sin cambiar tamaño ni fecha no se relee. `ScanOptions::force` o `rescan(true)` lo fuerzan.
- Cambiar `sacd_multichannel` requiere un reescaneo forzado para tener efecto.
- Un álbum repartido entre dos carpetas cuenta como dos, salvo que sean subcarpetas de disco.
- Los `.bin` de imagen de CD no se decodifican. Hay dos hojas así en la colección y sus pistas sueltas entran por separado.
- Las pistas sin etiquetas quedan como `Unknown Artist`: 93 en la colección. El nombre del archivo no se interpreta.
- `Library::remove_folder` es asíncrono: volver a añadir la misma carpeta antes de que termine da error de solapamiento.

## Siguiente trabajo

### Interfaz en WebView2

- Ejecutable propio en `src/app/`: ventana Win32 y WebView2, con `wrl.h` y sin ATL.
- Web en `ui/`, servida con `SetVirtualHostNameToFolderMapping`. Elegir framework al empezar.
- Mensajes JSON con nlohmann json por `PostWebMessageAsJson` y `WebMessageReceived`.
- Carátulas por `WebResourceRequested`.
- Estado desde `Player::status()`, consultado de 5 a 10 veces por segundo.
- Biblioteca con `Library`: `store()` para leer, `add_folder`, `remove_folder`, `rescan` y `status()`. `status().revision` crece cuando hay que refrescar las vistas. `make_queue_item` convierte una `Track` en un elemento de la cola, sea archivo, CUE o SACD. Las miniaturas salen de `artwork().thumbnail_path`.
- `Player` necesita `next` y `previous`, y la cola guardada con `set_queue` y `queue` debe restaurarse al arrancar.
- La base de datos y la caché de portadas deben ir en `%LOCALAPPDATA%\Arpegio`.
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
| `src/decode/` | Interfaz `Decoder` y un decodificador por formato; SACD en `sacd_iso`; rangos CUE en `range_decoder` |
| `src/engine/` | `render_plan` decide la ruta, `pipeline` convierte, `player` coordina |
| `src/output/` | ASIO, WASAPI e hilo STA |
| `src/formats/` | Lector DSF y escritor WAV |
| `src/library/` | `library` coordina; `library_store` es el catálogo en SQLite; `scanner` lee las carpetas; `metadata`, `artwork` y `cue_sheet` son sus piezas; `track_source` conecta con el motor |
| `cli/` | `arpegio-cli` y `arpegio-dsd2wav` |
| `tests/` | Pruebas automáticas, con una salida simulada en `test_player.cpp` y la biblioteca en `test_library.cpp` |
| `tools/` | Generador DSD, informes, `arpegio-library` y `measure/` |

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

Biblioteca sobre la colección real, con la base de datos fuera del repositorio:

```powershell
$db = "$env:TEMP\arpegio\biblioteca.db"
New-Item -ItemType Directory -Force (Split-Path $db)
.\arpegio-library.exe $db scan "D:\Música"
.\arpegio-library.exe $db stats
.\arpegio-library.exe $db verify     # abre todas las pistas y compara los inicios CUE
.\arpegio-library.exe $db scan "D:\Música" --multichannel --force
```

Señal inaudible para probar hardware: `arpegio-dsdgen` sin tonos, por ejemplo `.\arpegio-dsdgen.exe quiet64.dsf ntf64.txt 2822400 20`. Para otras tasas, generar antes el diseño con `design_ntf.py 128` o `256`.

## Entorno y trampas conocidas

- Windows 11 x64, i5-12600K, 32 GB. VS Build Tools 2022 sin ATL, Windows SDK 10.0.26100, CMake 4.4, Ninja, Node 24, Rust 1.99, Python 3.14.
- DAC Topping DX1 II y Arturia MiniFuse 2. Música en `D:\Música`.
- Una compilación fallida deja el binario anterior, y las pruebas corren sobre él. Hay que mirar la salida de compilación antes de fiarse del resultado.
- `scripts\build.bat` imprime un aviso de `vswhere`. Es inofensivo.
- Evitar parches largos de Python en línea desde Bash: se rompen. Es mejor escribir el script en un archivo.
- La descarga del binario de FFmpeg fue bloqueada por el control de permisos. FFmpeg no hace falta y no se debe reintentar.
- La colección real descubre lo que las pruebas sintéticas no: un `.flac` con ID3v2 delante cerraba el programa, y había híbridos `.iso.wv`, FLAC sin longitud en la cabecera y hojas con tiempos fuera de norma. Tras tocar decodificadores o escáner, repetir `scan` y `verify`.
- Windows sella los archivos con un reloj de unos 15 ms. Una prueba que reescribe un archivo del mismo tamaño justo después de escanearlo debe mover la fecha a mano.
- `third_party/` no se versiona. `scripts/fetch_deps.py` lo recrea con versiones y SHA-256 fijos.
