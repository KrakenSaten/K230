# DOORS app theme - simulator contact sheets (DS §32)

Every screen at 1/3 scale, with a mock radiod answering so the status chip
reads RX. Simulator only (SDL dummy driver): these show layout and colour,
not the panel.

| File | What |
| --- | --- |
| `before-ice-portrait.png`, `before-ice-landscape.png` | master `cdb33ed`, Ice & Ember (the default before §32) |
| `doors-portrait.png`, `doors-landscape.png` | this change, `doors` Normal |
| `doors-outdoor-portrait.png`, `doors-night-portrait.png` | `doors` Outdoor and Night |

Order, portrait: launcher, Controls, System, Notes, Clock, Calculator, RIFT,
Settings, Calendar, Radio, Wave, Fleet, Radar, Timber. Landscape: launcher,
Controls, System, Notes, Clock, Calculator, RIFT, Settings, Calendar, Fleet,
Radar, Timber.

In the landscape "before" sheet the compact bar's chip reads `DX`: the
clipped `RX` of §31.8 item 7.
