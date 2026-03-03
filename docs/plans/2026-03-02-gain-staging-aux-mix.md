# Gain Staging + OUT/AUX Mix Knob Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix per-engine level inconsistencies and replace the 3-way output_mode enum with a continuous aux_mix crossfade knob.

**Architecture:** All changes are isolated to `src/dsp/plaits_plugin.cpp` and `src/module.json`. A static `kGainTable[24]` is applied after `voice.Render()` during the float-conversion loop. The `output_mode` enum and `volume` parameter are removed; a single `aux_mix` float replaces them on knob 8. Percussion engines (21, 22, 23) bypass `aux_mix` and always output their OUT signal.

**Tech Stack:** C++14, Plaits/stmlib vendored DSP (do not modify), move-anything plugin_api_v2, Docker/zig for aarch64 cross-compilation.

---

## Critical Pre-Work: Engine Index Correction

The spec's gain table uses **incorrect indices** — they were written against an older Plaits firmware that didn't include the engine2 set. The engine2 engines (VA VCF, Phase Dist, 6×Op, Wave Terrain, String Machine, Chiptune) were prepended to the front in the current firmware.

**Source of truth:** `plaits/dsp/voice.cc Voice::Init()` — registration order = index.

Confirmed mapping (voice.cc lines 40–65):

| Index | Name in kEngineNames | Engine Class              | Plaits out_gain | Plaits aux_gain |
|-------|---------------------|---------------------------|-----------------|-----------------|
| 0     | VA VCF              | VirtualAnalogVCFEngine    | 1.0f            | 1.0f            |
| 1     | Phase Dist          | PhaseDistortionEngine     | 0.7f            | 0.7f            |
| 2     | 6-Op I              | SixOpEngine               | 1.0f            | 1.0f            |
| 3     | 6-Op II             | SixOpEngine               | 1.0f            | 1.0f            |
| 4     | 6-Op III            | SixOpEngine               | 1.0f            | 1.0f            |
| 5     | Wave Terr           | WaveTerrainEngine         | 0.7f            | 0.7f            |
| 6     | Str Mach            | StringMachineEngine       | 0.8f            | 0.8f            |
| 7     | Chiptune            | ChiptuneEngine            | 0.5f            | 0.5f            |
| 8     | V. Analog           | VirtualAnalogEngine       | 0.8f            | 0.8f            |
| 9     | Waveshape           | WaveshapingEngine         | 0.7f            | 0.6f            |
| 10    | FM                  | FMEngine (two-op)         | 0.6f            | 0.6f            |
| 11    | Grain               | GrainEngine               | 0.7f            | 0.6f            |
| 12    | Additive            | AdditiveEngine            | 0.8f            | 0.8f            |
| 13    | Wavetable           | WavetableEngine           | 0.6f            | 0.6f            |
| 14    | Chord               | ChordEngine               | 0.8f            | 0.8f            |
| 15    | Speech              | SpeechEngine              | -0.7f (lim)     | 0.8f            |
| 16    | Swarm               | SwarmEngine               | -3.0f (lim)     | 1.0f            |
| 17    | Noise               | NoiseEngine               | -1.0f (lim)     | -1.0f (lim)     |
| 18    | Particle            | ParticleEngine            | -2.0f (lim)     | 1.0f            |
| 19    | String              | StringEngine              | -1.0f (lim)     | 0.8f            |
| 20    | Modal               | ModalEngine               | -1.0f (lim)     | 0.8f            |
| 21    | Bass Drum           | BassDrumEngine            | 0.8f            | 0.8f            |
| 22    | Snare Drum          | SnareDrumEngine           | 0.8f            | 0.8f            |
| 23    | Hi-Hat              | HiHatEngine               | 0.8f            | 0.8f            |

Notes on negative gains: Plaits uses negative gain in RegisterInstance as a flag to activate its internal stmlib Limiter. Our gain_table is applied AFTER voice.Render() is fully complete (post-limiter), so multiplying a limited signal by > 1.0f is safe — the final int16 clamp in render_block prevents overflow.

**Percussion AUX content** (confirmed from engine source):
- BassDrumEngine: OUT = 808-style analog bass drum, AUX = synthetic/FM bass drum
- SnareDrumEngine: OUT = analog snare, AUX = synthetic snare/clap
- HiHatEngine: OUT = open hi-hat, AUX = closed hi-hat

