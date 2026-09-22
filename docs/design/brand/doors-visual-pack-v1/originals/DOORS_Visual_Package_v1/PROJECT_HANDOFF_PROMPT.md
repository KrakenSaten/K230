# Prompt for the DOORS project assistant

Integrate the attached DOORS_Visual_Package_v1 into the existing DOORS/K230 project.

The visual direction is approved. Use this package as the source of truth for boot, lock, open and launcher only. Read START_HERE.md, INTEGRATION.md and ui_layout.json first; inspect both overview images. Do not invent a new visual direction or redesign application screens.

Deliver implementation, not just suggestions:

1. Inspect the actual repository, applicable instructions, UI framework/version, existing screen lifecycle, font resources, image decoder/build pipeline, rotation support and input/focus model. Reuse those mechanisms.
2. Import the 8 native files from backgrounds/ through the project's established asset path. Portrait is exactly 568 × 1232; landscape is exactly 1232 × 568. Only load resources for the active orientation. Do not ship masters, reference screen screenshots, fonts or build tools unless the actual runtime needs them.
3. Render clock/date, telemetry, boot status, labels, focus and controls as real UI widgets over the clean backgrounds. Do not use screens/ or static overlay PNGs as production UI. Reproduce the visual placement from SVG/JSON, translating baseline coordinates with real font metrics. Prefer the existing project typography where compatible.
4. Keep the launcher text-only, with no application icons. Use the specified category order and bind it to real existing app IDs. Preserve existing themes, keyboard navigation, touch routing and focus behavior. Do not create pretend apps or nonfunctional menu items.
5. Preserve authentication and lock semantics. The opening door is decorative. Use a direct background switch initially; this package does not contain a registered door animation. Do not misrepresent an opacity fade as a door-opening sequence.
6. Keep loading and decoding out of repaint callbacks, follow object/image lifetime rules and measure memory before preloading additional images. Avoid real-time expensive effects.
7. Validate dimensions, build integration, app routing, both orientation layouts and screen lifecycle with focused checks. Run existing relevant tests where available. Report hardware checks you could not perform rather than claiming they passed.

Do not modify radio behavior, services, protocols or unrelated application logic. Do not hardcode preview values such as 17:24, 86% or 40% in production. Reuse existing time, battery, Wi-Fi and startup state providers. Do not fabricate connected states.

When finished, give a short change summary, the changed files, verification performed and any remaining hardware validation. If the current architecture cannot support part of the integration, implement the supported portion and identify the concrete blocker without silently changing the design.
