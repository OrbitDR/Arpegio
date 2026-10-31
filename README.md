# Arpegio

Reproductor de música para Windows x64 centrado en la calidad de audio: PCM bit-perfect, DSD nativo por ASIO, DoP y conversión de DSD a PCM. El nombre es provisional.

Estado: motor, biblioteca y línea de comandos funcionando. La interfaz aún no existe. Para retomar el desarrollo, ver [docs/PROGRESS.md](docs/PROGRESS.md). El diseño está en [docs/DESIGN.md](docs/DESIGN.md).

## Compilar

Requiere Visual Studio Build Tools 2022, Windows SDK, CMake, Ninja y Python.

```powershell
python scripts\fetch_deps.py
scripts\build.bat release test
```

## Usar

```powershell
cd build\release
.\arpegio-cli.exe devices
.\arpegio-cli.exe play "pista.flac" --output exclusive --device topping
.\arpegio-cli.exe play "pista.dsf" --output asio --device topping
.\arpegio-cli.exe play "disco.iso" --output asio --device topping --track 3
```

`--output` acepta `asio`, `exclusive` o `shared`. `--device` busca por parte del nombre. `--dsd` acepta `auto`, `native`, `dop` o `pcm`.

La biblioteca se prueba con `arpegio-library`, que nunca escribe en la carpeta de música:

```powershell
.\arpegio-library.exe biblioteca.db scan "D:\Música"
.\arpegio-library.exe biblioteca.db stats
.\arpegio-library.exe biblioteca.db search "texto"
.\arpegio-library.exe biblioteca.db verify
```

## Licencia

GPLv3. El SDK ASIO de Steinberg se usa bajo su opción GPLv3 y no se incluye en el repositorio: lo descarga `scripts/fetch_deps.py`. ASIO es una marca de Steinberg. Las licencias de las dependencias están en [third_party/README.md](third_party/README.md).