All three percussion engines carry a *different drum voice* on AUX — not a blend of the same voice. Therefore `aux_mix` must be bypassed for all three.

---

## Task 1: Add Per-Engine Gain Compensation Table

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp`

### Step 1: Add kGainTable constant

In `plaits_plugin.cpp`, after the `kEngineLabels` array (around line 226), add:

```cpp
// Per-engine gain compensation applied after voice.Render().
// Indexed by engine registration order in plaits/dsp/voice.cc Voice::Init().
// Corrected from spec: engine2 engines occupy indices 0-7, not 14-23.
static constexpr float kGainTable[24] = {
    1.0f,   //  0  VA VCF      (VirtualAnalogVCFEngine)
    1.0f,   //  1  Phase Dist  (PhaseDistortionEngine)
    2.8f,   //  2  6-Op I      (SixOpEngine)
    2.8f,   //  3  6-Op II     (SixOpEngine)
    2.8f,   //  4  6-Op III    (SixOpEngine)
    1.8f,   //  5  Wave Terr   (WaveTerrainEngine)
    1.5f,   //  6  Str Mach    (StringMachineEngine)
    1.2f,   //  7  Chiptune    (ChiptuneEngine)
    1.0f,   //  8  V. Analog   (VirtualAnalogEngine)
    1.0f,   //  9  Waveshape   (WaveshapingEngine)
    2.5f,   // 10  FM          (FMEngine, two-op)
    1.1f,   // 11  Grain       (GrainEngine)
    1.0f,   // 12  Additive    (AdditiveEngine)
    1.2f,   // 13  Wavetable   (WavetableEngine)
    0.8f,   // 14  Chord       (ChordEngine)
    1.3f,   // 15  Speech      (SpeechEngine)
    0.9f,   // 16  Swarm       (SwarmEngine)
    1.2f,   // 17  Noise       (NoiseEngine)
    1.2f,   // 18  Particle    (ParticleEngine)
    1.0f,   // 19  String      (StringEngine)
    1.0f,   // 20  Modal       (ModalEngine)
    1.0f,   // 21  Bass Drum   (BassDrumEngine)
    1.0f,   // 22  Snare Drum  (SnareDrumEngine)
    1.0f,   // 23  Hi-Hat      (HiHatEngine)
};
```

### Step 2: Apply gain in render_block

In `render_block`, the output conversion loop currently reads:
```cpp
const float gain = inst->volume;
for (int i = 0; i < frames; i++) {
    float out_f = -inst->frame_buf[i].out / 32767.0f;
    float aux_f = -inst->frame_buf[i].aux / 32767.0f;
    ...
```

Multiply `kGainTable[inst->engine]` into both signals during the float conversion:
```cpp
const float vol = inst->volume;
const float eg = kGainTable[inst->engine];
for (int i = 0; i < frames; i++) {
    float out_f = -inst->frame_buf[i].out / 32767.0f * eg;
    float aux_f = -inst->frame_buf[i].aux / 32767.0f * eg;
    ...
    l *= vol;
    r *= vol;
```

This inserts the gain compensation after voice.Render() and before the volume and output-mode scaling.

### Step 3: Verify build compiles

```bash
cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh
```

Expected output ends with:
```
Build complete: dist/plaits-module.tar.gz
```

No errors or warnings about kGainTable. If compile fails, check that `constexpr` is valid in C++14 (it is for arrays of literals).

### Step 4: Commit

```bash
cd ~/move-everything-parent/move-anything-plaits
git add src/dsp/plaits_plugin.cpp
git commit -m "feat: add per-engine gain compensation table

Corrects level imbalance between engine families. Six-Op (2.8x),
FM two-op (2.5x), Wave Terrain (1.8x), and String Machine (1.5x)
were significantly quieter than VA/analog engines.

Gain table indexed against voice.cc registration order — corrected
from spec which used pre-engine2 index ordering."
```

---

## Task 2: Replace output_mode with aux_mix

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp`
- Modify: `src/module.json`

### Step 1: Remove output_mode constants and add aux_mix to instance struct

Remove from constants section (top of file):
```cpp
static const int   OUTPUT_MONO   = 0;
static const int   OUTPUT_STEREO = 1;
static const int   OUTPUT_AUX    = 2;
```

In `plaits_instance_t`, remove:
```cpp
int   output_mode;
```

Add in its place:
```cpp
float aux_mix;
```

### Step 2: Update create_instance defaults

Remove:
```cpp
inst->output_mode        = OUTPUT_MONO;
```

Add:
```cpp
inst->aux_mix            = 0.0f;
```

Also change `inst->volume` default from `0.7f` to `0.85f` (spec value):
```cpp
inst->volume             = 0.85f;
```

### Step 3: Update set_param

Remove the `output_mode` block:
```cpp
} else if (strcmp(key, "output_mode") == 0) {
    if (strcmp(val, "stereo") == 0)    inst->output_mode = OUTPUT_STEREO;
    else if (strcmp(val, "aux") == 0)  inst->output_mode = OUTPUT_AUX;
    else                               inst->output_mode = OUTPUT_MONO;
}
```

Add `aux_mix` handling (after `fm_amount` block):
```cpp
} else if (strcmp(key, "aux_mix") == 0) {
    float v = (float)atof(val);
    inst->aux_mix = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}
```

Also remove the `volume` block from `set_param` (volume is now internal-only):
```cpp
} else if (strcmp(key, "volume") == 0) {
    float v = (float)atof(val);
    inst->volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}
```

### Step 4: Update get_single_param

Remove:
```cpp
if (strcmp(key, "output_mode") == 0) {
    const char* mode = inst->output_mode == OUTPUT_STEREO ? "stereo"
                     : inst->output_mode == OUTPUT_AUX    ? "aux"
                     :                                       "mono";
    return snprintf(buf, buf_len, "%s", mode);
}
```

Add:
```cpp
if (strcmp(key, "aux_mix") == 0)
    return snprintf(buf, buf_len, "%.3f", inst->aux_mix);
```

Remove volume from get_single_param:
```cpp
if (strcmp(key, "volume") == 0)
    return snprintf(buf, buf_len, "%.3f", inst->volume);
```

### Step 5: Update get_param ui_hierarchy and chain_params strings

In `get_param`, update the `ui_hierarchy` JSON string:
- In `"knobs"`: replace `"volume"` with `"aux_mix"`
- In `"params"`: replace the volume entry with aux_mix, remove output_mode entry

New knobs array in ui_hierarchy:
```
"knobs":[\"engine\",\"harmonics\",\"timbre\",\"morph\",\"decay\",\"lpg_colour\",\"fm_amount\",\"aux_mix\"]
```

New params list in ui_hierarchy (remove output_mode and volume entries, add aux_mix):
```
{\"key\":\"aux_mix\",\"label\":\"Mix\"}
```

In `chain_params`, replace the `output_mode` entry and the `volume` entry with `aux_mix`:

Remove:
```
"{\"key\":\"output_mode\",\"name\":\"Output\",\"type\":\"enum\","
 "\"options\":[\"mono\",\"stereo\",\"aux\"],\"default\":\"mono\"},"
```
And:
```
"{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\","
 "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.7}"
```

Add (in the position where volume was — knob 8):
```
"{\"key\":\"aux_mix\",\"name\":\"Mix\",\"type\":\"float\","
 "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.0}"
```

### Step 6: Update render_block output routing

Remove the old switch statement:
```cpp
float l, r;
switch (inst->output_mode) {
    case OUTPUT_STEREO:
        l = out_f;
        r = aux_f;
        break;
    case OUTPUT_AUX:
        l = r = aux_f;
        break;
    case OUTPUT_MONO:
    default:
        l = r = out_f;
        break;
}
```

Replace with aux_mix crossfade:
```cpp
float blended = out_f * (1.0f - inst->aux_mix) + aux_f * inst->aux_mix;
float l = blended;
float r = blended;
```

### Step 7: Update module.json

In `src/module.json`:

1. In `"knobs"` array: replace `"volume"` with `"aux_mix"`
2. In `"params"` array:
   - Remove the `output_mode` enum param object entirely
   - Remove the `volume` float param object entirely
   - Add after `fm_amount`:
     ```json
     {
         "key": "aux_mix",
         "label": "Mix",
         "type": "float",
         "min": 0.0,
         "max": 1.0,
         "default": 0.0,
         "step": 0.02,
         "unit": "%"
     }
     ```

### Step 8: Build and verify

```bash
cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh
```

Expected: clean compile, `Build complete: dist/plaits-module.tar.gz`.

Verify correctness mentally:
- `aux_mix = 0.0`: `blended = out_f * 1.0 + aux_f * 0.0 = out_f` → identical to old `OUTPUT_MONO`
- `aux_mix = 1.0`: `blended = out_f * 0.0 + aux_f * 1.0 = aux_f` → identical to old `OUTPUT_AUX`
- `aux_mix = 0.5`: equal blend of both signals

### Step 9: Commit

```bash
cd ~/move-everything-parent/move-anything-plaits
git add src/dsp/plaits_plugin.cpp src/module.json
git commit -m "feat: replace output_mode enum with aux_mix crossfade knob

Removes 3-way mono/stereo/aux enum. Adds continuous 0.0-1.0 mix
knob blending OUT and AUX signals. aux_mix=0.0 is identical to
previous mono behavior. Volume fixed at 0.85 internally."
```

---

## Task 3: Percussion Engine AUX Bypass

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp`

### Step 1: Understand the problem

All three percussion engines route a *separate drum voice* to AUX (not a variation of OUT):
- Engine 21 Bass Drum: OUT = analog bass drum, AUX = synthetic bass drum
- Engine 22 Snare Drum: OUT = analog snare, AUX = synthetic snare/clap
- Engine 23 Hi-Hat: OUT = open hi-hat, AUX = closed hi-hat

Setting `aux_mix > 0` on these engines would blend two different drum voices together, not produce the intended crossfade behavior. These engines must always output OUT only, regardless of the `aux_mix` knob position.

### Step 2: Add percussion bypass to render_block

Replace the crossfade line added in Task 2 Step 6:

```cpp
float blended = out_f * (1.0f - inst->aux_mix) + aux_f * inst->aux_mix;
float l = blended;
float r = blended;
```

With:

```cpp
float l, r;
const bool is_percussion = (inst->engine >= 21 && inst->engine <= 23);
if (is_percussion) {
    // Percussion engines use separate voices on OUT and AUX.
    // Always output OUT only — aux_mix is not meaningful here.
    l = r = out_f;
} else {
    float blended = out_f * (1.0f - inst->aux_mix) + aux_f * inst->aux_mix;
    l = r = blended;
}
```

### Step 3: Build and verify

```bash
cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh
```

Expected: clean compile. No changes to module.json needed — the knob still exists and its value is stored, just ignored for percussion engines.

### Step 4: Commit

```bash
cd ~/move-everything-parent/move-anything-plaits
git add src/dsp/plaits_plugin.cpp
git commit -m "feat: bypass aux_mix for percussion engines (21-23)

Bass Drum, Snare Drum, and Hi-Hat engines carry distinct drum voices
on OUT and AUX, not variations of the same voice. Blending them via
aux_mix produces unintended results. These engines now always output
OUT only, ignoring the Mix knob."
```

---

## Post-Implementation Checklist

Manual verification on hardware:

- [ ] `aux_mix = 0.0` on any melodic engine sounds identical to v0.2.0 default behavior
- [ ] `aux_mix = 1.0` on Chord (14) produces AUX signal (chord voicing variant)
- [ ] `aux_mix = 0.5` on Speech (15) blends voiced and unvoiced output
- [ ] 6-Op engines (2, 3, 4) are perceptually similar in volume to VA VCF (0)
- [ ] FM engine (10) is perceptually similar in volume to VA VCF (0)
- [ ] Bass Drum (21) ignores aux_mix — output unchanged regardless of knob
- [ ] Snare Drum (22) ignores aux_mix — output unchanged regardless of knob
- [ ] Hi-Hat (23) ignores aux_mix — output unchanged regardless of knob
- [ ] `module.json` loads without error (valid JSON, no output_mode or volume params)
- [ ] Chain patches (Plaits Pad, Plaits Pluck, Plaits + Reverb) load without error
