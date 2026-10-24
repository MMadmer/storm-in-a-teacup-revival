# StormBench

Development-only benchmark for Storm.exe v1.0.147. It stays inert unless the `STORM_BENCH` environment
variable is set and is disabled in `Mods.ini` by default; `tools\bench.ps1` enables it for a single run.

An armed run starts a race from the main menu, waits for the warm-up, records frame times for the requested
duration, writes `Benchmark-<label>.txt` next to the DLL and closes the game.

`STORM_BENCH` is a `key=value` list separated by `;`:

| Key | Meaning |
| --- | --- |
| `label` | Report name suffix. |
| `track`, `glider` | Menu indices of the track and the glider. |
| `warmup`, `duration` | Seconds before and during the measurement. |
| `profile` | `1` adds a main-thread sampling profile (self, inclusive, external callers, threads). |
| `drive` | `1` holds the accelerator during the race. |
| `screenshot` | `1` saves the last rendered back buffer as a BMP. |
