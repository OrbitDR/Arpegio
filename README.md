# Arpegio

Reproductor de música para Windows x64 centrado en la calidad de audio: PCM bit-perfect, DSD nativo por ASIO, DoP y conversión de DSD a PCM. El nombre es provisional.

Estado: aplicación completa de escritorio, con biblioteca, cola y ajustes por dispositivo. Para retomar el desarrollo, ver [docs/PROGRESS.md](docs/PROGRESS.md). El diseño está en [docs/DESIGN.md](docs/DESIGN.md).

## Compilar

Requiere Visual Studio Build Tools 2022, Windows SDK, CMake, Ninja y Python. Para ejecutarlo hace falta WebView2 Runtime, que Windows 11 ya incluye.

```powershell
python scripts\fetch_deps.py
scripts\build.bat release test
```

El instalador sale de `scripts\package.bat`, que necesita Inno Setup 6 (`winget install --id JRSoftware.InnoSetup --scope user`) y deja `build\dist\installer\Arpegio-VERSIÓN-x64.exe`. Se instala para el usuario actual, sin permisos de administrador. No está firmado, así que Windows puede avisar la primera vez. Al desinstalar se conservan el catálogo y los ajustes.

## Usar

La aplicación es `build\release\arpegio.exe`, o la copia instalada. Guarda su catálogo y sus ajustes en `%LOCALAPPDATA%\Arpegio`, y solo lee la música.

Junto al buscador están las secciones por tipo de archivo: MQA, DSD, CD, PCM HighRes y Lossy. MQA solo se reconoce: Arpegio no lo decodifica.

En Ajustes se elige la salida. Por defecto usa WASAPI compartido, que no es bit-perfect. Con WASAPI conviene tener el volumen de Windows al 100 %.

Por ASIO y WASAPI exclusivo, Arpegio retiene el dispositivo mientras suena y también en pausa, y ningún otro programa puede usarlo. El enchufe dorado a la derecha del indicador bit-perfect lo libera: detiene la música y queda desconectado en gris. Reproducir vuelve a tomar el dispositivo desde el mismo punto. En WASAPI compartido no retiene nada, y el enchufe queda en gris.

El volumen de la barra inferior está en 0 dB y, mientras siga ahí, no toca ninguna muestra. Al bajarlo la salida deja de ser bit-perfect, así que lo mejor sigue siendo dejarlo en 0 dB y usar el volumen del DAC. Cada dispositivo guarda su nivel. El DSD nativo y el DoP no tienen volumen por software: con el control por debajo de 0 dB, el DSD se convierte a PCM para no sonar a nivel completo.

En Ajustes también están:

- ReplayGain por pista o por álbum, a partir de las etiquetas. Las pistas sin etiquetas no cambian, y Arpegio no las analiza.
- El remuestreador, r8brain o SoX, que solo actúa si el dispositivo no admite la frecuencia de la pista o en WASAPI compartido.
- La prueba de DoP, que envía silencio DSD y pregunta qué indica la pantalla del DAC.

La pista que suena aparece también en los controles de Windows: el panel de medios, la pantalla de bloqueo y los botones de la miniatura en la barra de tareas. Las teclas multimedia del teclado las envía Windows al programa que esté sonando.

| Teclas | Acción |
|---|---|
| Espacio | Reproducir o pausar |
| Ctrl+Izquierda, Ctrl+Derecha | Pista anterior o siguiente |
| Mayús+Izquierda, Mayús+Derecha | Retroceder o avanzar 10 s |
| Ctrl+Arriba, Ctrl+Abajo | Subir o bajar el volumen 1 dB |
| Ctrl+F | Buscar |
| Alt+Izquierda, Alt+Derecha | Atrás o adelante en las vistas |

También hay una línea de comandos:

```powershell
cd build\release
.\arpegio-cli.exe devices
.\arpegio-cli.exe play "pista.flac" --output exclusive --device topping
.\arpegio-cli.exe play "pista.dsf" --output asio --device topping
.\arpegio-cli.exe play "disco.iso" --output asio --device topping --track 3
```

`--output` acepta `asio`, `exclusive` o `shared`. `--device` busca por parte del nombre. `--dsd` acepta `auto`, `native`, `dop` o `pcm`. `--volume-db` y `--resampler` hacen lo mismo que sus controles de la aplicación.

La biblioteca se prueba con `arpegio-library`, que nunca escribe en la carpeta de música:

```powershell
.\arpegio-library.exe biblioteca.db scan "D:\Música"
.\arpegio-library.exe biblioteca.db stats
.\arpegio-library.exe biblioteca.db search "texto"
.\arpegio-library.exe biblioteca.db verify
```

## Licencia

GPLv3. El SDK ASIO de Steinberg se usa bajo su opción GPLv3 y no se incluye en el repositorio: lo descarga `scripts/fetch_deps.py`. ASIO es una marca de Steinberg. Las licencias de las dependencias están en [third_party/README.md](third_party/README.md), y el instalador las lleva en la carpeta `licenses`.
