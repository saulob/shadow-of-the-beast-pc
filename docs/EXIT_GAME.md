# Exit Game

The port adds an **Exit Game** command to the game's shared menu footer (`UMenuInfoBarController`) on the **Map** and **Pause** screens, bound to the left face button (**X** on Xbox prompts, **Square** on PlayStation prompts).

- **Footer integration**: injected natively via `UMenuInfoBarController::AddButton` (`FrontendInputButton::Feed`, `0x8`) after both `UMenuInfoBarController::Tick` and `UFrontendScreenController::RefreshInfoBar`, using the game's own `HotkeyButtonBP` widget and prompt textures. Skipped automatically if slot `0x8` is already occupied or the InfoBar is hidden.
- **Confirmation popup**: pressing X/Square in Map or Pause opens a centered host confirmation modal (`SotbOverlay`) so accidental presses never close the game immediately. While open, guest input is held neutral.
- **Localization**: both the native footer label and the confirmation popup follow the launcher's **Menu & Subtitles** (`text_language`) setting across all 19 supported cultures, using terminology from the game's `Game.locres` files.
- **Menu Button Layout & Controller Prompts**: Western uses A/Cross to confirm and B/Circle to cancel; Original uses B/Circle to confirm and A/Cross to cancel. Visual hints follow both settings automatically.
- **Clean shutdown**: confirming waits for any active writable `sceSaveData` mount to unmount, then requests clean window shutdown via `sotbgpu_request_close()`.
