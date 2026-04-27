# Moonlight Qt hotkey fork notes

This working tree is a fork of Moonlight Qt 6.1.0 with local stream-time hotkey changes. The relevant keyboard shortcuts are handled by SDL while a streaming session is active, not by Qt's QML `Shortcut` items.

## Requested keybind changes

- Remove `Ctrl+Alt+Shift+S` for toggling the performance stats overlay.
- Rebind the quit/disconnect combo from `Ctrl+Alt+Shift+Q` to `Ctrl+Alt+Shift+-`.
- Rebind the mouse/input ungrab combo from `Ctrl+Alt+Shift+Z` to `Ctrl+Alt+Shift++`.

## Files that control this

- `app/streaming/input/input.cpp`
  - The `SdlInputHandler` constructor populates `m_SpecialKeyCombos`.
  - Change `KeyComboQuit` from `SDLK_q` / `SDL_SCANCODE_Q` to `SDLK_MINUS` / `SDL_SCANCODE_MINUS`.
  - Change `KeyComboUngrabInput` from `SDLK_z` / `SDL_SCANCODE_Z` to `SDLK_PLUS` / `SDL_SCANCODE_EQUALS`.
  - Disable `KeyComboToggleStatsOverlay` by setting `enabled = false`.
- `app/streaming/input/keyboard.cpp`
  - `SdlInputHandler::handleKeyEvent()` checks for the shared `Ctrl+Alt+Shift` modifier chord.
  - It first matches `SDL_Keycode`, then falls back to `SDL_Scancode` so non-Latin layouts still work.
  - `SdlInputHandler::performSpecialKeyCombo()` maps each `KeyCombo` enum to the action it performs.
  - This file does not need code changes for the current fork, but it is the place to modify the matching behavior if the modifier chord itself ever changes.
- `app/streaming/input/input.h`
  - Defines the `KeyCombo` enum and the `m_SpecialKeyCombos` table shape.
  - This file does not need code changes unless adding or removing a whole combo entry.
- `app/gui/StreamSegue.qml`
  - Shows the stream loading tip for disconnecting a session.
  - Update the keyboard hint from `Ctrl+Alt+Shift+Q` to `Ctrl+Alt+Shift+-`.
- `app/gui/SettingsView.qml`
  - Shows the tooltip for the performance overlay setting.
  - Because `Ctrl+Alt+Shift+S` is disabled in this fork, remove that keyboard shortcut from the tooltip text.

## Important detail for `Ctrl+Alt+Shift++`

On a US keyboard, `+` is produced by pressing the equals key with Shift. The binding uses `SDLK_PLUS` for the keycode and `SDL_SCANCODE_EQUALS` for the physical fallback. This matches the visible `+` key meaning while preserving Moonlight's existing scancode fallback behavior.

## Validation checklist

- Search source for stale visible strings:
  - `Ctrl+Alt+Shift+Q`
  - `Ctrl+Alt+Shift+Z`
  - `Ctrl+Alt+Shift+S`
- Build Moonlight Qt after changing the bindings.
- In a streaming session, verify:
  - `Ctrl+Alt+Shift+S` no longer toggles stats.
  - `Ctrl+Alt+Shift+-` disconnects/quits the stream.
  - `Ctrl+Alt+Shift++` toggles mouse/input capture where supported.

## Windows build and bundle commands

For Windows release packages, use the batch scripts in `scripts` from the repository root inside a Qt MSVC command prompt, or an equivalent shell where the selected Qt MSVC `bin` directory is on `%PATH%`.

The script argument is the build configuration, not the architecture. `scripts\build-arch.bat` detects the target architecture from the active Qt `qmake` path.

Typical x64-only development/package build:

```bat
scripts\build-arch.bat Release
```

Full installer bundle flow:

```bat
rem Use the x64 Qt MSVC command prompt or put Qt's msvc*_64\bin on PATH.
scripts\build-arch.bat Release

rem Switch to the arm64 Qt MSVC command prompt or put Qt's msvc*_arm64\bin on PATH.
scripts\build-arch.bat Release

scripts\generate-bundle.bat Release
```

`scripts\generate-bundle.bat Release` requires both `build\build-x64-Release\Moonlight.msi` and `build\build-arm64-Release\Moonlight.msi` to exist. It builds the final setup bundle after both architecture-specific builds are complete.
