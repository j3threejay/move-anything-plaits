# 6-Op FM Preset Knob Fix & QOL Improvements

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make knob 7 (fm_amount) work as a DX7 preset selector on 6-Op engines, fix pitch artifacts, hide dead knobs, and clean up debug output.

**Architecture:** The WIP changes in the working tree are ~80% correct. This plan cleans them up, fixes the remaining frequency_modulation_amount bug, hides non-functional knobs for 6-Op, removes debug fprintf, and produces a clean commit.

**Tech Stack:** C++14, Plaits/stmlib vendored DSP, plugin_api_v2

---

### Task 1: Remove debug fprintf from six_op_engine.cc

**Files:**
- Modify: `src/dsp/plaits/dsp/engine2/six_op_engine.cc:31-32` (remove `#include <cstdio>`)
- Modify: `src/dsp/plaits/dsp/engine2/six_op_engine.cc:69-70` (remove fprintf in LoadPatch)

**Step 1: Remove the `#include <cstdio>` added for debug**

In `six_op_engine.cc`, remove line 32 (`#include <cstdio>`). The file doesn't need stdio without the debug prints.

```cpp
// BEFORE (lines 31-32):
#include <algorithm>
#include <cstdio>

// AFTER:
#include <algorithm>
```

**Step 2: Remove fprintf in FMVoice::LoadPatch**

In `six_op_engine.cc`, remove the fprintf at lines 69-70:

```cpp
// BEFORE (lines 65-74):
void FMVoice::LoadPatch(const fm::Patch* patch) {
  if (patch == patch_) {
    return;
  }
  fprintf(stderr, "[plaits] LoadPatch ptr=%p (was %p)\n",
          (const void*)patch, (const void*)patch_);
  patch_ = patch;
  voice_.SetPatch(patch_);
  lfo_.Set(patch_->modulations);
}

// AFTER:
void FMVoice::LoadPatch(const fm::Patch* patch) {
  if (patch == patch_) {
    return;
  }
  patch_ = patch;
  voice_.SetPatch(patch_);
  lfo_.Set(patch_->modulations);
}
```

**Step 3: Verify the LoadPatch-outside-rising-edge fix is retained**

Confirm that at line ~138, `voice_[active_voice_].LoadPatch(&patches_[patch_index])` is OUTSIDE the `if (parameters.trigger & TRIGGER_RISING_EDGE)` block. This is the correct WIP change — keeps it.

**Step 4: Build**

Run: `cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh`
Expected: Build succeeds

**Step 5: Commit**

```bash
git add src/dsp/plaits/dsp/engine2/six_op_engine.cc
git commit -m "fix(6op): remove debug fprintf, keep LoadPatch outside rising-edge gate

LoadPatch moved outside TRIGGER_RISING_EDGE so preset changes take
effect immediately (not waiting for next note-on). FMVoice::LoadPatch
is a no-op when the pointer hasn't changed, so this is safe."
```

---

### Task 2: Clean up debug fprintf in plaits_plugin.cpp

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp:473-474` (remove fprintf in set_param fm_amount)
- Modify: `src/dsp/plaits_plugin.cpp:542-543` (remove fprintf in get_param fm_amount)
- Modify: `src/dsp/plaits_plugin.cpp:688-692` (remove fprintf in chain_params, simplify error check)
- Modify: `src/dsp/plaits_plugin.cpp:786-793` (remove render_block debug block)

**Step 1: Remove fprintf in set_param("fm_amount")**

```cpp
// REMOVE these 2 lines (~473-474):
            fprintf(stderr, "[plaits] set_param(fm_amount) val=\"%s\" -> preset=%d\n",
                    val, inst->fm_preset);
```

**Step 2: Remove fprintf in get_param("fm_amount")**

```cpp
// REMOVE these 2 lines (~542-543):
            fprintf(stderr, "[plaits] get_param(fm_amount) -> \"%s\" (preset=%d)\n",
                    name, inst->fm_preset);
```

**Step 3: Remove fprintf in chain_params generation**

```cpp
// REMOVE these 2 lines (~688-689):
        fprintf(stderr, "[plaits] chain_params eng=%d len=%d buf_len=%d is_6op=%d\n",
                inst->engine, len, buf_len, is_6op ? 1 : 0);

// Also REMOVE the truncation warning (~691-692) and simplify back to:
        if (len < 0 || len >= buf_len) return -1;
// (i.e., remove the fprintf inside the if block, but keep the if/return)
```

**Step 4: Remove the 6-Op debug logging block in render_block**

```cpp
// REMOVE the entire block (~786-793):
    // ── Debug: log 6-Op preset state (throttled to ~10Hz) ──────────────
    {
        static int dbg_counter = 0;
        if (inst->engine >= 2 && inst->engine <= 4 && (++dbg_counter % 344) == 0) {
            fprintf(stderr, "[plaits] 6op eng=%d preset=%d harmonics=%.4f\n",
                    inst->engine, inst->fm_preset, inst->patch.harmonics);
        }
    }
