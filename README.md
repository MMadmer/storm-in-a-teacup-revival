# Storm in a Teacup Revival

[Русский](#русский) · [English](#english)

## Русский

Неофициальные моды и исправления для игры «Буря в стакане» (1С, версия 1.0.147). Главное исправление:
**финальный ролик воспроизводится до конца**, игра больше не завершается с ошибкой.

### Состав

- **VideoCrashFix** — исправляет вылет на финальном ролике «Малиновый Джыдах, эпилог». Ролик прерывался через
  несколько секунд во всех версиях игры из-за ошибки синхронизации потоков в видеодекодере `rk_theora.dll`,
  поэтому концовку нельзя было досмотреть, а прохождение не засчитывалось. Исправление применяется в памяти,
  файлы игры не изменяются.
- **ExpandedSettings** — добавляет в меню «Видео» режим экрана (в окне, полноэкранный, в окне без рамки),
  вертикальную синхронизацию и ограничение FPS. Обеспечивает чёткое изображение на мониторах с масштабированием.
- **PerformanceOptimizer** — повышает производительность без снижения качества изображения. На тестовом
  ноутбуке (RTX 3070 Laptop, 2560×1440): около 250 FPS в полноэкранном режиме и около 175 FPS в окне без рамки.
- **AutoUpdate** — при запуске игры проверяет наличие новой версии и предлагает её установить. Загружаются
  только изменённые файлы, настройки сохраняются.
- **Загрузчик модов** — `d3d9.dll` в папке игры загружает моды из папки `Mods`. Каждый мод включается
  и отключается в файле `Mods\Mods.ini`.

### Установка

1. Скачайте архив со страницы [Releases](../../releases).
2. Распакуйте его в папку игры, где находится `Storm.exe`.
3. Запустите игру.

Для удаления удалите `d3d9.dll`, `StormModLoader.dll` и папку `Mods`. Файлы игры моды не изменяют.

Требуется версия игры 1.0.147. На других версиях моды отключаются автоматически.
На ноутбуках с двумя видеокартами полноэкранный режим работает заметно быстрее окна без рамки.

### Сборка

Требуются Visual Studio 2022 или новее с инструментами C++ для x86 и CMake 3.24 или новее. Репозиторий
предназначен для размещения в папке игры: собранные файлы сохраняются туда, где их ожидает игра.

```
cmake -S . -B build -A Win32
cmake --build build --config Release
python tools/package_release.py
```

Порядок выпуска обновлений описан в [docs/AUTO_UPDATE.md](docs/AUTO_UPDATE.md). `Mods\StormBench`
и `tools\bench.ps1` предназначены для замеров производительности при разработке.

### Лицензия

Код модов распространяется по лицензии [MIT](LICENSE). Игра и её файлы в репозиторий не входят; права на них
принадлежат правообладателям.

## English

Unofficial mods and fixes for the Russian racing game "Буря в стакане" (Storm in a Teacup, 1C, version 1.0.147).
The main fix: **the final video plays to the end** and no longer crashes the game.

### Contents

- **VideoCrashFix** — fixes the crash in the final video, "Малиновый Джыдах, эпилог". In every release of the
  game the video stopped a few seconds in because of a thread synchronization error in the `rk_theora.dll` video
  decoder, so the ending could not be watched and the campaign was never completed. The fix is applied in memory;
  game files are not modified.
- **ExpandedSettings** — adds display mode (windowed, fullscreen, borderless), vertical synchronization and an FPS
  limit to the Video menu. Keeps the image sharp on scaled displays.
- **PerformanceOptimizer** — improves performance without reducing image quality. On the test laptop
  (RTX 3070 Laptop, 2560×1440): about 250 FPS in fullscreen and about 175 FPS in borderless mode.
- **AutoUpdate** — checks for a new version when the game starts and offers to install it. Only changed files are
  downloaded; settings are preserved.
- **Mod loader** — `d3d9.dll` in the game folder loads the mods from the `Mods` folder. Each mod is enabled or
  disabled in `Mods\Mods.ini`.

### Installation

1. Download the archive from the [Releases](../../releases) page.
2. Unpack it into the game folder that contains `Storm.exe`.
3. Start the game.

To uninstall, delete `d3d9.dll`, `StormModLoader.dll` and the `Mods` folder. The mods do not modify game files.

Game version 1.0.147 is required. On other versions the mods disable themselves.
On laptops with two graphics cards, fullscreen mode is noticeably faster than borderless mode.

### Building

Requires Visual Studio 2022 or newer with the x86 C++ tools and CMake 3.24 or newer. The repository is meant to
be placed in the game folder: the built files are saved where the game expects them.

```
cmake -S . -B build -A Win32
cmake --build build --config Release
python tools/package_release.py
```

The update release procedure is described in [docs/AUTO_UPDATE.md](docs/AUTO_UPDATE.md). `Mods\StormBench` and
`tools\bench.ps1` are intended for performance measurements during development.

### License

The mod code is licensed under [MIT](LICENSE). The game and its files are not included; all rights to them belong
to their owners.
