// Plaits macro oscillator plugin for Move Anything
// Based on Mutable Instruments Plaits by Emilie Gillet
// Port by charlesvestal

extern "C" {
#include "host/plugin_api_v2.h"
}

#include "plaits/dsp/voice.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <new>
#include <algorithm>

using std::min;
using std::max;

// ──────────────────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────────────────

static const int   BLOCK_SIZE   = 128;
static const int   BUFFER_SIZE  = 32768;  // 32KB for voice engine buffers

static const int   LEGATO_OFF = 0;
static const int   LEGATO_ON  = 1;

static constexpr float kOutputVolume = 0.85f;  // internal output scalar, not user-exposed

// ──────────────────────────────────────────────────────────────────────────
// Per-engine default parameter values.
// Applied ONCE when an engine is first visited in a session.
// Subsequent visits restore last-used values — not re-applied.
// Only covers: harmonics, timbre, morph, decay, lpg_colour.
// fm_amount, timbre_mod, morph_mod, and aux_mix are NOT reset on engine switch.
// ──────────────────────────────────────────────────────────────────────────

namespace {
struct EngineDefaults {
    float harmonics, timbre, morph, decay, lpg_colour;
};
} // namespace

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

// ──────────────────────────────────────────────────────────────────────────
// Instance struct
// ──────────────────────────────────────────────────────────────────────────

struct plaits_instance_t {
    // Plaits voice engine
    plaits::Voice voice;
    plaits::Patch patch;
    plaits::Modulations mods;
    stmlib::BufferAllocator allocator;
    char shared_buffer[BUFFER_SIZE];

    // Note state
    int  current_note;    // MIDI note 0-127
    bool note_active;     // Is a note currently held?
    bool trigger_pending; // True for one render cycle after Note On
    float velocity;       // Last note velocity, 0.0-1.0
    int  legato_mode;     // LEGATO_OFF or LEGATO_ON

    // Parameters
    int   engine;
    float harmonics;
    float timbre;
    float morph;
    float decay;
    float lpg_colour;
    float fm_amount;
    float timbre_mod;
    float morph_mod;
    float aux_mix;
    float velocity_sensitivity;
    int   octave_transpose;

    // Render frame buffer (plaits::Voice::Frame has interleaved short out/aux)
    plaits::Voice::Frame frame_buf[BLOCK_SIZE];

    // Engine switch tracking
    int  previous_engine;  // detects change in set_param to trigger voice reset
    bool engine_visited[24]; // true after first visit; prevents re-applying defaults
};

// ──────────────────────────────────────────────────────────────────────────
// create_instance
// ──────────────────────────────────────────────────────────────────────────

static void* create_instance(const char* module_dir, const char* json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    // Must use new (not calloc) — Engine subclasses have virtual functions;
    // calloc zeros vtable pointers causing a crash on the first virtual call.
    plaits_instance_t* inst = new (std::nothrow) plaits_instance_t();
    if (!inst) return nullptr;

    // Initialize Plaits voice
    inst->allocator.Init(inst->shared_buffer, BUFFER_SIZE);
    inst->voice.Init(&inst->allocator);

    // Default parameter values
    inst->engine             = 0;
    inst->previous_engine    = -1;  // sentinel: forces reset on first set_param("engine", ...)
    // Mark engine 0 as visited — defaults applied below from kEngineDefaults[0]
    memset(inst->engine_visited, 0, sizeof(inst->engine_visited));
    inst->engine_visited[0] = true;
    inst->harmonics          = kEngineDefaults[0].harmonics;
    inst->timbre             = kEngineDefaults[0].timbre;
    inst->morph              = kEngineDefaults[0].morph;
    inst->decay              = kEngineDefaults[0].decay;
    inst->lpg_colour         = kEngineDefaults[0].lpg_colour;
    inst->fm_amount          = 0.0f;
    inst->timbre_mod         = 0.0f;
    inst->morph_mod          = 0.0f;
    inst->aux_mix            = 0.0f;
    inst->velocity_sensitivity = 0.5f;
    inst->octave_transpose   = 0;
    inst->legato_mode        = LEGATO_OFF;

    // Note state
    inst->current_note    = 60;
    inst->note_active     = false;
    inst->trigger_pending = false;
    inst->velocity        = 1.0f;

    return inst;
}

