# Doors roadmap

What DOORS is working on next, in priority order. The product owner sets the
priorities. This page carries no dates, version numbers or promises: an item
is ready when it has been built and tested, and the release notes say what
each release actually contains.

The current release is **Doors 0.3.5**
([release notes](releases/v0.3.5.md)). What works today is in the
[README](../README.md#highlights); known limitations are in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

**How to read this page.** The order is the order of priority, not a
schedule, and not every item has started. Each item is marked:

- **Ongoing**: work has started.
- **Planned**: agreed as a direction; work has not started.

Bug fixes and stability improvements continue alongside all of these
priorities.

## Priorities

### 1. Vision: our own trained AI model (Ongoing)

Doors 0.3.5 shipped without a detector model; 0.3.6 ships the project's own
first one, R0, as a beta ([VISION.md](apps/VISION.md),
[MODEL_LICENSES.md](../MODEL_LICENSES.md)). The aim is a detector trained by
the DOORS project, on data whose licences allow it to be shipped.

Done so far, on a research branch: a reproducible training pipeline, the
first training dataset (R0) from licence-checked COCO and Open Images
images with a held-out test split, the R0 training run (completed
2026-10-09), its evaluation, including small and distant objects, and its
export to the K230's KPU format. R0 ran on unit A in a bench A/B
comparison. It misses many small and distant vehicles.

Now: R0 ships in Doors 0.3.6 (2026-10-09) as an experimental beta
("R0 · Beta"), under Apache-2.0. It passed a fresh-card smoke test on
hardware. A second training round (R1-A), aimed at the cars R0 misses in
street scenes, is running; no result from it is claimed here.

### 2. DeskBuddy development (Planned)

DeskBuddy, the desk companion, is in the image today with its BUDDY, GUARD
and NIGHT modes ([DESKBUDDY.md](apps/DESKBUDDY.md)). Next: more personality,
better animations, and improved BUDDY, GUARD and NIGHT experiences. The
detailed scope will be decided separately.

### 3. RIFT development (Planned)

RIFT already provides MeshCore direct messages with delivery
acknowledgement, channels, nodes, a map, repeater control, an RX log and the
radio's status ([RIFT.md](apps/RIFT.md)). Next: extend the mesh
communication features and improve everyday usability. Candidates come from
the open RIFT and meshcored items in [KNOWN_ISSUES.md](KNOWN_ISSUES.md), for
example per-channel flood scopes, the command line (`/msg`, `/join`) that
RIFT's design describes, and telling a stalled mesh service apart from a
quiet one.

### 4. Vision improvements (Planned)

Once the new model is integrated (priority 1), improve detection, tracking
and traffic counting based on how that model performs. This is separate
from the first model integration and starts from its measured results.

### 5. New and improved games (Ongoing)

- **Poker**: an offline Texas Hold'em game against three computer players,
  for play chips only, is in the Games folder
  ([#71](https://github.com/KrakenSaten/K230/pull/71); tested on unit B,
  docs/apps/poker/VALIDATION.md). Next: keep the table across a rotation
  and across attaching or detaching the keyboard. Today a rotation
  restarts the shell and Poker deals a fresh table. Planned.
- **Existing games**: improvements to Fleet, Radar, Timber, Solitaire,
  Blackjack and 2048. Planned.

## History

Earlier roadmap phases, status tables and per-release summaries up to
Doors 0.3.5 are kept in [ROADMAP_HISTORY.md](ROADMAP_HISTORY.md). Release
notes for every release are in [releases/](releases/).
