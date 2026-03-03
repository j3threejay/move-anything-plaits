# Per-Engine Defaults + State Reset on Engine Switch

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** When switching engines, reset voice state to prevent bleed from the previous engine, apply safe per-engine default parameters on first visit, and ensure the shadow UI reflects the new values.

**Architecture:** All changes are in `src/dsp/plaits_plugin.cpp` only. On engine switch in `set_param`, we (1) re-initialize the Plaits voice to clear all internal state, (2) apply first-visit defaults from a static table. Task 3 (shadow UI refresh) requires no code — it is already handled by the existing `refreshes_labels:true` mechanism combined with the fact that `getSlotParam` reads live from `get_param` with no JS-side caching.

**Tech Stack:** C++14, Plaits/stmlib vendored DSP (do not modify), plugin_api_v2.

---

## Research Findings (Read Before Implementing)

### Why voice.Init() is the right reset

`plaits::Voice` has no `Reset()` method. It has only: `Init()`, `ReloadUserData()`, `Render()`, `active_engine()`.

`voice.cc Voice::Render()` already calls `e->Reset()` on the new engine and `out_post_processor_.Reset()` when it detects an engine change. However it does NOT reset:
- `aux_post_processor_` — can bleed resonance
- `lpg_envelope_` — can carry envelope state
- `decay_envelope_` — can carry decay state

Re-calling `voice.Init(&allocator)` (after re-initializing the allocator) resets everything cleanly. This is the only correct approach without modifying vendor code.

### Why Task 3 requires no code

`shadow_ui.js` calls `invalidateKnobContextCache()` after every engine change (because `engine` has `"refreshes_labels":true` in `chain_params`). After cache invalidation, the shadow UI reads knob values via `getSlotParam(slot, key)` → `shadow_get_param()` → `get_param()` on the plugin — no JS-side value cache. So as long as defaults are written to the instance fields in `set_param` before the engine change returns, the UI will show the new values automatically on its next tick.

### Confirmed engine indices (from voice.cc Voice::Init registration order)

| Index | Name         | Special default needed |
|-------|--------------|----------------------|
| 2     | 6-Op I       | decay=0.7            |
| 3     | 6-Op II      | decay=0.7            |
| 4     | 6-Op III     | decay=0.7            |
| 7     | Chiptune     | morph=0.0            |
| 19    | String       | morph=0.3, decay=0.6 |
| 21    | Bass Drum    | decay=0.4            |
| 22    | Snare Drum   | decay=0.3            |
| 23    | Hi-Hat       | decay=0.3            |
| all others | —       | all 0.5              |

---

## Task 1: Voice State Reset on Engine Switch

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp`

### Step 1: Add previous_engine tracking to instance struct

In `plaits_instance_t`, add one field to track the previously-set engine index so we can detect engine changes in `set_param`:

```cpp
    // Render frame buffer (plaits::Voice::Frame has interleaved short out/aux)
    plaits::Voice::Frame frame_buf[BLOCK_SIZE];

    // Engine switch tracking
    int  previous_engine;  // detects change in set_param to trigger voice reset
```

### Step 2: Initialize previous_engine in create_instance

After `inst->engine = 0;`, add:

```cpp
    inst->previous_engine    = 0;
```

### Step 3: Add voice reset helper

Add a small static helper function just before `set_param` (around line 263):

```cpp
// ──────────────────────────────────────────────────────────────────────────
// reset_voice  —  reinitialize Plaits voice to clear all internal state.
// Called on engine switch. Does NOT change any plugin parameters.
// Voice::Init re-registers all engines and resets post-processors, envelopes,
// and internal state. Allocator must be re-initialized first (reuses same buffer).
// ──────────────────────────────────────────────────────────────────────────

