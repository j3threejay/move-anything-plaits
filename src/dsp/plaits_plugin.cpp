// Plaits macro oscillator plugin for Move Anything
// Based on Mutable Instruments Plaits by Emilie Gillet
// Port by charlesvestal

extern "C" {
#include "host/plugin_api_v2.h"
}

#include "plaits/dsp/voice.h"
#include "param_helper.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>

using std::min;
using std::max;

// ──────────────────────────────────────────────────────────────────────────
// Constants
// ──────────────────────────────────────────────────────────────────────────

static const int   BLOCK_SIZE   = 128;
static const int   BUFFER_SIZE  = 32768;  // 32KB for voice engine buffers

static const int   OUTPUT_MONO   = 0;
static const int   OUTPUT_STEREO = 1;
static const int   OUTPUT_AUX    = 2;

static const int   LEGATO_OFF = 0;
static const int   LEGATO_ON  = 1;

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
    int   output_mode;
    float velocity_sensitivity;
    int   octave_transpose;
    float volume;

    // Render frame buffer (plaits::Voice::Frame has interleaved short out/aux)
    plaits::Voice::Frame frame_buf[BLOCK_SIZE];
};

// ──────────────────────────────────────────────────────────────────────────
// create_instance
// ──────────────────────────────────────────────────────────────────────────

static void* create_instance(const char* module_dir, const char* json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    plaits_instance_t* inst =
        (plaits_instance_t*)calloc(1, sizeof(plaits_instance_t));
    if (!inst) return nullptr;

    // Initialize Plaits voice
    inst->allocator.Init(inst->shared_buffer, BUFFER_SIZE);
    inst->voice.Init(&inst->allocator);

    // Default parameter values
    inst->engine             = 0;
    inst->harmonics          = 0.5f;
    inst->timbre             = 0.5f;
    inst->morph              = 0.5f;
    inst->decay              = 0.5f;
    inst->lpg_colour         = 0.5f;
    inst->fm_amount          = 0.0f;
    inst->timbre_mod         = 0.0f;
    inst->morph_mod          = 0.0f;
    inst->output_mode        = OUTPUT_MONO;
    inst->velocity_sensitivity = 0.5f;
    inst->octave_transpose   = 0;
    inst->volume             = 0.7f;
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
    free(instance);
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
// set_param  (stub)
// ──────────────────────────────────────────────────────────────────────────

static void set_param(void* instance, const char* key, const char* val) {
    (void)instance; (void)key; (void)val;
}

// ──────────────────────────────────────────────────────────────────────────
// get_param  (stub)
// ──────────────────────────────────────────────────────────────────────────

static int get_param(void* instance, const char* key, char* buf, int buf_len) {
    (void)instance; (void)key; (void)buf; (void)buf_len;
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
static const float kPitchOffset = 12.0f * 0.12225f; // log2(48000/44100)*12 ≈ 1.47

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
    inst->voice.Render(inst->patch, inst->mods, inst->frame_buf, (size_t)frames);

    // ── Convert Frame output to int16 stereo with output routing ────────
    const float gain = inst->volume;
    for (int i = 0; i < frames; i++) {
        // Frame values are already int16 range (short)
        float out_f = inst->frame_buf[i].out / 32767.0f;
        float aux_f = inst->frame_buf[i].aux / 32767.0f;

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

        l *= gain;
        r *= gain;

        // Clamp and write
        out_lr[i * 2]     = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, l * 32767.0f));
        out_lr[i * 2 + 1] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, r * 32767.0f));
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
