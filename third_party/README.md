# Dependencias

Las descarga `scripts/fetch_deps.py`, con versión y SHA-256 fijos. No se versionan aquí.

| Paquete | Uso | Licencia |
|---|---|---|
| asiosdk 2.3.4 | ASIO | GPLv3 o licencia de Steinberg |
| r8brain-free-src 7.6 | Remuestreo | MIT |
| libsoxr 0.1.3 | Remuestreo alternativo, aún sin usar | LGPL 2.1 |
| flac 1.5.0, ogg 1.3.6, vorbis 1.3.7 | Decodificación | BSD |
| wavpack 5.9.0 | Decodificación | BSD |
| Monkey's Audio SDK 13.45 | Decodificación | BSD |
| minimp3 | Decodificación | CC0 |
| nlohmann json 3.12.0 | Mensajes entre la página y la aplicación, y ajustes | MIT |
| SQLite 3.54.1 | Biblioteca, con FTS5 | Dominio público |
| TagLib 2.3.2, utfcpp 4.2.1 | Etiquetas y portadas incrustadas | LGPL 2.1 o MPL 1.1; Boost |
| WebView2 SDK 1.0.4314.35 | Interfaz. Se enlaza `WebView2LoaderStatic.lib`, y el runtime es el que trae Windows | BSD, de Microsoft |

Los iconos de `ui/js/dom.js` son trazados de Material Icons de Google, con licencia Apache 2.0, compatible con GPLv3.

`src/dsd/dst_decoder.cpp` deriva de FFmpeg (LGPL 2.1 o posterior). `src/decode/sacd_iso.cpp` sigue las estructuras de SACD Ripper (GPL 2 o posterior) y foo_input_sacd (LGPL 2.1 o posterior). Las tres licencias son compatibles con GPLv3.
