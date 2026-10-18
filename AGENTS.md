# Editor Waveform

Geode mod that draws the main song's waveform and spectrogram on the Geometry Dash editor grid.
Hooks/settings live in `src/main.cpp`, audio in `src/waveform.*`, and rendering in
`src/timeline.*`. Offline spectral analysis and rasterization live in
`src/spectrogram.*`; KISS FFT and palette attribution live in `third_party/`.
`references/` contains the original mod for context.
Keep player-facing docs short; development instructions belong here.

Eclipse integration uses one Editor Waveform tab. Its Settings button opens the
compact Cocos panel in `src/settings_popup.*`, using stock Eclipse's public API.
Match the Song Subtitles menu chrome: native GD popup assets, a left-aligned gold
heading, a flat dark viewport, alternating shaded rows, and compact white labels.
Keep appearance and analysis parameters in one scrolling column with independent
Waveform and Spectrogram sections, both expanded when the panel opens. Keep show
and camera-lock toggles in Eclipse and Geode settings, outside this panel; do not
create style or choice menu tabs. Numeric controls pair a slider with an editable
value; choices use arrow fields. Each value has a reset arrow using its Geode
default and an info button for its description, without hover tooltips. FFT size
snaps to powers of two. Coordinate/height sliders cover the full saved range with
finer control near zero. Collapsed and clipped controls must not receive input.
Holding Shift temporarily hides the panel and backdrop for a grid preview;
releasing it restores the same sections and scroll position, with editing blocked
while hidden.
Check the Eclipse overlay closes before the Cocos modal receives input.

## Build

Requires Geode CLI, an SDK matching `mod.json`, CMake 3.25+, and C++23.
Set `GEODE_SDK` to the installed SDK; use its supplied bindings, cocos, and FMOD.

```sh
geode build --config RelWithDebInfo
```

Output: `build/asdalion.editor-waveform.geode`. The CLI installs to its active
profile; add `-- -DGEODE_DONT_INSTALL_MODS=ON` for packaging only.
CI builds universal macOS and Windows x64 packages.

## Tests

Core tests run without Geode or a game installation:

```sh
cmake -S . -B build/tests -DEDITOR_WAVEFORM_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests --config Debug --parallel
ctest --test-dir build/tests -C Debug --output-on-failure
```

Spectral tests cover all window sizes, stereo phase, boundary padding, axis
mapping, rasterization and bounded temporal coarsening. To benchmark 5/30-minute
stereo songs at FFT sizes 512/8192, build Release tests and run
`build/tests-release/spectrogram_tests --benchmark`. Spectral storage reserves at
most 64 MiB once and coarsens in place; no complete PCM buffer is retained.
Settings-only display changes must not restart decoding or FFT analysis. Loader
requests are serialized, cancellation-aware, and generation-checked; rebuilds
retain the finished waveform and release old spectral storage.

Optional macOS audio tests also need `GEODE_SDK`, `EDITOR_WAVEFORM_FMOD_TESTS=ON`,
and `EDITOR_WAVEFORM_FMOD_LIBRARY` pointing to the game's original FMOD dylib.
For hook/UI changes, check playback, playtesting, song/offset changes, settings
persistence, and loading with and without Eclipse in-game. Windows gameplay
still needs verification. Run `git diff --check` before finishing.
