# Task for the DOORS coding assistant

Implement the selected **B** visual direction using this complete package. This supersedes the prior text-only launcher. The user explicitly prefers colored icons and fixed group frames. Do not revert to A or introduce a third direction.

Read START_HERE.md, INTEGRATION.md, b_ui_layout.json and the overview images. Use base_ui_layout.json only for boot, lock and open. Inspect the actual repository instructions, UI framework/version, fonts, image-resource pipeline, orientation, lifecycle and input handling before editing.

1. Integrate native background resources for both orientations: portrait 568 × 1232 and landscape 1232 × 568. Use the clean backgrounds, never baked screenshots as live UI.
2. Add the supplied nine portal icons and system glyphs through the existing build pipeline. Prefer offline PNG/image conversion over runtime SVG. Retain their frames, subdued colors and transparent margins.
3. Build the B grouped launcher with Connections: Radio/Mesh/Network; Workspace: Tools/AI/Files; Device & play: Games/Settings/Apps. Bind these conceptual keys to real existing app routes. Preserve input, focus and theme infrastructure. No pretend apps.
4. Build the B system menu using real Wi-Fi/Bluetooth/radio/sound state, brightness and volume providers, existing detail screens, Lock and a safe Power action menu. Do not invent telemetry or unsupported hardware functions. Disable unavailable controls according to project conventions.
5. Render time/date, battery, status, selection and values dynamically. All example values in this package are placeholders. Reproduce SVG/JSON placements using actual font baselines, and check localized labels for clipping. Keep the supplied quiet photographic background visible beneath the panels.
6. Retain boot/lock/open direction and existing authentication behavior. No door-opening animation frames are included; start with a direct transition. Do not add realtime 3D/blur.
7. Respect image/widget lifetimes, avoid decode in repaint callbacks, load only current resources and measure memory for any preloading. Do not change radio protocols or unrelated service behavior.
8. Validate builds, both orientations, focus navigation, app routes, slider errors and safe power behavior with relevant existing checks. Report any tests that need physical K230 access.

Complete the implementation supported by the actual repository, then summarize files changed, validations and concrete remaining blockers. Do not claim this asset package itself is tested firmware.
