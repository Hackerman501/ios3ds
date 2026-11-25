# Stock app launch matrix

One tap per icon, in QEMU on a fresh `build/sd.img` built from this branch,
with `--icount 2` (hardware timing). `emu/soak.py --unlock-first --no-extras
--crashlogs --targets …` wakes the device, takes a home-screen reference, then
for every point taps, waits 12 s, takes a screenshot, presses HOME and waits
until the home screen is stable again. "app diff" is the mean brightness
difference between the app screenshot and the reference; "home diff" is the
same for the screenshot after HOME (over 12 means "not back on the home
screen"). The run fails on a panic, a crash message, an md0 error or a second
`[input] connected to SpringBoard` line.

Results: see [re-log.md](re-log.md) section 91 (the table is kept there with the
investigation). Screenshots and the full log are in `build/emu/apps-survey/`.
The App Store opens and shows its loading error; store servers for iOS 4 are
long gone, so that error is expected. "Loading" without a crash means the app
itself did not fail; a game crashing after installation is the OpenGL ES side
([gpu.md](gpu.md)), not this launch path.