static void reset_voice(plaits_instance_t* inst) {
    inst->allocator.Init(inst->shared_buffer, BUFFER_SIZE);
    inst->voice.Init(&inst->allocator);
}
```

### Step 4: Call reset_voice when engine changes in set_param

In the `"engine"` branch of `set_param`, after setting `inst->engine`, detect a change and reset:

Current code:
```cpp
    if (strcmp(key, "engine") == 0) {
        // Accept engine name string (enum) or numeric index (legacy)
        int v = -1;
        for (int i = 0; i < kNumEngines; i++) {
            if (strcmp(val, kEngineNames[i]) == 0) { v = i; break; }
        }
        if (v < 0) v = atoi(val);  // fallback: numeric string
        inst->engine = (v < 0) ? 0 : (v >= kNumEngines) ? kNumEngines - 1 : v;
    }
```

Replace with:
```cpp
    if (strcmp(key, "engine") == 0) {
        // Accept engine name string (enum) or numeric index (legacy)
        int v = -1;
        for (int i = 0; i < kNumEngines; i++) {
            if (strcmp(val, kEngineNames[i]) == 0) { v = i; break; }
        }
        if (v < 0) v = atoi(val);  // fallback: numeric string
        int new_engine = (v < 0) ? 0 : (v >= kNumEngines) ? kNumEngines - 1 : v;
        if (new_engine != inst->previous_engine) {
            inst->engine = new_engine;
            inst->previous_engine = new_engine;
            reset_voice(inst);
        }
    }
```

### Step 5: Build and verify

```bash
cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh
```

Expected: `Build complete: dist/plaits-module.tar.gz`

### Step 6: Commit

```bash
cd ~/move-everything-parent/move-anything-plaits
git add src/dsp/plaits_plugin.cpp
git commit -m "feat: reset voice state on engine switch

Re-initializes Plaits voice (allocator + voice.Init) when engine
changes. Clears aux_post_processor_, lpg_envelope_, and decay_envelope_
which voice.cc does not reset on engine switch, preventing resonance
bleed from Modal/String into subsequent engines."
```

---

## Task 2: Per-Engine Default Parameter Values (First-Visit Only)

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp`

### Step 1: Add EngineDefaults struct and table

After `kGainTable` (around line 258), add:

```cpp
// ──────────────────────────────────────────────────────────────────────────
// Per-engine default parameter values.
// Applied ONCE when an engine is first visited in a session.
// Subsequent visits restore the last-used values — not re-applied.
// Only covers: harmonics, timbre, morph, decay, lpg_colour.
// fm_amount, timbre_mod, morph_mod, aux_mix are NOT reset on engine switch.
// ──────────────────────────────────────────────────────────────────────────

struct EngineDefaults {
    float harmonics, timbre, morph, decay, lpg_colour;
};

// Indexed by engine registration order from plaits/dsp/voice.cc Voice::Init().
static constexpr EngineDefaults kEngineDefaults[24] = {
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  //  0  VA VCF
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  //  1  Phase Dist
    { 0.5f, 0.5f, 0.5f, 0.7f, 0.5f },  //  2  6-Op I      (longer decay suits pads)
    { 0.5f, 0.5f, 0.5f, 0.7f, 0.5f },  //  3  6-Op II
    { 0.5f, 0.5f, 0.5f, 0.7f, 0.5f },  //  4  6-Op III
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  //  5  Wave Terr
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  //  6  Str Mach
    { 0.5f, 0.5f, 0.0f, 0.5f, 0.5f },  //  7  Chiptune    (morph=0 prevents self-osc)
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  //  8  V. Analog
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  //  9  Waveshape
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 10  FM
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 11  Grain
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 12  Additive
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 13  Wavetable
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 14  Chord
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 15  Speech
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 16  Swarm
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 17  Noise
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 18  Particle
    { 0.5f, 0.5f, 0.3f, 0.6f, 0.5f },  // 19  String      (lower morph, longer decay)
    { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },  // 20  Modal
    { 0.5f, 0.5f, 0.5f, 0.4f, 0.5f },  // 21  Bass Drum   (slightly shorter decay)
    { 0.5f, 0.5f, 0.5f, 0.3f, 0.5f },  // 22  Snare Drum  (shorter decay)
    { 0.5f, 0.5f, 0.5f, 0.3f, 0.5f },  // 23  Hi-Hat      (shorter decay)
};
```

