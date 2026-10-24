# Video Crash Fix

Fixes the crash a few seconds into the final video of Storm.exe v1.0.147 ("Малиновый Джыдах, эпилог"), so the
ending can finally be watched to the end and the campaign counts as completed.

The crash is a race in the game's video decoder `rk_theora.dll` (v1.0.2.0). When a video player is destroyed,
it raises a stop flag and deletes the critical section its worker thread locks first. A worker thread that gets
scheduled only after that locks freed memory and takes the game down; the final video hits this reliably.

The mod loads `rk_theora.dll` before the game does and patches the worker thread in memory: a worker that
starts after the stop flag is set exits right away. No game file is modified. A copy of `rk_theora.dll` that
already carries the same guard on disk is detected and left alone.