```

**Step 5: Build**

Run: `cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh`
Expected: Build succeeds

**Step 6: Commit**

```bash
git add src/dsp/plaits_plugin.cpp
git commit -m "chore: remove debug fprintf from plaits_plugin.cpp"
```

---

### Task 3: Zero frequency_modulation_amount for 6-Op engines

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp` render_block, after the harmonics override block (~line 742)

**Context:** In `Voice::Render`, `patch.frequency_modulation_amount` feeds into `ApplyModulations` on the note pitch — it adds pitch modulation proportional to the internal decay envelope. For 6-Op engines, `fm_amount` is repurposed as a preset selector, but the stale float value (from a previous engine or default) still gets written to `patch.frequency_modulation_amount`, causing unwanted pitch wobble.

**Step 1: Add frequency_modulation_amount zeroing**

After the existing harmonics override block for 6-Op (~line 740-742), add:

```cpp
    // For 6-Op FM engines (2-4), override harmonics LAST to select fm_preset.
    // This must happen after all knob values are applied to patch.
    // SixOpEngine::Render quantizes (harmonics * 1.02) into 0-31 via
    // HysteresisQuantizer2(32). Map preset index to center of each bin.
    if (inst->engine >= 2 && inst->engine <= 4) {
        inst->patch.harmonics = ((float)inst->fm_preset + 0.5f) / 32.0f;
        // fm_amount is repurposed as preset selector for 6-Op; zero out
        // frequency_modulation_amount to prevent stale values from causing
        // pitch modulation via Voice::Render's ApplyModulations on note.
        inst->patch.frequency_modulation_amount = 0.0f;
    }
```

**Step 2: Build**

Run: `cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/dsp/plaits_plugin.cpp
git commit -m "fix(6op): zero frequency_modulation_amount to prevent pitch artifacts

Stale fm_amount float values were feeding into Voice::Render's
ApplyModulations on note pitch, causing unwanted pitch wobble
on 6-Op engines where fm_amount is repurposed as preset selector."
```

---

### Task 4: Hide non-functional knobs for 6-Op in chain_params

**Files:**
- Modify: `src/dsp/plaits_plugin.cpp` chain_params generation (~line 653-695)

**Context:** 6-Op engines have `already_enveloped=true`, so the Plaits LPG is bypassed entirely. The Decay and LPG Color knobs do nothing audible. Showing them is confusing. Label them "---" for 6-Op engines so the user knows they're inactive.

**Step 1: Make decay and lpg_colour labels dynamic for 6-Op**

In the chain_params snprintf, replace the static decay/lpg_colour entries with dynamic ones that show "---" for 6-Op:

```cpp
        // Decay and LPG Color: functional for most engines, but 6-Op engines
        // have already_enveloped=true (LPG bypassed). DX7 patches carry their
        // own operator envelopes, so these knobs are non-functional.
        const char* decay_name = is_6op ? "---" : "Decay";
        const char* lpg_name   = is_6op ? "---" : "LPG Color";
```

Then in the snprintf format string, change:
```
"\"name\":\"Decay\""    → "\"name\":\"%s\""
"\"name\":\"LPG Color\"" → "\"name\":\"%s\""
```

And add `decay_name, lpg_name` to the snprintf args (after `m`, before `fm_amount_entry`).

**Step 2: Build**

Run: `cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/dsp/plaits_plugin.cpp
git commit -m "feat(6op): label Decay and LPG Color as --- for 6-Op engines

6-Op engines have already_enveloped=true (LPG bypassed). DX7 patches
have their own operator envelopes, so these Plaits knobs are
non-functional. Labels now show --- to indicate inactivity."
```

---

### Task 5: Final build, deploy, and tag

**Step 1: Full build**

Run: `cd ~/move-everything-parent/move-anything-plaits && ./scripts/build.sh`
Expected: Build succeeds, produces `dist/plaits-module.tar.gz`

**Step 2: Deploy to Move**

Run: `cd ~/move-everything-parent/move-anything-plaits && ./scripts/install.sh`
Expected: Deploys successfully to move.local

**Step 3: Verify on hardware**

Manual verification checklist:
- [ ] Switch to 6-Op I (engine 2) — knob 7 scrolls through Bank 0 preset names
- [ ] Turn knob 7 — sound changes between DX7 patches
- [ ] Switch to 6-Op II (engine 3) — knob 7 shows Bank 1 presets
- [ ] Switch to 6-Op III (engine 4) — knob 7 shows Bank 2 presets
- [ ] Decay and LPG Color knobs show "---" label on 6-Op engines
- [ ] Decay and LPG Color show normal labels on non-6-Op engines (e.g. VA VCF)
- [ ] FM knob works normally on non-6-Op engines (float 0-1)
- [ ] No pitch wobble on 6-Op engines
- [ ] Preset changes take effect immediately (not waiting for next note)
