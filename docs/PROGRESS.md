# Progreso de Arpegio

Punto de partida para retomar el desarrollo. Última sesión: 2026-11-04. El diseño y las decisiones técnicas están en [DESIGN.md](DESIGN.md).

## Dónde estamos

La aplicación `arpegio.exe` funciona de punta a punta: biblioteca, cola, reproducción y ajustes por dispositivo, con la colección real y por las salidas reales. Tiene volumen por software, ReplayGain, dos remuestreadores, controles de Windows, prueba guiada de DoP e instalador. Toda la verificación de la interfaz se hizo con silencio digital, así que falta que el usuario la escuche con música.

El 2026-11-04, a petición del usuario, la interfaz cambió de aspecto: barra de título propia, paneles laterales translúcidos sobre el acrílico de Windows, barra inferior teñida con la portada, el transporte siempre en el centro de la ventana, los botones de sección en negrita y un indicador nuevo de formato y bit-perfect, sin el visto. Ver [Interfaz](DESIGN.md#interfaz) y [Marco de la ventana](DESIGN.md#verificación-del-marco-de-la-ventana-2026-11-04).

| Fase | Estado |
|---|---|
| 1. Base del proyecto y detección de dispositivos | Hecha |
| 2. Motor PCM: WASAPI, ASIO, gapless, seek, pausa | Hecha |
| 3. DSD: DSF, DFF, SACD ISO, DST, nativo, DoP, conversión | Hecha |
| 4. Biblioteca | Hecha |
| 5. Interfaz en WebView2 | Hecha, pendiente de escucha |
| 6. Cierre | Hecha, pendiente de lo que requiere al usuario: ver [Siguiente trabajo](#siguiente-trabajo) |

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
3. Seguir con el [trabajo pendiente](#siguiente-trabajo).

`scripts\package.bat` genera el instalador en `build\dist\installer`. Compila el preset `dist`, en otra carpeta, así que no molesta a un `arpegio.exe` abierto desde `build\release`.

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

### Comparación con Audirvana

El 2026-11-01 el usuario notó por ASIO mejor separación de instrumentos en Audirvana, escuchando WAV 16/44.1. Medido con `asio_tap`: los dos programas envían al driver exactamente las mismas muestras, con la misma frecuencia, formato y buffer. Los datos y lo que la medida no cubre están en [DESIGN.md](DESIGN.md#comparación-con-audirvana-por-asio-2026-11-01). No se cambió el motor.

Lo único que reabriría el asunto en PCM es una prueba a ciegas con el mismo volumen físico, con otra persona cambiando de programa. En 10 rondas, 9 aciertos o más tienen un 1 % de probabilidad de salir al azar. Entonces tocaría medir la salida analógica.

## Verificado

Los detalles y cifras están en [DESIGN.md](DESIGN.md#verificación-hecha-el-2026-10-30).

- 190 pruebas automáticas sin fallos. Diez fallos introducidos a propósito en el motor, cinco en la biblioteca, doce en la detección de MQA y las secciones y trece en los clics y en liberar el dispositivo fueron detectados.
- Aplicación real con la colección real y silencio digital: `tools\ui\run.ps1` recorre la ventana sin errores de consola. Ver [Interfaz](DESIGN.md#verificación-de-la-interfaz-2026-10-31).
- A 0 dB, lo que reciben los drivers ASIO reales del Topping y del SMSL coincide al 100 % con un WAV 16/44.1 y otro 24/96 después de añadir el volumen. A −6.0206 dB la ganancia es exacta y el residuo es el del dither. Ver [Cierre](DESIGN.md#verificación-del-cierre-2026-11-01).
- SMSL: WASAPI exclusivo, compartido y ASIO, PCM, DoP y DSD nativo, también DSD512, sin eventos tardíos ni cortes, con silencio y señal inaudible.
- Remuestreo con r8brain y SoX: tono, banda alta, alias, longitud y alineación, en seis pares de frecuencias. Ver [Remuestreo](DESIGN.md#remuestreo).
- Volumen, ReplayGain, remuestreador y prueba de DoP en la ventana real con eventos de ratón y teclado reales, con el DSD por ASIO en el Topping.
- Controles de Windows leídos y manejados desde fuera de la aplicación, y teclas multimedia reales.
- Instalador: instalar, abrir y desinstalar en este equipo, conservando los datos.
- Clics con eventos de ratón reales: cada botón de la barra inferior acepta el clic en todos sus puntos, también si una actualización de estado llega durante la pulsación o si su icono cambia entre presionar y soltar. Ver [Clics](DESIGN.md#verificación-de-los-clics-2026-11-01).
- Barra de título propia con eventos reales del sistema: arrastrar, redimensionar por arriba, doble clic, maximizar sin tapar la barra de tareas, minimizar y cerrar. El transporte queda centrado a cualquier ancho. Ver [Marco de la ventana](DESIGN.md#verificación-del-marco-de-la-ventana-2026-11-04).
- Liberar el dispositivo contra otro proceso real, en WASAPI exclusivo y en ASIO con DSD nativo.
- `LOOPBACK` del MiniFuse: coincidencia total a 24 bits en 44.1, 96 y 192 kHz.
- Música real por ASIO en el Topping con `arpegio.exe`: lo que recibe el driver coincide al 100 % con un WAV 16/44.1 y es idéntico a lo que envía Audirvana. Ver [Comparación con Audirvana](DESIGN.md#comparación-con-audirvana-por-asio-2026-11-01).
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

- **La interfaz nueva con `tools\ui\run.ps1` completo.** `steps_frame.mjs`, `steps_sections.mjs` y las 190 pruebas pasaron, pero los pasos que reproducen silencio por el DAC, con los cambios de `steps_clicks.mjs` y `steps_volume.mjs`, no se han ejecutado: Audirvana y Arpegio estaban abiertos.
- Windows 10 y Windows 11 anterior a 22H2, donde los paneles quedan opacos y Windows no dibuja el borde de arriba. Solo se probó Windows 11 con el acrílico.
- Otras escalas de pantalla que el 100 % con el texto al 110 %, y maximizar en un segundo monitor de otro tamaño.
- **Interfaz con música real.** Todas las rutas se recorrieron con silencio digital y señal inaudible. Falta escuchar seek, siguiente, anterior, cambio de salida y liberar el dispositivo con música, por si hay clics.
- Controles de Windows a la vista. La sesión se leyó y se manejó desde fuera con la misma API que usan el panel de medios y la pantalla de bloqueo, y las teclas multimedia reales funcionan, pero nadie miró la pantalla de bloqueo. Los botones de la miniatura en la barra de tareas no se han visto: solo se probó que el programa recibe sus clics.
- Volumen con música: la rampa de 20 ms se midió sobre los datos, no de oído.
- Volumen en un driver con formato distinto de Int32LSB: el Topping y el SMSL solo usan ese. Los demás formatos están cubiertos por pruebas, no en un DAC real.
- Prueba de DoP con la respuesta de una persona: se recorrió por ASIO y WASAPI exclusivo con clics reales y el flujo DoP sale, pero nadie miró la pantalla del DAC durante esa prueba. En el Topping el mismo flujo ya se había visto como DSD el 2026-10-31. En el SMSL, nunca.
- Si el SMSL muestra DSD nativo y DoP como tales: las rutas corrieron sin errores, pero con silencio y sin mirar su pantalla.
- SoX con música real: medido con tonos, y en la aplicación solo con silencio.
- Instalación desde otro equipo o sin WebView2: el ciclo de instalar, abrir y desinstalar se probó en este equipo, que ya tiene el runtime.
- Reordenar la cola arrastrando: probado con eventos simulados, no con el ratón.
- Paso sin hueco: pasó de oído en SACD, CUE y archivos separados, sin comparar la salida real muestra a muestra. `asio_tap` ya permite hacerlo.
- DoP por ASIO con el volumen de Windows bajo: ASIO no consulta ese volumen, y no se sabe si le afecta.
- Aviso de volumen: la lógica está cubierta por pruebas, pero no se vio en pantalla con el Topping por debajo del 100 %. Se lee al listar dispositivos y al empezar a reproducir, no durante la reproducción.
- Reapertura tras un reset de ASIO o una desconexión USB (`reopen_output_if_needed`).
- DFF con DST y WavPack con DSD: no hay archivos así en la colección.
- AIFF y Ogg Vorbis: no hay archivos en `D:\Música`, aunque sí en otras carpetas del equipo.
- ALAC: TagLib identifica los 37 `.m4a` de la colección como AAC, así que no hay ningún ALAC real con el que probar que sea exacto.
- APE: pasó de oído, sin comprobar contra un hash.
- Mezcla de 6 canales a estéreo: pasó de oído, sin prueba automática.
- Vigilancia de carpetas sobre la colección real: solo se probó con carpetas temporales.
- Hoja CUE en el bloque `CUESHEET` nativo de FLAC: solo se lee la etiqueta del mismo nombre.
- Liberar el dispositivo mientras otro programa espera para tomarlo: se comprobó que puede abrirlo después, no que un programa ya abierto recupere el sonido por sí solo.
- MQA en ALAC: la colección no tiene ningún ALAC. El detector no depende del formato, pero solo se probó con FLAC reales y WAV sintéticos.

## Problema abierto

Una ejecución de `arpegio-library verify` sobre el catálogo multicanal se quedó parada, sin usar CPU, tras 33 s de CPU, y hubo que matarla. No se repitió en 17 ejecuciones posteriores, seis de ellas en las mismas condiciones, y las 5 184 pistas abren bien. No se encontró la causa, ni se sabe si fue en un decodificador o al cerrar el proceso. Si vuelve a ocurrir, capturar los hilos del proceso antes de matarlo.

Tres comprobaciones fallaban de vez en cuando, casi siempre con la CPU cargada. Las 20 ejecuciones completas de las 189 pruebas hechas después de los arreglos, 8 en reposo y 12 con todos los núcleos ocupados, dieron 0 fallos. La tabla de antes y después:

| Comprobación | Antes | Después | Qué se sabe |
|---|---|---|---|
| `library_follows_a_watched_folder`: `wait_for_tracks(library, 2)` | 2 de 48, y 1 de 120 de esa prueba sola con carga | 0 de 600 de esa prueba sola con carga | Resuelta y confirmada con un experimento. El constructor de `FolderWatcher` volvía antes de que su hilo llamara a `ReadDirectoryChangesW`: perdía 30 de cada 100 cambios hechos justo después, y ninguno tras el arreglo. Una vigilancia recién creada podía no enterarse de un cambio hasta el siguiente escaneo |
| `controller_shuffle_keeps_the_current_track_in_front`: `index == 0` | 2 de 48 | 0 de 20 | Arreglada, pero leída en el código y sin experimento, y con esa tasa 20 ejecuciones son indicio y no prueba. `run_queue` copiaba el índice antes de abrir el decoder y `add_segment` lo usaba después, y `worker` podía pisar `loader_index_` con `play_request_`. Si vuelve a fallar, esta explicación no bastaba |
| `seek_lands_on_the_right_sample`: `underruns == 0` | 1 de 48 | 0 de 20 | Sin investigar ni tocar. La salida simulada solo da unos milisegundos de margen tras un seek |

## Limitaciones actuales del motor

- DSD nativo y DoP no tienen volumen por software. Con el volumen por debajo de 0 dB, el DSD se convierte a PCM.
- ReplayGain solo lee etiquetas, no analiza el audio. En la colección real las llevan 227 de las 5 184 pistas, así que el resto no cambia de nivel.
- ReplayGain no se aplica a DSD nativo ni a DoP. Esas pistas siguen a su nivel aunque tengan etiquetas: en la colección real solo una.
- Bajar el volumen no cambia una pista DSD que ya suene nativa, porque el control está desactivado. Subirlo a 0 dB no devuelve al DSD nativo la pista en curso, sino la siguiente.
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
- Actualizar la lista de dispositivos exige parar la música: cargar otra copia de un driver ASIO en uso puede tumbarlo. El botón de liberar el dispositivo basta para eso. En WASAPI compartido está desactivado, y la única forma de parar es vaciar la cola.
- Sin arrastrar archivos desde el Explorador.
- WebView2 ocupa unos 420 MB de memoria en 7 procesos.
- Al pasar sobre el botón de maximizar no aparecen los diseños de ajuste de Windows 11, porque el botón es de la página. Windows+Z los abre.
- Con la ventana inactiva, Windows muestra el material de los paneles como un gris liso.

## Siguiente trabajo

Lo que solo puede hacer el usuario:

- Mirar la interfaz nueva: barra de título, paneles translúcidos, la barra inferior con distintas portadas y el indicador de formato.
- Escuchar la interfaz con música: seek, siguiente, anterior, cambio de salida y liberar el dispositivo, por si hay clics.
- Bajar y subir el volumen con música.
- Mirar los controles de la pantalla de bloqueo y los botones de la miniatura en la barra de tareas.
- Si quiere usar DoP por WASAPI, hacer la prueba guiada mirando la pantalla del DAC. Por ASIO, el Topping y el SMSL usan DSD nativo y no la necesitan.

Opcional, sin pedir por nadie:

- Analizar el volumen de las pistas sin etiquetas, porque ReplayGain apenas tiene efecto en la colección real. El análisis no escribiría en los archivos: se guardaría en el catálogo.
- Kernel Streaming, que Audirvana ofrece y el usuario no pidió.
- Firmar el instalador, que requiere un certificado de pago.
- Decidir el nombre definitivo. Si cambia, hay que tocar el AppUserModelID de `src/app/main.cpp` e `installer/arpegio.iss`, los recursos de `src/app/arpegio.rc` y la carpeta `%LOCALAPPDATA%\Arpegio`.

## Mapa del código

| Carpeta | Contenido |
|---|---|
| `src/audio/` | Formatos de dispositivo, anillo sin bloqueos, cuantización, dither y volumen |
| `src/dsd/` | Tabla DSD a PCM, filtro FIR, DoP, decodificador DST |
| `src/decode/` | Interfaz `Decoder` y un decodificador por formato; SACD en `sacd_iso`; rangos CUE en `range_decoder` |
| `src/engine/` | `render_plan` decide la ruta, `pipeline` convierte, `resampler` cambia la frecuencia, `player` coordina |
| `src/output/` | ASIO, WASAPI e hilo STA |
| `src/formats/` | Lector DSF y escritor WAV |
| `src/library/` | `library` coordina; `library_store` es el catálogo en SQLite; `scanner` lee las carpetas; `metadata`, `mqa`, `artwork` y `cue_sheet` son sus piezas; `track_source` conecta con el motor |
| `src/app/` | `controller` es la aplicación, sin ventanas y por tanto comprobable; `main_window` es la ventana con WebView2; `media_controls` son los controles de Windows; `settings` son los ajustes |
| `ui/` | La página, en JavaScript sin dependencias. `store` guarda el estado, `router` las vistas, `views/` cada vista, `signal` la ruta de la señal, `titlebar` la barra de título y `playerbar` la barra inferior |
| `cli/` | `arpegio-cli` y `arpegio-dsd2wav` |
| `tests/` | Pruebas automáticas, con salidas simuladas en `test_player.cpp` y `test_controller.cpp`, y la biblioteca en `test_library.cpp` |
| `tools/` | Generador DSD, informes, `arpegio-library`, `measure/` y `ui/` |
| `installer/` | Script de Inno Setup, que se ejecuta desde `scripts\package.bat` |

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

# Lo que dos programas envían a un driver ASIO, contra el archivo. El DAC recibe silencio
..\..\tools\measure\asio_tap.bat
powershell ..\..\tools\measure\asio_tap.ps1 on
.\arpegio-cli.exe play "pista.wav" --output asio --device topping --seconds 40
# Aquí, reproducir la misma pista en el otro programa y cerrarlo
powershell ..\..\tools\measure\asio_tap.ps1 off
& $py $t\compare_tap.py "pista.wav" (Get-ChildItem ..\tap\captures\*.raw).FullName
```

Cada programa deja en `build\tap\captures` un `.log` con las llamadas que hizo al driver. Las capturas ocupan unos 21 MB por minuto a 44.1 kHz. Con `--volume-db -6.0206`, `compare_tap.py` debe dar una ganancia de 0.5 y un residuo de −146 dBFS.

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

`-Build dist` usa el ejecutable del preset `dist`. Conviene cerrar antes Arpegio y Audirvana: la prueba necesita las salidas libres, y otra sesión multimedia se quedaría con las teclas. Abre y cierra la ventana varias veces, minimiza, pulsa teclas multimedia, abre el dispositivo desde otro proceso y cambia el formato del DAC. Con la colección real tarda unos 5 minutos. Las capturas quedan en `%TEMP%\arpegio-ui-check\shots`. `tools\ui\check.mjs` sirve para escribir otras pruebas: controla la aplicación por el protocolo de DevTools de Chromium con `--devtools`. Devuelve error si falla cualquier comprobación de un paso. Con `--no-devices` la aplicación no lista salidas, y así los pasos que no reproducen nada, como `steps_frame.mjs` y `steps_sections.mjs`, pueden correr mientras otro programa usa el DAC: listar carga todos los drivers ASIO.

Señal inaudible para probar hardware: `arpegio-dsdgen` sin tonos, por ejemplo `.\arpegio-dsdgen.exe quiet64.dsf ntf64.txt 2822400 20`. Para otras tasas, generar antes el diseño con `design_ntf.py 128` o `256`.

## Entorno y trampas conocidas

- Windows 11 x64, i5-12600K, 32 GB. VS Build Tools 2022 sin ATL, Windows SDK 10.0.26100, CMake 4.4, Ninja, Node 24, Rust 1.99, Python 3.14, Inno Setup 6.7 para el usuario.
- DAC Topping DX1 II, SMSL y Arturia MiniFuse 2. El driver ASIO del SMSL se llama `USB DAC ASIO`, sin la marca, y su salida WASAPI `Altavoces (SMSL USB DAC)`: `USB DAC` sirve para los dos en `-Device`. Música en `D:\Música`.
- Una compilación fallida deja el binario anterior, y las pruebas corren sobre él. Hay que mirar la salida de compilación antes de fiarse del resultado. Pasa, por ejemplo, con `arpegio.exe` abierto, que da `LNK1168`.
- `scripts\build.bat` imprime un aviso de `vswhere`. Es inofensivo.
- Evitar parches largos de Python en línea desde Bash: se rompen. Es mejor escribir el script en un archivo.
- La descarga del binario de FFmpeg fue bloqueada por el control de permisos. FFmpeg no hace falta y no se debe reintentar.
- Los archivos de `ui/` se copian junto al ejecutable en cada compilación. Sin compilar, `arpegio.exe` sigue usando la copia anterior.
- En JavaScript, `elemento.append(null)` escribe el texto «null». Los hijos condicionales van por `fill()` de `dom.js` o dentro de `h()`.
- `element.click()` no pasa por la detección de lo que hay bajo el puntero: un botón tapado o un clic perdido no se ven. Para probar que se puede pulsar algo hay que usar `Input.dispatchMouseEvent`, y mirar sobre qué botón cayó el clic: una presión fuera de la pantalla también puede contar como clic en otro sitio.
- Mientras suena llega un `status` cada 200 ms. Reconstruir algo que tenga controles en cada actualización pierde clics. Lo que cambia con el estado se actualiza en su sitio.
- La salida simulada de las pruebas corre unas 40 veces más rápido que el tiempo real. Una prueba que mire un momento concreto de una pista debe pausar antes, o fallará con la máquina cargada.
- En el `status` del controlador, `index` y `trackId` salen de la cola, no de lo que suena. Una prueba con silencio solo distingue la pista real por `duration`, que viene del decoder abierto: es lo que comprueba `Harness::sounding` en `test_controller.cpp`. Mirando `trackId`, un motor que sigue con la pista anterior pasa la prueba.
- Desde PowerShell, un `$null` pasado a `FindWindowW` llega como cadena vacía y no encuentra nada. Las pruebas de minimizar parecían pasar sin haber minimizado.
- La colección real descubre lo que las pruebas sintéticas no: un `.flac` con ID3v2 delante cerraba el programa, y había híbridos `.iso.wv`, FLAC sin longitud en la cabecera y hojas con tiempos fuera de norma. Tras tocar decodificadores o escáner, repetir `scan` y `verify`.
- Windows sella los archivos con un reloj de unos 15 ms. Una prueba que reescribe un archivo del mismo tamaño justo después de escanearlo debe mover la fecha a mano.
- `third_party/` no se versiona. `scripts/fetch_deps.py` lo recrea con versiones y SHA-256 fijos.
- En ASIO, `windowsGainDb` no es una lectura: `current_hardware_gain_db` devuelve el 0 por defecto aunque Windows esté más bajo. El valor real está en el endpoint WASAPI del mismo DAC.
- Con `asio_tap.ps1 on`, ese driver queda en silencio para todos los programas hasta `off`. Antes de reproducir hay que comprobar que el programa cargó `asio_tap.dll`: si no, sonaría de verdad. `-Audible` deja pasar el sonido y sigue grabando.
- Un programa empaquetado en MSIX, como Audirvana, ve sus escrituras en `AppData` redirigidas a su paquete. Por eso las capturas van junto a la DLL, en `build\tap`, y no en `%TEMP%`.
- El `RenderPlan` de una pista se decide antes de oírse. Lo que dependa del momento, como el volumen, debe recalcularse al abrir el flujo.
- Al cerrar DSD nativo por ASIO, la salida WASAPI del mismo DAC falta unos 0.8 s. Un programa que arranque en ese hueco no la lista, y hace falta Buscar dispositivos. `run.ps1` espera 2 s entre pasos por eso.
- Un paso de `tools/ui` que no encuentra lo que debe medir tiene que fallar: dos de ellos daban por buena una medida que no habían hecho. Hay que leer las cifras, no solo el código de salida.
- Una prueba con `for (const json& x : h.call(...).at("list"))` recorre un temporal ya destruido. Hay que guardar antes el resultado de `call`.
- `Get-StartApps` tarda unos segundos en enterarse de un acceso directo nuevo o borrado.
- Un heredoc largo en Bash se rompe si el texto lleva apóstrofes, como los comentarios en inglés. Para añadir código, mejor las herramientas de edición.