// ──────────────────────────────────────────────────────────────────────────
// destroy_instance
// ──────────────────────────────────────────────────────────────────────────

static void destroy_instance(void* instance) {
    delete (plaits_instance_t*)instance;
}

// ──────────────────────────────────────────────────────────────────────────
// on_midi
// ──────────────────────────────────────────────────────────────────────────

static void on_midi(void* instance, const uint8_t* msg, int len, int source) {
    (void)source;
    if (len < 3) return;

    plaits_instance_t* inst = (plaits_instance_t*)instance;
    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = msg[1];
    uint8_t vel    = msg[2];

    if (status == 0x90 && vel > 0) {
        // Note On
        bool retrigger = true;
        if (inst->legato_mode == LEGATO_ON && inst->note_active) {
            // Legato: don't retrigger LPG when a note is already held
            retrigger = false;
        }
        inst->current_note    = (int)note;
        inst->velocity        = vel / 127.0f;
        inst->note_active     = true;
        inst->trigger_pending = retrigger;

    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        // Note Off (0x80) or Note On with velocity 0 (treated as note off)
        if (note == (uint8_t)inst->current_note) {
            inst->note_active     = false;
            inst->trigger_pending = false;
        }

    } else if (status == 0xB0) {
        // Control Change
        uint8_t cc  = msg[1];
        if (cc == 123 || cc == 120) {
            // All Notes Off (123) or All Sound Off (120)
            inst->note_active     = false;
            inst->trigger_pending = false;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────
// Engine names (registration order from plaits::Voice::Init)
// ──────────────────────────────────────────────────────────────────────────

static const char* kEngineNames[] = {
    "VA VCF",     // 0  Virtual Analog VCF  [engine2]
    "Phase Dist", // 1  Phase Distortion    [engine2]
    "6-Op I",     // 2  Six Op FM (1)       [engine2]
    "6-Op II",    // 3  Six Op FM (2)       [engine2]
    "6-Op III",   // 4  Six Op FM (3)       [engine2]
    "Wave Terr",  // 5  Wave Terrain        [engine2]
    "Str Mach",   // 6  String Machine      [engine2]
    "Chiptune",   // 7  Chiptune            [engine2]
    "V. Analog",  // 8  Virtual Analog      [engine]
    "Waveshape",  // 9  Waveshaping         [engine]
    "FM",         // 10 FM                  [engine]
    "Grain",      // 11 Granular            [engine]
    "Additive",   // 12 Additive            [engine]
    "Wavetable",  // 13 Wavetable           [engine]
    "Chord",      // 14 Chord               [engine]
    "Speech",     // 15 Speech              [engine]
    "Swarm",      // 16 Swarm               [engine]
    "Noise",      // 17 Noise               [engine]
    "Particle",   // 18 Particle            [engine]
    "String",     // 19 Karplus-Strong      [engine]
    "Modal",      // 20 Modal resonator     [engine]
    "Bass Drum",  // 21                     [engine]
    "Snare Drum", // 22                     [engine]
    "Hi-Hat",     // 23                     [engine]
};
static const int kNumEngines = 24;

// Per-engine labels for Harmonics (0), Timbre (1), Morph (2).
// Strings are substituted directly into JSON in get_param("chain_params") —
// must not contain JSON special characters (", \, control chars).
static const char* kEngineLabels[24][3] = {
    {"Detune",     "Cutoff",      "Resonance" },  //  0 VA VCF
    {"Ratio",      "Phase",       "Distortion"},  //  1 Phase Dist
    {"Ratio",      "Feedback",    "Level"     },  //  2 6-Op I
    {"Ratio",      "Feedback",    "Level"     },  //  3 6-Op II
    {"Ratio",      "Feedback",    "Level"     },  //  4 6-Op III
    {"Terrain",    "X",           "Y"         },  //  5 Wave Terrain
    {"Detune",     "Tone",        "Envelope"  },  //  6 Str Machine
    {"Arpeggio",   "Duty",        "Filter"    },  //  7 Chiptune
    {"Detune",     "Cutoff",      "Shape"     },  //  8 V. Analog
    {"Overtone",   "Waveshape",   "Fold"      },  //  9 Waveshape
    {"Ratio",      "Feedback",    "Topology"  },  // 10 FM
    {"Position",   "Size",        "Density"   },  // 11 Grain
    {"Bumps",      "Slope",       "Shift"     },  // 12 Additive
    {"Bank",       "Pointer",     "Mirror"    },  // 13 Wavetable
    {"Chord",      "Timbre",      "Waveform"  },  // 14 Chord
    {"Prosody",    "Phoneme",     "Formant"   },  // 15 Speech
    {"Detune",     "Spread",      "Chaos"     },  // 16 Swarm
    {"Frequency",  "Slope",       "Ruggedness"},  // 17 Noise
    {"Spread",     "Extinction",  "Scatter"   },  // 18 Particle
    {"Harmonic",   "Timbre",      "Morph"     },  // 19 String
    {"Harmonic",   "Timbre",      "Decay"     },  // 20 Modal
    {"Frequency",  "Punch",       "Decay"     },  // 21 Bass Drum
    {"Frequency",  "Tone",        "Decay"     },  // 22 Snare Drum
    {"Frequency",  "Tone",        "Decay"     },  // 23 Hi-Hat
};

// Per-engine gain compensation table.
// Indexed by engine registration order from plaits::Voice::Init() in voice.cc.
//
// Applied AFTER Plaits' own ChannelPostProcessor, which already bakes each
// engine's RegisterInstance gain into the Frame shorts. Do NOT attempt to
// derive these values from RegisterInstance parameters — that gain is already
// included in the short values written to frame_buf. These values are
// empirically tuned against what the hardware I2S bus would produce, to
// equalise perceived loudness across engine families.
static constexpr float kGainTable[24] = {
    1.0f,  //  0 VA VCF       (VirtualAnalogVCFEngine)
    1.0f,  //  1 Phase Dist   (PhaseDistortionEngine)
    2.8f,  //  2 6-Op I       (SixOpEngine)
    2.8f,  //  3 6-Op II      (SixOpEngine)
    2.8f,  //  4 6-Op III     (SixOpEngine)
    1.8f,  //  5 Wave Terr    (WaveTerrainEngine)
    1.5f,  //  6 Str Mach     (StringMachineEngine)
    1.2f,  //  7 Chiptune     (ChiptuneEngine)
    1.0f,  //  8 V. Analog    (VirtualAnalogEngine)
    1.0f,  //  9 Waveshape    (WaveshapingEngine)
    2.5f,  // 10 FM           (FMEngine, two-op)
    1.1f,  // 11 Grain        (GrainEngine)
    1.0f,  // 12 Additive     (AdditiveEngine)
    1.2f,  // 13 Wavetable    (WavetableEngine)
    0.8f,  // 14 Chord        (ChordEngine)
    1.3f,  // 15 Speech       (SpeechEngine)
    0.9f,   // 16  Swarm      (SwarmEngine,       limiter path)
    1.2f,   // 17  Noise      (NoiseEngine,        limiter path)
    1.2f,   // 18  Particle   (ParticleEngine,     limiter path)
    1.0f,   // 19  String     (StringEngine,       limiter path)
    1.0f,   // 20  Modal      (ModalEngine,        limiter path)
    1.0f,  // 21 Bass Drum    (BassDrumEngine)
    1.0f,  // 22 Snare Drum   (SnareDrumEngine)
    1.0f,  // 23 Hi-Hat       (HiHatEngine)
};


// ──────────────────────────────────────────────────────────────────────────
// reset_voice  —  reinitialize Plaits voice to clear all internal state.
// Called on engine switch. Does NOT change any plugin parameters.
// Voice::Init re-registers all engines and resets post-processors, envelopes,
// and internal state. Allocator must be re-initialized first (reuses same buffer).
//
// COST: Voice::Init calls engine->Init() on all 24 engines (virtual dispatch +
// allocator re-walk). This is intentional — the only way to clear
// aux_post_processor_, lpg_envelope_, and decay_envelope_ without modifying
// vendor code. Engine switches are infrequent (user-initiated) so the cost
// is acceptable. Do not call from a hot path.
//
// Also resets previous_engine_index_ to -1 inside voice.cc, ensuring the
// next voice.Render() call sees a fresh engine selection and calls e->Reset()
// and out_post_processor_.Reset() on the new engine.
//
// THREADING: Assumes set_param is not called concurrently with render_block.
// The plugin_api_v2 contract does not document a threading model. If concurrent
// delivery is ever required, move the reset_voice call into render_block behind
// a reset_pending flag owned by the audio thread.
// ──────────────────────────────────────────────────────────────────────────

static void reset_voice(plaits_instance_t* inst) {
    inst->allocator.Init(inst->shared_buffer, BUFFER_SIZE);
    inst->voice.Init(&inst->allocator);
}

// ──────────────────────────────────────────────────────────────────────────
// set_param
// ──────────────────────────────────────────────────────────────────────────

static void set_param(void* instance, const char* key, const char* val) {
    plaits_instance_t* inst = (plaits_instance_t*)instance;

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
    } else if (strcmp(key, "harmonics") == 0) {
        float v = (float)atof(val);
        inst->harmonics = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "timbre") == 0) {
        float v = (float)atof(val);
        inst->timbre = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "morph") == 0) {
        float v = (float)atof(val);
        inst->morph = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "decay") == 0) {
        float v = (float)atof(val);
        inst->decay = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "lpg_colour") == 0) {
        float v = (float)atof(val);
        inst->lpg_colour = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "fm_amount") == 0) {
        float v = (float)atof(val);
        inst->fm_amount = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "timbre_mod") == 0) {
        float v = (float)atof(val);
        inst->timbre_mod = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "morph_mod") == 0) {
        float v = (float)atof(val);
        inst->morph_mod = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "aux_mix") == 0) {
        float v = (float)atof(val);
        inst->aux_mix = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "legato") == 0) {
        inst->legato_mode = (strcmp(val, "on") == 0) ? LEGATO_ON : LEGATO_OFF;
    } else if (strcmp(key, "velocity_sensitivity") == 0) {
        float v = (float)atof(val);
        inst->velocity_sensitivity = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    } else if (strcmp(key, "octave_transpose") == 0) {
        int v = atoi(val);
        inst->octave_transpose = v < -3 ? -3 : v > 3 ? 3 : v;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// get_param
// ──────────────────────────────────────────────────────────────────────────

static int get_single_param(const plaits_instance_t* inst, const char* key,
                             char* buf, int buf_len) {
    if (strcmp(key, "engine") == 0)
        return snprintf(buf, buf_len, "%s", kEngineNames[inst->engine]);
    if (strcmp(key, "harmonics") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->harmonics);
    if (strcmp(key, "timbre") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->timbre);
    if (strcmp(key, "morph") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->morph);
    if (strcmp(key, "decay") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->decay);
    if (strcmp(key, "lpg_colour") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->lpg_colour);
    if (strcmp(key, "fm_amount") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->fm_amount);
    if (strcmp(key, "timbre_mod") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->timbre_mod);
    if (strcmp(key, "morph_mod") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->morph_mod);
    if (strcmp(key, "aux_mix") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->aux_mix);
    if (strcmp(key, "legato") == 0)
        return snprintf(buf, buf_len, "%s",
                        inst->legato_mode == LEGATO_ON ? "on" : "off");
    if (strcmp(key, "velocity_sensitivity") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->velocity_sensitivity);
    if (strcmp(key, "octave_transpose") == 0)
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    return -1;
}

static int get_param(void* instance, const char* key, char* buf, int buf_len) {
    const plaits_instance_t* inst = (const plaits_instance_t*)instance;

    // Individual param read
    int r = get_single_param(inst, key, buf, buf_len);
    if (r >= 0) return r;

    // ui_hierarchy — Shadow UI navigation structure
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char* json =
            "{"
              "\"label\":\"Plaits\","
              "\"levels\":{"
                "\"root\":{"
                  "\"label\":\"Plaits\","
                  "\"knobs\":[\"engine\",\"harmonics\",\"timbre\",\"morph\","
                              "\"decay\",\"lpg_colour\",\"fm_amount\",\"aux_mix\"],"
                  "\"params\":["
                    "{\"key\":\"engine\",\"label\":\"Engine\",\"type\":\"enum\"},"
                    "{\"key\":\"harmonics\",\"label\":\"Harmonics\"},"
                    "{\"key\":\"timbre\",\"label\":\"Timbre\"},"
                    "{\"key\":\"morph\",\"label\":\"Morph\"},"
                    "{\"key\":\"decay\",\"label\":\"Decay\"},"
                    "{\"key\":\"lpg_colour\",\"label\":\"LPG Color\"},"
                    "{\"key\":\"fm_amount\",\"label\":\"FM\"},"
                    "{\"key\":\"timbre_mod\",\"label\":\"Timbre Mod\"},"
                    "{\"key\":\"morph_mod\",\"label\":\"Morph Mod\"},"
                    "{\"key\":\"aux_mix\",\"label\":\"Mix\"},"
                    "{\"key\":\"legato\",\"label\":\"Legato\"},"
                    "{\"key\":\"velocity_sensitivity\",\"label\":\"Vel Sens\"},"
                    "{\"key\":\"octave_transpose\",\"label\":\"Octave\"}"
                  "]"
                "}"
              "}"
            "}";
        int len = (int)strlen(json);
        if (len >= buf_len) return -1;
        memcpy(buf, json, len + 1);
        return len;
    }

    // chain_params — parameter metadata for Shadow UI knob editing
    // Returns engine-specific names for harmonics, timbre, morph.
    if (strcmp(key, "chain_params") == 0) {
        const char* h = kEngineLabels[inst->engine][0];
        const char* t = kEngineLabels[inst->engine][1];
        const char* m = kEngineLabels[inst->engine][2];
        int len = snprintf(buf, buf_len,
            "["
              "{\"key\":\"engine\",\"name\":\"Engine\",\"type\":\"enum\","
               "\"options\":[\"VA VCF\",\"Phase Dist\",\"6-Op I\",\"6-Op II\",\"6-Op III\","
               "\"Wave Terr\",\"Str Mach\",\"Chiptune\",\"V. Analog\",\"Waveshape\","
               "\"FM\",\"Grain\",\"Additive\",\"Wavetable\",\"Chord\",\"Speech\","
               "\"Swarm\",\"Noise\",\"Particle\",\"String\",\"Modal\","
               "\"Bass Drum\",\"Snare Drum\",\"Hi-Hat\"],\"default\":\"VA VCF\","
               "\"refreshes_labels\":true},"
              "{\"key\":\"harmonics\",\"name\":\"%s\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
              "{\"key\":\"timbre\",\"name\":\"%s\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
              "{\"key\":\"morph\",\"name\":\"%s\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
              "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
              "{\"key\":\"lpg_colour\",\"name\":\"LPG Color\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
              "{\"key\":\"fm_amount\",\"name\":\"FM\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.0},"
              "{\"key\":\"timbre_mod\",\"name\":\"Timbre Mod\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.0},"
              "{\"key\":\"morph_mod\",\"name\":\"Morph Mod\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.0},"
              "{\"key\":\"aux_mix\",\"name\":\"Mix\",\"type\":\"float\","
               "\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.0},"
              "{\"key\":\"legato\",\"name\":\"Legato\",\"type\":\"enum\","
               "\"options\":[\"off\",\"on\"],\"default\":\"off\"},"
              "{\"key\":\"velocity_sensitivity\",\"name\":\"Vel Sens\","
               "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.5},"
              "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\","
               "\"min\":-3,\"max\":3,\"default\":0}"
            "]",
            h, t, m);
        if (len < 0 || len >= buf_len) return -1;
        return len;
    }

    return -1;
}

