#ifndef NES_HOST_H
#define NES_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include "libretro.h"

#define NES_MAX_W 512
#define NES_MAX_H 480
#define NES_JOY_BITS 16
#define NES_PATH_MAX 4096
/* FCEUmm preserves content-path metadata in a fixed 2048-byte buffer. */
#define NES_CORE_CONTENT_PATH_BYTES 2048u
#define NES_MAX_ROM_BYTES (16u * 1024u * 1024u)
#define NES_STATE_SLOT_COUNT 10u
#define NES_STATE_LABEL_BYTES 96u
#define NES_MAX_STATE_BYTES (4u * 1024u * 1024u)
/* Stereo frames in the ring (about one second at 48 kHz). */
#define NES_AUDIO_RING_FRAMES 48000
/* Discard older queued PCM after a stalled consumer resumes. */
#define NES_AUDIO_RECENT_MS 100u

struct nes_host_private;

/*
 * A consistent copy of externally visible host state.  Callers should use
 * host_get_status() instead of reading the strings and timing fields in
 * struct nes_host while the emulation thread is running.
 */
struct nes_host_status {
	bool core_loaded;
	bool game_loaded;
	bool running;
	bool paused;
	bool demo_mode;
	int viewers;

	char core_path[NES_PATH_MAX];
	char rom_path[NES_PATH_MAX];
	char library_name[128];
	char library_version[64];

	unsigned width;
	unsigned height;
	uint64_t frame_id;
	double fps;
	double sample_rate;
};

struct nes_state_info {
	unsigned slot;
	bool exists;
	bool loadable;
	bool durable;
	uint64_t modified;
	uint64_t size;
	int error;
	char label[NES_STATE_LABEL_BYTES];
};

struct nes_host {
	/*
	 * Core handles and entry points are owned by the emulation thread.
	 * fceumm_bind.c fills these fields while executing on that thread.
	 */
	void *dl;
	struct retro_core core;

	/* Atomics retain source compatibility for simple read-only status users. */
	atomic_bool core_loaded;
	atomic_bool game_loaded;
	atomic_bool running;
	atomic_bool paused;
	atomic_bool demo_mode;
	atomic_bool thread_alive;
	atomic_bool initialized;

	char core_path[NES_PATH_MAX];
	char rom_path[NES_PATH_MAX];
	char system_dir[NES_PATH_MAX];
	char save_dir[NES_PATH_MAX];
	char library_name[128];
	char library_version[64];

	atomic_int pixel_fmt;
	unsigned width;
	unsigned height;
	size_t pitch;
	double fps;
	double sample_rate;

	uint16_t frame_rgb565[NES_MAX_W * NES_MAX_H];
	unsigned frame_w;
	unsigned frame_h;
	uint64_t frame_id;
	pthread_mutex_t frame_mu;

	/* Interleaved stereo int16 ring. */
	int16_t audio_ring[NES_AUDIO_RING_FRAMES * 2];
	size_t audio_w;
	size_t audio_r;
	size_t audio_count;
	pthread_mutex_t audio_mu;
	uint64_t audio_seq;

	/* Joypad bitmask, RETRO_DEVICE_ID_JOYPAD_* as bit index. */
	atomic_uint joy[2];

	pthread_t emu_thread;
	atomic_int viewers;

	/* Demo tone phase; emulation-thread owned. */
	uint32_t demo_phase;

	/* Synchronisation, command queue and SRAM bookkeeping. */
	struct nes_host_private *priv;
};

int host_init(struct nes_host *h, const char *core_path,
	const char *system_dir, const char *save_dir);
int host_shutdown(struct nes_host *h);

/*
 * These calls are synchronous.  Every libretro lifecycle operation is
 * executed by the emulation thread, never by the HTTP/caller thread.
 */
int host_load_core(struct nes_host *h, const char *core_path);
int host_unload_core(struct nes_host *h);

/* Bind statically linked FCEUmm (called by the emulation thread). */
int host_bind_builtin_fceumm(struct nes_host *h);

int host_load_game(struct nes_host *h, const char *rom_path);
int host_unload_game(struct nes_host *h);

int host_start(struct nes_host *h);
int host_stop(struct nes_host *h);
int host_reset(struct nes_host *h);
int host_set_paused(struct nes_host *h, bool paused);

/* Force an atomic SRAM flush. Returns 0 if saved or no SRAM is exposed. */
int host_flush_sram(struct nes_host *h);

/*
 * Ten persistent, per-ROM full-machine save-state slots. State payloads are
 * captured and restored by the emulation thread at frame boundaries.
 */
int host_list_states(struct nes_host *h, struct nes_state_info *states,
	size_t count);
int host_save_state(struct nes_host *h, unsigned slot, const char *label,
	struct nes_state_info *state);
int host_load_state(struct nes_host *h, unsigned slot);
int host_delete_state(struct nes_host *h, unsigned slot, bool *durable);

void host_set_button(struct nes_host *h, unsigned port, unsigned id, bool pressed);
void host_set_joy_mask(struct nes_host *h, unsigned port, uint16_t mask);

/* Active Play clients. Zero puts emulation into a deep, interruptible idle. */
void host_set_viewers(struct nes_host *h, int n);

bool host_is_running(const struct nes_host *h);
bool host_is_paused(const struct nes_host *h);
void host_get_status(struct nes_host *h, struct nes_host_status *status);

/* Lower CPU priority and make nesd disposable under Linux OOM pressure. */
void host_apply_router_limits(void);

/* Copy the latest tightly packed, native-endian RGB565 frame. */
bool host_copy_frame(struct nes_host *h, uint16_t *dst, size_t dst_pixels,
	unsigned *w, unsigned *height, uint64_t *frame_id);

/*
 * Drain up to max_frames stereo frames into dst (interleaved L,R). If the
 * consumer stalled, obsolete PCM older than NES_AUDIO_RECENT_MS is discarded
 * first. Returns the number of stereo frames copied.
 */
size_t host_copy_audio(struct nes_host *h, int16_t *dst, size_t max_frames,
	unsigned *sample_rate, unsigned *channels);

#endif
