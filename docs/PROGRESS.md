# Progreso de Arpegio

Punto de partida para retomar el desarrollo. Última sesión: 2026-11-01. El diseño y las decisiones técnicas están en [DESIGN.md](DESIGN.md).

## Dónde estamos

La aplicación `arpegio.exe` funciona de punta a punta: biblioteca, cola, reproducción y ajustes por dispositivo, con la colección real y por las salidas reales. Toda la verificación de la interfaz se hizo con silencio digital, así que falta que el usuario la escuche con música.

| Fase | Estado |
|---|---|
| 1. Base del proyecto y detección de dispositivos | Hecha |
| 2. Motor PCM: WASAPI, ASIO, gapless, seek, pausa | Hecha |
| 3. DSD: DSF, DFF, SACD ISO, DST, nativo, DoP, conversión | Hecha |
| 4. Biblioteca | Hecha |
| 5. Interfaz en WebView2 | Hecha, pendiente de escucha |
| 6. Cierre | **Siguiente** |

El repositorio está en la rama `main` y se publica como repositorio público de GitHub. La identidad de git está configurada únicamente para este repositorio, con el correo `noreply` de GitHub. El usuario dejó el control de versiones a criterio del asistente.

Como es público, ningún archivo ni mensaje de commit debe llevar rutas personales, el correo real ni nombres de la colección: en estos documentos, `D:\Música`, `pista-dsd128.dsf` y similares son ejemplos, no rutas reales.

## Cómo retomar

1. Compilar y probar:

   ```powershell
   cd arpegio
   python scripts\fetch_deps.py     # solo si falta third_party\
   scripts\build.bat release test
   ```