// ──────────────────────────────────────────────────────────────────────────
// get_error  (stub)
// ──────────────────────────────────────────────────────────────────────────

static int get_error(void* instance, char* buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

// ──────────────────────────────────────────────────────────────────────────
// render_block
// ──────────────────────────────────────────────────────────────────────────

// Pitch compensation: Plaits was designed for 48kHz hardware.
// At 44100Hz the pitch is slightly flat (~1.47 semitones).
// Compensate by adding semitones to the note value.
// Plaits uses kCorrectedSampleRate = 47872.34f (actual STM32 I2S clock).
// At 44100Hz: log2(47872.34 / 44100) * 12 ≈ 1.421 semitones compensation.
static const float kPitchOffset = 12.0f * log2f(47872.34f / 44100.0f);

static void render_block(void* instance, int16_t* out_lr, int frames) {
    plaits_instance_t* inst = (plaits_instance_t*)instance;

    // ── Build Patch ──────────────────────────────────────────────────────
    inst->patch.engine   = inst->engine;
    inst->patch.note     = (float)(inst->current_note + inst->octave_transpose * 12)
                           + kPitchOffset;
    inst->patch.harmonics = inst->harmonics;
    inst->patch.timbre    = inst->timbre;
    inst->patch.morph     = inst->morph;
    inst->patch.decay     = inst->decay;
    inst->patch.lpg_colour = inst->lpg_colour;
    inst->patch.frequency_modulation_amount = inst->fm_amount;
    inst->patch.timbre_modulation_amount    = inst->timbre_mod;
    inst->patch.morph_modulation_amount     = inst->morph_mod;

    // ── Build Modulations ────────────────────────────────────────────────
    memset(&inst->mods, 0, sizeof(inst->mods));
    inst->mods.trigger_patched = true;
    inst->mods.level_patched   = true;

    // Trigger: pulse for one render block after Note On
    inst->mods.trigger    = inst->trigger_pending ? 1.0f : 0.0f;
    inst->trigger_pending = false;

    // Level: controls LPG amplitude (gate + velocity)
    if (inst->note_active) {
        // Scale level by velocity sensitivity:
        // At vel_sens=0: always full level regardless of velocity
        // At vel_sens=1: level is exactly the velocity value
        float vs = inst->velocity_sensitivity;
        inst->mods.level = inst->velocity * vs + (1.0f - vs);
    } else {
        inst->mods.level = 0.0f;  // Note off: LPG starts release
    }

    // ── Render ───────────────────────────────────────────────────────────
    {
        // Plaits internal buffers are kMaxBlockSize=24 frames; must render in chunks
        static const int kChunkSize = 24;
        int remaining = frames;
        int offset = 0;
        while (remaining > 0) {
            int chunk = remaining < kChunkSize ? remaining : kChunkSize;
            inst->voice.Render(inst->patch, inst->mods,
                               inst->frame_buf + offset, (size_t)chunk);
            // Only trigger on the first chunk
            inst->mods.trigger = 0.0f;
            remaining -= chunk;
            offset    += chunk;
        }
    }

    // ── Convert Frame output to int16 stereo with output routing ────────
    const float gain = kOutputVolume;
    const float eg = kGainTable[inst->engine];
    for (int i = 0; i < frames; i++) {
        // Frame values are already int16 range (short)
        float out_f = -inst->frame_buf[i].out / 32767.0f * eg;
        float aux_f = -inst->frame_buf[i].aux / 32767.0f * eg;

        // Move is mono hardware: blend OUT and AUX, write same sample to both channels.
        const float blended = out_f * (1.0f - inst->aux_mix) + aux_f * inst->aux_mix;
        const float sample = blended * gain;

        // Clamp and write
        const int16_t s = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, sample * 32767.0f));  // hard clip intentional
        out_lr[i * 2]     = s;
        out_lr[i * 2 + 1] = s;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// Plugin API export
// ──────────────────────────────────────────────────────────────────────────

static plugin_api_v2_t api = {
    /* api_version      = */ 2,
    /* create_instance  = */ create_instance,
    /* destroy_instance = */ destroy_instance,
    /* on_midi          = */ on_midi,
    /* set_param        = */ set_param,
    /* get_param        = */ get_param,
    /* get_error        = */ get_error,
    /* render_block     = */ render_block,
};

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    (void)host;
    return &api;
}