### Step 2: Add engine_visited array to instance struct

In `plaits_instance_t`, after `previous_engine`:

```cpp
    int  previous_engine;  // detects change in set_param to trigger voice reset
    bool engine_visited[24]; // true after first visit; prevents re-applying defaults
```

### Step 3: Initialize engine_visited in create_instance

After `inst->previous_engine = 0;`, add:

```cpp
    // Mark engine 0 as already visited — defaults applied above in create_instance
    memset(inst->engine_visited, 0, sizeof(inst->engine_visited));
    inst->engine_visited[0] = true;
```

### Step 4: Apply defaults on first visit in set_param

In the engine-change block added in Task 1 (inside the `if (new_engine != inst->previous_engine)` block), after `reset_voice(inst)`, apply first-visit defaults:

```cpp
        if (new_engine != inst->previous_engine) {
            inst->engine = new_engine;
            inst->previous_engine = new_engine;
            reset_voice(inst);
            // Apply per-engine defaults on first visit only.
            // Subsequent visits restore last-used values.
            if (!inst->engine_visited[new_engine]) {
                inst->engine_visited[new_engine] = true;
                const EngineDefaults& d = kEngineDefaults[new_engine];
                inst->harmonics  = d.harmonics;
                inst->timbre     = d.timbre;
                inst->morph      = d.morph;
                inst->decay      = d.decay;
                inst->lpg_colour = d.lpg_colour;
            }
        }
```

### Step 5: Build and verify

```bash
cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh
```

Expected: `Build complete: dist/plaits-module.tar.gz`

### Step 6: Commit

```bash
cd ~/move-everything-parent/move-anything-plaits
git add src/dsp/plaits_plugin.cpp
git commit -m "feat: apply per-engine default parameters on first visit

Adds kEngineDefaults[24] table with safe starting values per engine.
Defaults applied once on first visit to each engine in a session;
subsequent visits restore last-used values so in-progress sounds
are not lost when switching away and returning.

Key overrides: Chiptune morph=0 (prevents self-osc), Six-Op decay=0.7
(pad-suitable), String morph=0.3/decay=0.6, percussion shorter decay."
```

---

## Task 3: Shadow UI Refresh — No Code Required

**This task is already handled automatically.** Here is why:

1. The `engine` parameter has `"refreshes_labels":true` in `chain_params` (already present in the existing plugin code at line ~404).

2. When the user changes the engine knob, `shadow_ui.js` calls `invalidateKnobContextCache()` after the engine set (lines 4837, 4858, 5287, 5316 in `shadow_ui.js`).

3. `getSlotParam()` in `shadow_ui.js` calls `shadow_get_param()` → `get_param()` directly with no JS-side value cache (confirmed by reading `getSlotParam` implementation at line 1406).

4. Therefore: defaults written to `inst->harmonics`, `inst->timbre`, etc. in `set_param` are immediately visible to the shadow UI on its next tick via `get_param`.

**No code changes needed for Task 3.** The shadow UI will reflect the new default values the next time it reads any of the updated parameter keys.

---

## Deploy

After both commits:

```bash
cd ~/move-everything-parent/move-anything-plaits && ./scripts/install.sh
```

Expected: `Done! Restart Move Anything to load the module.`

---

## Manual Verification Checklist

- [ ] Switch to Chiptune from any engine with morph > 0 — no self-oscillation on load
- [ ] Switch to Modal, hold a long resonant note, switch to VA VCF — no resonance bleed
- [ ] Switch to String engine for the first time — morph at 0.3, decay at 0.6
- [ ] Switch to 6-Op I for the first time — decay at 0.7
- [ ] Dial in a custom sound on VA VCF, switch to Chord, switch back — VA VCF values restored
- [ ] First visit to Bass Drum — decay at 0.4; return after tweaking — last-used values kept
- [ ] Shadow UI knob display updates correctly after engine switch (knob 4 = Morph reflects 0.0 on Chiptune)