2. Abrir `build\release\arpegio.exe`. Sus datos están en `%LOCALAPPDATA%\Arpegio`.
3. Seguir con el [cierre](#cierre).

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

Con el volumen al 100 % y el físico al mínimo, el DAC mostró DSD en las 8 combinaciones probadas: DSD128 y DSD64, por ASIO y por WASAPI con S32, S32 de 24 bits y S24. Los contenedores no influyen.

Por eso `DeviceInfo::hardware_gain_db` lee ese volumen en WASAPI. Si no es neutro, el modo automático no usa DoP, ninguna ruta se declara bit-perfect y `arpegio-cli` avisa. `--wire s32-24|s24` fuerza el contenedor WASAPI para repetir estas comprobaciones.

Segunda tanda el mismo día, con Windows al 100 %. Las 14 pruebas sonaron bien, con 0 cortes, 0 eventos tardíos y 0 muestras recortadas:

| Pruebas | Qué se escuchó |
|---|---|
| 1 y 2 | DoP con música: DSD128 por WASAPI y DSD64 por ASIO |
| 3 a 6 | PCM directo: FLAC 24/96 por ASIO, WAV 16/44.1 por WASAPI, AAC por ASIO y MP3 por WASAPI |
| 7 a 10 | Paso sin hueco: CUE en APE y en FLAC, archivos FLAC separados e híbrido `.iso.wv`. Nunca se reabrió el dispositivo |
| 11 a 13 | Cambio de formato entre pistas: PCM a DSD y DSD a PCM por ASIO, y 44.1 a 96 kHz por WASAPI |
| 14 | Mezcla a estéreo de un DSF de 6 canales |

`arpegio-cli` reproduce álbumes de la biblioteca y salta a una transición. Los ids salen de `arpegio-library $db albums`:

```powershell
# Los 12 últimos segundos de la primera pista y el paso a la segunda
.\arpegio-cli.exe play --db $db --album 18 --output asio --device topping --last 12 --seconds 24
# Lo mismo entre dos archivos, aquí cambiando de formato
.\arpegio-cli.exe play $wav $jazz --output asio --device topping --last 8 --seconds 18
```

## Verificado

Los detalles y cifras están en [DESIGN.md](DESIGN.md#verificación-hecha-el-2026-10-30).

- 126 pruebas automáticas sin fallos. Las 121 anteriores pasaron también en 12 ejecuciones seguidas con la CPU al 100 %. Diez fallos introducidos a propósito en el motor, cinco en la biblioteca y doce en la detección de MQA y las secciones fueron detectados.
- Aplicación real con la colección real y silencio digital: `tools\ui\run.ps1` hace más de 50 comprobaciones sobre la ventana, sin errores de consola. Ver [Interfaz](DESIGN.md#verificación-de-la-interfaz-2026-10-31).
- `LOOPBACK` del MiniFuse: coincidencia total a 24 bits en 44.1, 96 y 192 kHz.
- 11 FLAC reales coinciden con su MD5.
- DST idéntico a `libdstdec` en 18 000 frames, en 2 y 6 canales.
- Topping: WASAPI exclusivo, compartido y ASIO, PCM, DoP y DSD nativo hasta DSD256, sin eventos tardíos ni cortes.
- [Escucha real](#prueba-de-escucha) de DSD nativo, DoP, SACD, PCM, conversión a PCM, paso sin hueco, cambio de formato y mezcla de 6 canales.
- APE, WavPack PCM, M4A, MP3, DFF sin comprimir, WAV y FLAC reales abren y decodifican.
- Biblioteca sobre la colección real: 5 184 pistas, 476 álbumes y 382 artistas. Las 5 184 pistas abren y se leen al principio, en medio y al final, también con las áreas multicanal de SACD. Reescanear sin cambios tarda 0.06 s y ningún archivo de la colección se modificó.
- 954 pistas CUE empiezan en su muestra exacta en FLAC, WavPack, APE y DSF.
- Detección de MQA sobre la colección real: los 1 022 archivos que marca el escáner son exactamente los que encuentra un análisis independiente de 30 s por pista, escrito aparte con libFLAC y sin usar el código de Arpegio. Ningún WAV, APE ni WavPack lo lleva. Esos 1 022 son 1 039 pistas contando las CUE, y en los 944 archivos que llevan etiqueta `ORIGINALSAMPLERATE` esa frecuencia coincide con la del audio.
- Secciones sobre la colección real: los 476 álbumes están cada uno en una sola, con 80 en MQA, 31 en DSD, 303 en CD, 42 en PCM HighRes y 20 en Lossy. `tools\ui\steps_sections.mjs` las recorre en la ventana real, también con la ventana mínima y la cola abierta, sin errores de consola.
- Actualización de un catálogo real de la versión 1, sobre una copia: 4 748 pistas se releen en 25 s, y quedan iguales ids, favoritos, listas, cola y fechas de alta. El escaneo siguiente vuelve a tardar 0.05 s. Un escaneo completo desde cero tarda 43 s.

## Implementado pero sin verificar

- **Interfaz con música real.** Todas las rutas se recorrieron con silencio digital y señal inaudible. Falta escuchar seek, siguiente, anterior y cambio de salida con música, por si hay clics.
- Reordenar la cola arrastrando: probado con eventos simulados, no con el ratón.
- Paso sin hueco: pasó de oído en SACD, CUE y archivos separados, sin comparar la salida real muestra a muestra.
- DoP por ASIO con el volumen de Windows bajo: ASIO no consulta ese volumen, y no se sabe si le afecta.
- Aviso de volumen: la lógica está cubierta por pruebas, pero no se vio en pantalla con el Topping por debajo del 100 %. Se lee al listar dispositivos y al empezar a reproducir, no durante la reproducción.
- Reapertura tras un reset de ASIO o una desconexión USB (`reopen_output_if_needed`).
- DFF con DST y WavPack con DSD: no hay archivos así en la colección.
- AIFF y Ogg Vorbis: no hay archivos en `D:\Música`, aunque sí en otras carpetas del equipo.
- ALAC: TagLib identifica los 37 `.m4a` de la colección como AAC, así que no hay ningún ALAC real con el que probar que sea exacto.
- APE: pasó de oído, sin comprobar contra un hash.
- Mezcla de 6 canales a estéreo: pasó de oído, sin prueba automática.
- Calidad medida de r8brain en esta integración: solo corrió con silencio.
- Vigilancia de carpetas sobre la colección real: solo se probó con carpetas temporales.
- Hoja CUE en el bloque `CUESHEET` nativo de FLAC: solo se lee la etiqueta del mismo nombre.
- Las comprobaciones completas de `tools\ui\run.ps1` no se repitieron tras añadir las secciones, porque reproducen por el DAC y pulsan teclas multimedia. Solo se ejecutó `steps_sections.mjs`.
- MQA en ALAC: la colección no tiene ningún ALAC. El detector no depende del formato, pero solo se probó con FLAC reales y WAV sintéticos.

## Problema abierto

Una ejecución de `arpegio-library verify` sobre el catálogo multicanal se quedó parada, sin usar CPU, tras 33 s de CPU, y hubo que matarla. No se repitió en 17 ejecuciones posteriores, seis de ellas en las mismas condiciones, y las 5 184 pistas abren bien. No se encontró la causa, ni se sabe si fue en un decodificador o al cerrar el proceso. Si vuelve a ocurrir, capturar los hilos del proceso antes de matarlo.

## Limitaciones actuales del motor

- No hay volumen por software ni ReplayGain.
- DoP automático depende de que el usuario lo confirme en Ajustes. Falta la prueba guiada.
- La precarga en RAM es el anillo: 20 s del formato de salida, con un mínimo de 32 MB, que a 44.1 kHz son unos 90 s.
- Editar la cola en los tramos que el anillo ya contiene provoca un hueco breve, porque el anillo se rellena desde lo que se oye. Añadir al final no lo provoca.
- Cambiar de dispositivo o sus ajustes reinicia el flujo en la misma posición, con un hueco breve.

## Limitaciones actuales de la biblioteca

- Mover o renombrar un archivo lo convierte en una pista nueva, y pierde favoritos y listas.
- Un archivo modificado sin cambiar tamaño ni fecha no se relee. `ScanOptions::force` o `rescan(true)` lo fuerzan.
- Cambiar `sacd_multichannel` requiere un reescaneo forzado para tener efecto.
- Un álbum repartido entre dos carpetas cuenta como dos, salvo que sean subcarpetas de disco.
- Los `.bin` de imagen de CD no se decodifican. Hay dos hojas así en la colección y sus pistas sueltas entran por separado.
- Las pistas sin etiquetas quedan como `Unknown Artist`: 93 en la colección. El nombre del archivo no se interpreta.
- `Library::remove_folder` es asíncrono: volver a añadir la misma carpeta antes de que termine da error de solapamiento.
- MQA se reconoce pero no se decodifica. Solo se busca en los primeros 4 s de cada archivo, así que uno que tuviera MQA únicamente más adelante, como un disco en un solo archivo con hoja CUE y una pista MQA en medio, no se marcaría.
- Un álbum va a una sola sección: las pistas MQA de un álbum mayoritariamente CD solo se distinguen en su fila.
- WMA cuenta siempre como Lossy, también WMA Lossless. No hay archivos así en la colección.

## Limitaciones actuales de la interfaz

- Solo tema oscuro y solo español. Los mensajes de error poco habituales del motor llegan en inglés.
- Sin selección múltiple de pistas.
- La cola no se refresca si un reescaneo cambia las etiquetas de una pista que ya está en ella.
- Sin controles de Windows en la pantalla de bloqueo ni en la miniatura de la barra de tareas. Las teclas multimedia sí funcionan, salvo que otro programa las tenga reservadas.
- Actualizar la lista de dispositivos exige parar la música: cargar otra copia de un driver ASIO en uso puede tumbarlo.
- Sin arrastrar archivos desde el Explorador.
- WebView2 ocupa unos 420 MB de memoria en 7 procesos.

## Siguiente trabajo

### Cierre

- Escucha de la interfaz con música.
- Prueba guiada de DoP con silencio DSD, mirando la pantalla del DAC.
- Controles multimedia de Windows en la pantalla de bloqueo y en la barra de tareas.
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
| `src/library/` | `library` coordina; `library_store` es el catálogo en SQLite; `scanner` lee las carpetas; `metadata`, `mqa`, `artwork` y `cue_sheet` son sus piezas; `track_source` conecta con el motor |
| `src/app/` | `controller` es la aplicación, sin ventanas y por tanto comprobable; `main_window` es la ventana con WebView2; `settings` son los ajustes |
| `ui/` | La página, en JavaScript sin dependencias. `store` guarda el estado, `router` las vistas, `views/` cada vista y `signal` la ruta de la señal |
| `cli/` | `arpegio-cli` y `arpegio-dsd2wav` |
| `tests/` | Pruebas automáticas, con salidas simuladas en `test_player.cpp` y `test_controller.cpp`, y la biblioteca en `test_library.cpp` |
| `tools/` | Generador DSD, informes, `arpegio-library`, `measure/` y `ui/` |

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

Aplicación real, con silencio digital por las salidas reales. La carpeta de música solo se cataloga, y en una carpeta temporal:

```powershell
powershell -File tools\ui\run.ps1 -MusicFolder "D:\Música" -Device topping
```

Abre y cierra la ventana varias veces, minimiza, pulsa teclas multimedia y cambia el formato del DAC. Las capturas quedan en `%TEMP%\arpegio-ui-check\shots`. `tools\ui\check.mjs` sirve para escribir otras pruebas: controla la aplicación por el protocolo de DevTools de Chromium con `--devtools`.

Señal inaudible para probar hardware: `arpegio-dsdgen` sin tonos, por ejemplo `.\arpegio-dsdgen.exe quiet64.dsf ntf64.txt 2822400 20`. Para otras tasas, generar antes el diseño con `design_ntf.py 128` o `256`.

## Entorno y trampas conocidas

- Windows 11 x64, i5-12600K, 32 GB. VS Build Tools 2022 sin ATL, Windows SDK 10.0.26100, CMake 4.4, Ninja, Node 24, Rust 1.99, Python 3.14.
- DAC Topping DX1 II y Arturia MiniFuse 2. Música en `D:\Música`.
- Una compilación fallida deja el binario anterior, y las pruebas corren sobre él. Hay que mirar la salida de compilación antes de fiarse del resultado. Pasa, por ejemplo, con `arpegio.exe` abierto, que da `LNK1168`.
- `scripts\build.bat` imprime un aviso de `vswhere`. Es inofensivo.
- Evitar parches largos de Python en línea desde Bash: se rompen. Es mejor escribir el script en un archivo.
- La descarga del binario de FFmpeg fue bloqueada por el control de permisos. FFmpeg no hace falta y no se debe reintentar.
- Los archivos de `ui/` se copian junto al ejecutable en cada compilación. Sin compilar, `arpegio.exe` sigue usando la copia anterior.
- En JavaScript, `elemento.append(null)` escribe el texto «null». Los hijos condicionales van por `fill()` de `dom.js` o dentro de `h()`.
- La salida simulada de las pruebas corre unas 40 veces más rápido que el tiempo real. Una prueba que mire un momento concreto de una pista debe pausar antes, o fallará con la máquina cargada.
- Desde PowerShell, un `$null` pasado a `FindWindowW` llega como cadena vacía y no encuentra nada. Las pruebas de minimizar parecían pasar sin haber minimizado.
- La colección real descubre lo que las pruebas sintéticas no: un `.flac` con ID3v2 delante cerraba el programa, y había híbridos `.iso.wv`, FLAC sin longitud en la cabecera y hojas con tiempos fuera de norma. Tras tocar decodificadores o escáner, repetir `scan` y `verify`.
- Windows sella los archivos con un reloj de unos 15 ms. Una prueba que reescribe un archivo del mismo tamaño justo después de escanearlo debe mover la fecha a mano.
- `third_party/` no se versiona. `scripts/fetch_deps.py` lo recrea con versiones y SHA-256 fijos.
