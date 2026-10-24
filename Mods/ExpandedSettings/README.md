# Expanded Settings

Adds native Video-menu settings to Storm.exe v1.0.147:

- windowed, fullscreen, and borderless display modes;
- vertical synchronization;
- an FPS limit from unlimited to 240 in increments of 30.

Changes remain pending until Apply and are discarded when leaving the page.
Applied values are stored in `ExpandedSettings.ini`.

In exclusive fullscreen, VSync uses the hardware presentation interval, so toggling it resets the device.
In windowed and borderless modes the desktop compositor already prevents tearing, and VSync paces frames
to the vertical blank so it can change without a reset.

The window is laid out again only when the display mode or resolution changes; minimizing, maximizing and
moving it are left to the user, also across a lost-device recovery.

The native Direct3D 9 runtime rewrites its per-device dispatch table, so the mod redirects the game's own
`Present` and `Reset` call sites instead of hooking the device's virtual table. If those call sites are not
found, the mod leaves presentation, the window and the Video menu entirely to the game.
