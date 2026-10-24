# Storm in a Teacup Revival

[Русский](#русский) · [English](#english)

## Русский

Неофициальные моды и фиксы для «Буря в стакане» (1С, v1.0.147). Главное: **финальный ролик наконец
досматривается до конца** и игра больше не падает на нём.

### Что внутри

- **VideoCrashFix** — фикс финального ролика «Малиновый Джыдах, эпилог». Во всех версиях игры он падал через
  пару секунд после начала из-за гонки потоков в видеодекодере `rk_theora.dll`, поэтому концовку было не
  посмотреть, а прохождение не засчитывалось. Мод чинит это в памяти, файлы игры не меняются.
- **ExpandedSettings** — в меню «Видео» добавлены режим экрана (в окне, полноэкранный, в окне без рамки),
  вертикальная синхронизация и лимит FPS. Игра корректно работает на мониторах с масштабированием.
- **PerformanceOptimizer** — оптимизация без потери качества картинки. На тестовом ноутбуке
  (RTX 3070 Laptop, 2560×1440) около 250 FPS в полноэкранном режиме и около 175 в окне без рамки.
- **Загрузчик модов** — `d3d9.dll` рядом с игрой подгружает моды из папки `Mods`, каждый включается
  и выключается в `Mods\Mods.ini`.

### Установка

1. Скачай архив из [Releases](../../releases).
2. Распакуй его в папку игры, туда, где лежит `Storm.exe`.
3. Запускай игру как обычно.

Удаление: убери `d3d9.dll`, `StormModLoader.dll` и папку `Mods`. Файлы игры моды не трогают.

Нужна версия игры 1.0.147. На других версиях моды сами отключаются, ничего не ломая.
На ноутбуках с двумя видеокартами режим «Полноэкранный» заметно быстрее окна без рамки.

### Сборка

Visual Studio 2022+ (C++ для x86) и CMake 3.24+. Репозиторий рассчитан на то, что лежит прямо в папке игры:
собранные DLL попадают туда, где их ждёт игра.

```
cmake -S . -B build -A Win32
cmake --build build --config Release
```

`Mods\StormBench` и `tools\bench.ps1` — инструменты разработчика для замеров FPS, игрокам они не нужны.

### Лицензия

Код модов — [MIT](LICENSE). Игра и её файлы в репозиторий не входят, права на них принадлежат правообладателям.

## English

Unofficial mods and fixes for the Russian racing game "Буря в стакане" (Storm in a Teacup, 1C, v1.0.147).
The headline: **the final video can finally be watched to the end** without crashing the game.

### What's inside

- **VideoCrashFix** — fixes the final video ("Малиновый Джыдах, эпилог"). In every release of the game it
  crashed a few seconds in because of a thread race in the `rk_theora.dll` video decoder, so the ending was
  unwatchable and the campaign never completed. The fix is applied in memory; no game files are changed.
- **ExpandedSettings** — adds display mode (windowed, fullscreen, borderless), VSync and an FPS limit to the
  Video menu. The game now renders correctly on scaled (HiDPI) displays.
- **PerformanceOptimizer** — faster without touching image quality. On the test laptop (RTX 3070 Laptop,
  2560×1440): about 250 FPS in fullscreen and about 175 in borderless.
- **Mod loader** — `d3d9.dll` next to the game loads mods from the `Mods` folder; each one can be switched
  on or off in `Mods\Mods.ini`.

### Installation

1. Download the archive from [Releases](../../releases).
2. Unpack it into the game folder, next to `Storm.exe`.
3. Start the game as usual.

To uninstall, delete `d3d9.dll`, `StormModLoader.dll` and the `Mods` folder. Game files are never modified.

Game version 1.0.147 is required. On other versions the mods switch themselves off without breaking anything.
On laptops with two GPUs, fullscreen mode is noticeably faster than borderless.

### Building

Visual Studio 2022+ (x86 C++ tools) and CMake 3.24+. The repository is meant to live inside the game folder,
so the built DLLs land where the game expects them.

```
cmake -S . -B build -A Win32
cmake --build build --config Release
```

`Mods\StormBench` and `tools\bench.ps1` are developer tools for FPS measurements; players don't need them.

### License

Mod code is licensed under [MIT](LICENSE). The game and its files are not included; all rights to them belong
to their owners.
