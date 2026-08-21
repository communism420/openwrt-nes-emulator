#define _GNU_SOURCE
#include "host.h"
#include "sha256.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef RETRO_ENVIRONMENT_SET_CONTROLLER_INFO
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO 35
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define SRAM_FLUSH_INTERVAL_MS 30000u
#define DEMO_FPS 20u
#define NES_EMU_THREAD_STACK_BYTES (2u * 1024u * 1024u)
#define NES_SRAM_THREAD_STACK_BYTES (256u * 1024u)
#define NES_ROUTER_NICE 5
#define NES_STATE_FORMAT_VERSION 2u
#define NES_STATE_HEADER_BYTES 320u
#define NES_STATE_MAX_FRAME_BYTES \
	((size_t)NES_MAX_W * (size_t)NES_MAX_H * sizeof(uint16_t))
#define NES_STATE_TOTAL_QUOTA (16ull * 1024ull * 1024ull)
#define NES_STATE_FREE_RESERVE (8ull * 1024ull * 1024ull)
#define NES_STATE_MAX_DIRECTORY_ENTRIES 8192u

static const uint8_t state_magic[8] = {
	'N', 'E', 'S', 'D', 'S', 'T', '2', '\0'
};

enum host_command {
	HOST_CMD_NONE = 0,
	HOST_CMD_LOAD_CORE,
	HOST_CMD_UNLOAD_CORE,
	HOST_CMD_LOAD_GAME,
	HOST_CMD_UNLOAD_GAME,
	HOST_CMD_START,
	HOST_CMD_STOP,
	HOST_CMD_RESET,
	HOST_CMD_SET_PAUSED,
	HOST_CMD_FLUSH_SRAM,
	HOST_CMD_LIST_STATES,
	HOST_CMD_SAVE_STATE,
	HOST_CMD_LOAD_STATE,
	HOST_CMD_DELETE_STATE,
	HOST_CMD_SHUTDOWN
};

struct nes_host_private {
	pthread_mutex_t state_mu;
	pthread_mutex_t control_mu;
	pthread_mutex_t submit_mu;
	pthread_cond_t control_cv;
	clockid_t cond_clock;

	enum host_command command;
	bool command_done;
	bool command_bool;
	unsigned command_slot;
	char command_label[NES_STATE_LABEL_BYTES];
	struct nes_state_info *command_states;
	size_t command_state_count;
	bool shutdown_requested;
	int command_result;
	int command_errno;
	char command_path[NES_PATH_MAX];
	uint64_t wake_generation;

	char sram_name[256];
	uint8_t *sram_shadow;
	size_t sram_shadow_size;
	uint64_t next_sram_flush_ms;
	/* Each counter is accessed only by its named worker thread. */
	unsigned sram_temp_serial;
	unsigned state_temp_serial;
	uint8_t rom_digest[NES_SHA256_BYTES];
	uint64_t rom_size;
	unsigned rom_region;
	bool state_temps_cleaned;

	/*
	 * The emulation thread is the only producer. It snapshots libretro SRAM
	 * into this bounded latest-only slot; the writer owns all filesystem I/O.
	 */
	pthread_mutex_t sram_mu;
	pthread_cond_t sram_cv;
	pthread_t sram_thread;
	bool sram_thread_started;
	bool sram_shutdown;
	uint8_t *sram_pending_data;
	size_t sram_pending_size;
	char sram_pending_name[256];
	uint64_t sram_pending_seq;
	uint64_t sram_submitted_seq;
	uint64_t sram_completed_seq;
	int sram_completed_errno;
};

static _Atomic(struct nes_host *) g_host;

static int worker_load_core(struct nes_host *h, const char *core_path);
static int worker_unload_core(struct nes_host *h);
static void worker_discard_core(struct nes_host *h);
static int worker_load_game(struct nes_host *h, const char *rom_path);
static int worker_unload_game(struct nes_host *h);
static void worker_discard_game(struct nes_host *h);
static int worker_flush_sram(struct nes_host *h, bool force, bool wait);
static int worker_list_states(struct nes_host *h,
	struct nes_state_info *states, size_t count);
static int worker_save_state(struct nes_host *h, unsigned slot,
	const char *label, struct nes_state_info *state);
static int worker_load_state(struct nes_host *h, unsigned slot);
static int worker_delete_state(struct nes_host *h, unsigned slot,
	bool *durable);
static void *sram_thread_fn(void *argument);
static int stop_sram_thread(struct nes_host *h);

static uint64_t mono_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_ms(void)
{
	return mono_ns() / 1000000ull;
}

static void timespec_add_ns(struct timespec *ts, uint64_t ns)
{
	uint64_t nsec = (uint64_t)ts->tv_nsec + ns;

	ts->tv_sec += (time_t)(nsec / 1000000000ull);
	ts->tv_nsec = (long)(nsec % 1000000000ull);
}

static void wake_worker(struct nes_host *h)
{
	struct nes_host_private *p;

	if (!h || !(p = h->priv))
		return;
	pthread_mutex_lock(&p->control_mu);
	p->wake_generation++;
	pthread_cond_broadcast(&p->control_cv);
	pthread_mutex_unlock(&p->control_mu);
}

/*
 * Interruptible sleep. A command, viewer change, pause or stop wakes the
 * worker immediately; spurious wakeups are harmless because pacing uses an
 * absolute monotonic target.
 */
static void worker_wait_ns(struct nes_host *h, uint64_t wait_ns)
{
	struct nes_host_private *p = h->priv;
	struct timespec deadline;
	uint64_t generation;
	int result = 0;

	if (wait_ns == 0)
		return;
	if (clock_gettime(p->cond_clock, &deadline) != 0)
		return;
	timespec_add_ns(&deadline, wait_ns);

	pthread_mutex_lock(&p->control_mu);
	generation = p->wake_generation;
	while (p->command == HOST_CMD_NONE &&
	       generation == p->wake_generation) {
		result = pthread_cond_timedwait(&p->control_cv, &p->control_mu,
					       &deadline);
		if (result != 0)
			break;
	}
	pthread_mutex_unlock(&p->control_mu);
}

static void worker_pace(struct nes_host *h, uint64_t *next_ns, double fps)
{
	uint64_t now, step, remaining;

	if (!(fps >= 1.0 && fps <= 1000.0))
		fps = 60.0;
	step = (uint64_t)(1000000000.0 / fps + 0.5);
	if (step < 1000000ull)
		step = 1000000ull;

	now = mono_ns();
	if (*next_ns == 0 || now > *next_ns + step * 3)
		*next_ns = now;
	*next_ns += step;

	now = mono_ns();
	remaining = *next_ns > now ? *next_ns - now : 0;
	worker_wait_ns(h, remaining);
	if (remaining == 0)
		*next_ns = now;
}

static bool validate_geometry(const struct retro_game_geometry *geometry)
{
	unsigned max_w, max_h;

	if (!geometry || geometry->base_width == 0 ||
	    geometry->base_height == 0)
		return false;
	max_w = geometry->max_width ? geometry->max_width :
		geometry->base_width;
	max_h = geometry->max_height ? geometry->max_height :
		geometry->base_height;
	if (geometry->base_width > NES_MAX_W ||
	    geometry->base_height > NES_MAX_H ||
	    max_w > NES_MAX_W || max_h > NES_MAX_H) {
		fprintf(stderr,
			"nesd: core geometry %ux%u (max %ux%u) exceeds "
			"software framebuffer %ux%u\n",
			geometry->base_width, geometry->base_height,
			max_w, max_h, NES_MAX_W, NES_MAX_H);
		return false;
	}
	return true;
}

static bool apply_geometry(struct nes_host *h,
	const struct retro_game_geometry *geometry)
{
	struct nes_host_private *p;

	if (!h || !(p = h->priv) || !validate_geometry(geometry))
		return false;
	pthread_mutex_lock(&p->state_mu);
	h->width = geometry->base_width;
	h->height = geometry->base_height;
	pthread_mutex_unlock(&p->state_mu);
	return true;
}

static bool apply_av_info(struct nes_host *h,
	const struct retro_system_av_info *av)
{
	struct nes_host_private *p;
	double old_rate, new_rate;

	if (!h || !(p = h->priv) || !av || !validate_geometry(&av->geometry))
		return false;

	pthread_mutex_lock(&p->state_mu);
	old_rate = h->sample_rate;
	h->width = av->geometry.base_width;
	h->height = av->geometry.base_height;
	h->fps = av->timing.fps >= 1.0 && av->timing.fps <= 1000.0 ?
		av->timing.fps : 60.0;
	h->sample_rate =
		av->timing.sample_rate >= 1000.0 &&
		av->timing.sample_rate <= 384000.0 ?
		av->timing.sample_rate : 48000.0;
	new_rate = h->sample_rate;
	if (old_rate != new_rate) {
		pthread_mutex_lock(&h->audio_mu);
		h->audio_r = h->audio_w = h->audio_count = 0;
		pthread_mutex_unlock(&h->audio_mu);
	}
	pthread_mutex_unlock(&p->state_mu);
	return true;
}

static void demo_fill_frame(struct nes_host *h)
{
	unsigned x, y;
	uint16_t t = (uint16_t)(h->frame_id & 0xffffu);
	uint16_t joy0;

	joy0 = (uint16_t)atomic_load_explicit(&h->joy[0],
		memory_order_relaxed);

	h->frame_w = 256;
	h->frame_h = 240;
	for (y = 0; y < 240; y++) {
		for (x = 0; x < 256; x++) {
			uint8_t r = (uint8_t)(x + t);
			uint8_t g = (uint8_t)(y + (t >> 1));
			uint8_t b = (uint8_t)((x ^ y) + t);
			h->frame_rgb565[(size_t)y * 256 + x] =
				(uint16_t)(((r & 0xf8u) << 8) |
					   ((g & 0xfcu) << 3) |
					   (b >> 3));
		}
	}
	if (joy0 != 0) {
		for (x = 0; x < 256; x++)
			h->frame_rgb565[(size_t)10 * 256 + x] = 0xffffu;
	}
	h->frame_id++;
}

static uint16_t load_u16_native(const uint8_t *src)
{
	uint16_t value;

	memcpy(&value, src, sizeof(value));
	return value;
}

static uint32_t load_u32_native(const uint8_t *src)
{
	uint32_t value;

	memcpy(&value, src, sizeof(value));
	return value;
}

static void video_cb(const void *data, unsigned width, unsigned height,
	size_t pitch)
{
	struct nes_host *h = atomic_load_explicit(&g_host, memory_order_acquire);
	const uint8_t *src = data;
	enum retro_pixel_format fmt;
	size_t row_bytes;
	unsigned x, y;

	/* NULL is libretro's duplicate-frame signal. */
	if (!h || !data || data == (const void *)(intptr_t)-1 ||
	    width == 0 || height == 0)
		return;
	if (width > NES_MAX_W || height > NES_MAX_H) {
		fprintf(stderr, "nesd: dropped oversized video frame %ux%u\n",
			width, height);
		return;
	}

	fmt = (enum retro_pixel_format)atomic_load_explicit(&h->pixel_fmt,
							    memory_order_relaxed);
	if (fmt == RETRO_PIXEL_FORMAT_XRGB8888)
		row_bytes = (size_t)width * 4;
	else
		row_bytes = (size_t)width * 2;
	if (pitch < row_bytes) {
		fprintf(stderr,
			"nesd: dropped video frame with short pitch %zu < %zu\n",
			pitch, row_bytes);
		return;
	}

	pthread_mutex_lock(&h->frame_mu);
	for (y = 0; y < height; y++) {
		const uint8_t *row = src + (size_t)y * pitch;
		uint16_t *dst = h->frame_rgb565 + (size_t)y * width;

		if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
			memcpy(dst, row, row_bytes);
			continue;
		}
		for (x = 0; x < width; x++) {
			if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t pixel = load_u32_native(row + (size_t)x * 4);
				uint8_t r = (uint8_t)(pixel >> 16);
				uint8_t g = (uint8_t)(pixel >> 8);
				uint8_t b = (uint8_t)pixel;
				dst[x] = (uint16_t)(((r & 0xf8u) << 8) |
						    ((g & 0xfcu) << 3) |
						    (b >> 3));
			} else {
				uint16_t pixel =
					load_u16_native(row + (size_t)x * 2);
				unsigned r5 = (pixel >> 10) & 0x1fu;
				unsigned g5 = (pixel >> 5) & 0x1fu;
				unsigned b5 = pixel & 0x1fu;
				unsigned g6 = (g5 << 1) | (g5 >> 4);
				dst[x] = (uint16_t)((r5 << 11) |
						    (g6 << 5) | b5);
			}
		}
	}
	h->frame_w = width;
	h->frame_h = height;
	h->pitch = (size_t)width * 2;
	h->frame_id++;
	pthread_mutex_unlock(&h->frame_mu);
}

static void audio_push_frames(struct nes_host *h, const int16_t *data,
	size_t frames)
{
	size_t first, dropped;

	if (!h || !data || frames == 0 || frames > SIZE_MAX / 2)
		return;

	pthread_mutex_lock(&h->audio_mu);
	if (frames >= NES_AUDIO_RING_FRAMES) {
		data += (frames - NES_AUDIO_RING_FRAMES) * 2;
		frames = NES_AUDIO_RING_FRAMES;
		h->audio_r = h->audio_w = h->audio_count = 0;
	} else if (h->audio_count + frames > NES_AUDIO_RING_FRAMES) {
		dropped = h->audio_count + frames - NES_AUDIO_RING_FRAMES;
		h->audio_r = (h->audio_r + dropped) % NES_AUDIO_RING_FRAMES;
		h->audio_count -= dropped;
	}

	first = NES_AUDIO_RING_FRAMES - h->audio_w;
	if (first > frames)
		first = frames;
	memcpy(h->audio_ring + h->audio_w * 2, data,
	       first * 2 * sizeof(*data));
	if (first < frames) {
		memcpy(h->audio_ring, data + first * 2,
		       (frames - first) * 2 * sizeof(*data));
	}
	h->audio_w = (h->audio_w + frames) % NES_AUDIO_RING_FRAMES;
	h->audio_count += frames;
	h->audio_seq++;
	pthread_mutex_unlock(&h->audio_mu);
}

static void audio_cb(int16_t left, int16_t right)
{
	struct nes_host *h = atomic_load_explicit(&g_host, memory_order_acquire);
	int16_t pair[2];

	if (!h)
		return;
	pair[0] = left;
	pair[1] = right;
	audio_push_frames(h, pair, 1);
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	struct nes_host *h = atomic_load_explicit(&g_host, memory_order_acquire);

	if (h && data)
		audio_push_frames(h, data, frames);
	return frames;
}

static void demo_fill_audio(struct nes_host *h)
{
	const unsigned rate = 48000;
	const unsigned count = rate / DEMO_FPS;
	int16_t buffer[(48000 / DEMO_FPS) * 2];
	unsigned i;
	int16_t amplitude = 0;

	if (atomic_load_explicit(&h->joy[0], memory_order_relaxed) != 0)
		amplitude = 4000;

	pthread_mutex_lock(&h->priv->state_mu);
	h->sample_rate = rate;
	pthread_mutex_unlock(&h->priv->state_mu);

	for (i = 0; i < count; i++) {
		int16_t sample = 0;

		if (amplitude != 0) {
			h->demo_phase += 440;
			if (h->demo_phase >= rate)
				h->demo_phase -= rate;
			sample = h->demo_phase < rate / 2 ?
				amplitude : (int16_t)-amplitude;
		}
		buffer[i * 2] = sample;
		buffer[i * 2 + 1] = sample;
	}
	audio_push_frames(h, buffer, count);
}

static void input_poll_cb(void)
{
}

static int16_t input_state_cb(unsigned port, unsigned device,
	unsigned index, unsigned id)
{
	struct nes_host *h = atomic_load_explicit(&g_host, memory_order_acquire);
	int16_t value;

	(void)index;
	if (!h || port > 1 || device != RETRO_DEVICE_JOYPAD ||
	    id >= NES_JOY_BITS)
		return 0;

	value = (atomic_load_explicit(&h->joy[port], memory_order_relaxed) &
		 (1u << id)) != 0 ? 1 : 0;
	return value;
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	va_list args;
	const char *tag = "I";

	if (level == RETRO_LOG_DEBUG)
		tag = "D";
	else if (level == RETRO_LOG_WARN)
		tag = "W";
	else if (level == RETRO_LOG_ERROR)
		tag = "E";

	fprintf(stderr, "[fceumm:%s] ", tag);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static bool environ_cb(unsigned command, void *data)
{
	struct nes_host *h = atomic_load_explicit(&g_host, memory_order_acquire);

	switch (command) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format format;

		if (!h || !data)
			return false;
		format = *(const enum retro_pixel_format *)data;
		if (format != RETRO_PIXEL_FORMAT_0RGB1555 &&
		    format != RETRO_PIXEL_FORMAT_XRGB8888 &&
		    format != RETRO_PIXEL_FORMAT_RGB565)
			return false;
		atomic_store_explicit(&h->pixel_fmt, (int)format,
				      memory_order_relaxed);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (!data)
			return false;
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		if (!h || !data)
			return false;
		*(const char **)data = h->system_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (!h || !data)
			return false;
		*(const char **)data = h->save_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (!data)
			return false;
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		if (data)
			((struct retro_variable *)data)->value = NULL;
		return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (!data)
			return false;
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		return h && data &&
			apply_geometry(h, (const struct retro_game_geometry *)data);
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		return h && data &&
			apply_av_info(h, (const struct retro_system_av_info *)data);
	case RETRO_ENVIRONMENT_SHUTDOWN:
		if (!h)
			return false;
		atomic_store_explicit(&h->running, false, memory_order_release);
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_MESSAGE:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
	default:
		return false;
	}
}

static int bind_symbol(void *handle, const char *name, void *slot,
	size_t slot_size)
{
	void *symbol;
	const char *error;

	dlerror();
	symbol = dlsym(handle, name);
	error = dlerror();
	if (error || !symbol) {
		fprintf(stderr, "dlsym(%s): %s\n", name,
			error ? error : "symbol not found");
		return -1;
	}
	if (slot_size != sizeof(symbol)) {
		fprintf(stderr, "dlsym(%s): incompatible function pointer size\n",
			name);
		return -1;
	}
	memcpy(slot, &symbol, sizeof(symbol));
	return 0;
}

#define BIND_CORE_SYMBOL(core, handle, field) \
	bind_symbol((handle), #field, &(core)->field, sizeof((core)->field))

static int bind_core(struct nes_host *h)
{
	struct retro_core *core = &h->core;
	void *handle = h->dl;

	memset(core, 0, sizeof(*core));
	if (BIND_CORE_SYMBOL(core, handle, retro_init) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_deinit) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_api_version) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_get_system_info) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_get_system_av_info) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_environment) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_video_refresh) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_audio_sample) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_audio_sample_batch) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_input_poll) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_input_state) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_set_controller_port_device) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_reset) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_run) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_serialize_size) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_serialize) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_unserialize) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_load_game) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_unload_game) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_get_region) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_get_memory_data) != 0 ||
	    BIND_CORE_SYMBOL(core, handle, retro_get_memory_size) != 0)
		return -1;
	return 0;
}

#undef BIND_CORE_SYMBOL

static void clear_audio(struct nes_host *h)
{
	pthread_mutex_lock(&h->audio_mu);
	h->audio_r = h->audio_w = h->audio_count = 0;
	pthread_mutex_unlock(&h->audio_mu);
}

static void clear_frame(struct nes_host *h)
{
	pthread_mutex_lock(&h->frame_mu);
	memset(h->frame_rgb565, 0, sizeof(h->frame_rgb565));
	h->frame_id = 0;
	h->frame_w = 0;
	h->frame_h = 0;
	pthread_mutex_unlock(&h->frame_mu);
}

static void set_idle_identity(struct nes_host *h)
{
	struct nes_host_private *p = h->priv;

	pthread_mutex_lock(&p->state_mu);
	h->core_path[0] = '\0';
	h->library_name[0] = '\0';
	h->library_version[0] = '\0';
	h->width = 0;
	h->height = 0;
	h->fps = 0.0;
	h->sample_rate = 0.0;
	pthread_mutex_unlock(&p->state_mu);
	atomic_store_explicit(&h->demo_mode, false, memory_order_release);
}

static void enable_demo_mode(struct nes_host *h)
{
	struct nes_host_private *p = h->priv;

	pthread_mutex_lock(&p->state_mu);
	if (!atomic_load_explicit(&h->core_loaded, memory_order_acquire)) {
		snprintf(h->library_name, sizeof(h->library_name), "demo");
		snprintf(h->library_version, sizeof(h->library_version),
			 "built-in");
	}
	h->width = 256;
	h->height = 240;
	h->fps = 20.0;
	h->sample_rate = 48000.0;
	pthread_mutex_unlock(&p->state_mu);
	atomic_store_explicit(&h->demo_mode, true, memory_order_release);
}

static int worker_finish_core_init(struct nes_host *h)
{
	struct retro_system_info info;
	char name[sizeof(h->library_name)];
	char version[sizeof(h->library_version)];

	if (h->core.retro_api_version() != RETRO_API_VERSION) {
		fprintf(stderr, "nesd: unsupported libretro API version\n");
		return -1;
	}

	/*
	 * Libretro callbacks must all be installed before retro_init(). Some
	 * cores use them from their initialisation path.
	 */
	atomic_store_explicit(&h->pixel_fmt, RETRO_PIXEL_FORMAT_0RGB1555,
			      memory_order_relaxed);
	h->core.retro_set_environment(environ_cb);
	h->core.retro_set_video_refresh(video_cb);
	h->core.retro_set_audio_sample(audio_cb);
	h->core.retro_set_audio_sample_batch(audio_batch_cb);
	h->core.retro_set_input_poll(input_poll_cb);
	h->core.retro_set_input_state(input_state_cb);
	h->core.retro_init();
	h->core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	h->core.retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

	memset(&info, 0, sizeof(info));
	h->core.retro_get_system_info(&info);
	snprintf(name, sizeof(name), "%s",
		 info.library_name ? info.library_name : "unknown");
	snprintf(version, sizeof(version), "%s",
		 info.library_version ? info.library_version : "?");

	pthread_mutex_lock(&h->priv->state_mu);
	snprintf(h->library_name, sizeof(h->library_name), "%s", name);
	snprintf(h->library_version, sizeof(h->library_version), "%s", version);
	pthread_mutex_unlock(&h->priv->state_mu);
	atomic_store_explicit(&h->core_loaded, true, memory_order_release);
	atomic_store_explicit(&h->demo_mode, false, memory_order_release);
	fprintf(stderr, "nesd: loaded core %s %s (%s)\n", name, version,
		h->core_path);
	return 0;
}

static int worker_load_core(struct nes_host *h, const char *core_path)
{
	bool builtin;

	if (worker_unload_core(h) != 0)
		return -1;
	builtin = !core_path || !core_path[0] ||
		strcmp(core_path, "builtin") == 0 ||
		strcmp(core_path, "fceumm") == 0 ||
		strcmp(core_path, "builtin:fceumm") == 0;

	if (builtin) {
		if (host_bind_builtin_fceumm(h) != 0) {
			fprintf(stderr, "nesd: builtin FCEUmm is not available\n");
			set_idle_identity(h);
			return -1;
		}
		pthread_mutex_lock(&h->priv->state_mu);
		snprintf(h->core_path, sizeof(h->core_path),
			 "builtin:fceumm");
		pthread_mutex_unlock(&h->priv->state_mu);
	} else {
		h->dl = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
		if (!h->dl) {
			fprintf(stderr, "dlopen(%s): %s; trying builtin FCEUmm\n",
				core_path, dlerror());
			if (host_bind_builtin_fceumm(h) != 0) {
				set_idle_identity(h);
				return -1;
			}
			pthread_mutex_lock(&h->priv->state_mu);
			snprintf(h->core_path, sizeof(h->core_path),
				 "builtin:fceumm");
			pthread_mutex_unlock(&h->priv->state_mu);
		} else if (bind_core(h) != 0) {
			dlclose(h->dl);
			h->dl = NULL;
			memset(&h->core, 0, sizeof(h->core));
			if (host_bind_builtin_fceumm(h) != 0) {
				set_idle_identity(h);
				return -1;
			}
			pthread_mutex_lock(&h->priv->state_mu);
			snprintf(h->core_path, sizeof(h->core_path),
				 "builtin:fceumm");
			pthread_mutex_unlock(&h->priv->state_mu);
		} else {
			pthread_mutex_lock(&h->priv->state_mu);
			snprintf(h->core_path, sizeof(h->core_path), "%s",
				 core_path);
			pthread_mutex_unlock(&h->priv->state_mu);
		}
	}

	if (worker_finish_core_init(h) != 0) {
		if (h->dl) {
			dlclose(h->dl);
			h->dl = NULL;
		}
		memset(&h->core, 0, sizeof(h->core));
		set_idle_identity(h);
		return -1;
	}
	return 0;
}

static void reset_sram_tracking(struct nes_host *h)
{
	struct nes_host_private *p = h->priv;

	free(p->sram_shadow);
	p->sram_shadow = NULL;
	p->sram_shadow_size = 0;
	p->sram_name[0] = '\0';
	p->next_sram_flush_ms = 0;
}

static void worker_discard_core(struct nes_host *h)
{
	if (atomic_load_explicit(&h->game_loaded, memory_order_acquire))
		worker_discard_game(h);
	if (atomic_load_explicit(&h->core_loaded, memory_order_acquire) &&
	    h->core.retro_deinit)
		h->core.retro_deinit();
	if (h->dl) {
		dlclose(h->dl);
		h->dl = NULL;
	}
	memset(&h->core, 0, sizeof(h->core));
	atomic_store_explicit(&h->core_loaded, false, memory_order_release);
	set_idle_identity(h);
}

static int worker_unload_core(struct nes_host *h)
{
	if (atomic_load_explicit(&h->game_loaded, memory_order_acquire) &&
	    worker_unload_game(h) != 0)
		return -1;
	worker_discard_core(h);
	return 0;
}

static int read_rom_file(const char *path, void **out, size_t *out_size)
{
	struct stat st;
	uint8_t *buffer;
	size_t done, size;
	ssize_t got;
	int fd;

	if (!path || !out || !out_size) {
		errno = EINVAL;
		return -1;
	}
	*out = NULL;
	*out_size = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uint64_t)st.st_size > NES_MAX_ROM_BYTES) {
		close(fd);
		errno = EFBIG;
		return -1;
	}
	size = (size_t)st.st_size;
	buffer = malloc(size);
	if (!buffer) {
		close(fd);
		return -1;
	}
	for (done = 0; done < size; done += (size_t)got) {
		got = read(fd, buffer + done, size - done);
		if (got < 0 && errno == EINTR) {
			got = 0;
			continue;
		}
		if (got <= 0) {
			free(buffer);
			close(fd);
			errno = got == 0 ? EIO : errno;
			return -1;
		}
	}
	*out = buffer;
	*out_size = size;
	if (close(fd) != 0) {
		int saved_errno = errno;

		free(buffer);
		*out = NULL;
		*out_size = 0;
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static uint64_t fnv1a64(const char *text)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	while (*text) {
		hash ^= (unsigned char)*text++;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static void make_sram_name(struct nes_host *h, const char *rom_path)
{
	struct nes_host_private *p = h->priv;
	const char *base = strrchr(rom_path, '/');
	char safe[160];
	size_t i = 0;
	uint64_t hash = fnv1a64(rom_path);

	base = base ? base + 1 : rom_path;
	while (*base && i + 1 < sizeof(safe)) {
		unsigned char c = (unsigned char)*base++;

		if (c == '.' && strchr(base, '.') == NULL)
			break;
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' || c == '_')
			safe[i++] = (char)c;
		else
			safe[i++] = '_';
	}
	while (i > 0 && safe[i - 1] == '_')
		i--;
	if (i == 0) {
		memcpy(safe, "game", 4);
		i = 4;
	}
	safe[i] = '\0';
	snprintf(p->sram_name, sizeof(p->sram_name),
		 "%s-%016llx.srm", safe, (unsigned long long)hash);
}

static int open_save_directory(struct nes_host *h)
{
	int fd = open(h->save_dir,
		      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

	if (fd >= 0)
		return fd;
	if (errno != ENOENT || mkdir(h->save_dir, 0750) != 0)
		return -1;
	return open(h->save_dir,
		    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static int worker_load_sram(struct nes_host *h)
{
	struct nes_host_private *p = h->priv;
	uint8_t *memory, *loaded = NULL;
	struct stat st;
	size_t size, wanted, done = 0;
	ssize_t got;
	int dirfd, fd;

	if (!h->core.retro_get_memory_data ||
	    !h->core.retro_get_memory_size)
		return 0;
	memory = h->core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size = h->core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!memory || size == 0)
		return 0;

	dirfd = open_save_directory(h);
	if (dirfd < 0) {
		fprintf(stderr, "nesd: cannot open save directory %s: %s\n",
			h->save_dir, strerror(errno));
		return -1;
	}
	fd = openat(dirfd, p->sram_name,
		    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd >= 0) {
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
		    st.st_size < 0) {
			fprintf(stderr, "nesd: refusing invalid SRAM file %s\n",
				p->sram_name);
			close(fd);
			close(dirfd);
			return -1;
		}
		wanted = (uint64_t)st.st_size < size ?
			(size_t)st.st_size : size;
		loaded = calloc(1, size);
		if (!loaded) {
			close(fd);
			close(dirfd);
			return -1;
		}
		while (done < wanted) {
			got = read(fd, loaded + done, wanted - done);
			if (got < 0 && errno == EINTR)
				continue;
			if (got <= 0) {
				fprintf(stderr, "nesd: short read from SRAM %s\n",
					p->sram_name);
				free(loaded);
				close(fd);
				close(dirfd);
				return -1;
			}
			done += (size_t)got;
		}
		memcpy(memory, loaded, size);
		free(loaded);
		if ((uint64_t)st.st_size != size) {
			fprintf(stderr,
				"nesd: SRAM %s size %lld differs from core size "
				"%zu; data was safely resized\n",
				p->sram_name, (long long)st.st_size, size);
		}
		fprintf(stderr, "nesd: loaded SRAM %s (%zu bytes)\n",
			p->sram_name, size);
		close(fd);
	} else if (errno != ENOENT) {
		fprintf(stderr, "nesd: cannot read SRAM %s: %s\n",
			p->sram_name, strerror(errno));
		close(dirfd);
		return -1;
	}
	close(dirfd);

	p->sram_shadow = malloc(size);
	if (!p->sram_shadow)
		return -1;
	memcpy(p->sram_shadow, memory, size);
	p->sram_shadow_size = size;
	p->next_sram_flush_ms = mono_ms() + SRAM_FLUSH_INTERVAL_MS;
	return 0;
}

static int atomic_write_at(struct nes_host *h, int dirfd, const char *name,
	const void *data, size_t size)
{
	struct nes_host_private *p = h->priv;
	char temporary[300];
	const uint8_t *bytes = data;
	size_t done = 0;
	ssize_t wrote;
	unsigned attempt;
	int fd = -1;
	int saved_errno = 0;

	for (attempt = 0; attempt < 32; attempt++) {
		p->sram_temp_serial++;
		snprintf(temporary, sizeof(temporary), ".%s.tmp.%ld.%u",
			 name, (long)getpid(), p->sram_temp_serial);
		fd = openat(dirfd, temporary,
			    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
			    O_NOFOLLOW, 0600);
		if (fd >= 0)
			break;
		if (errno != EEXIST)
			return -1;
	}
	if (fd < 0) {
		errno = EEXIST;
		return -1;
	}

	while (done < size) {
		wrote = write(fd, bytes + done, size - done);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			goto fail;
		done += (size_t)wrote;
	}
	if (fsync(fd) != 0)
		goto fail;
	if (close(fd) != 0) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (renameat(dirfd, temporary, dirfd, name) != 0)
		goto fail;
	/*
	 * The rename makes the new SRAM visible, while the directory fsync makes
	 * that name durable across a sudden power loss. A few filesystems report
	 * EINVAL/EOPNOTSUPP for directory fsync; other errors (notably EIO) must
	 * reach the caller instead of falsely reporting a durable save.
	 */
	if (fsync(dirfd) != 0 && errno != EINVAL && errno != EOPNOTSUPP)
		goto fail;
	return 0;

fail:
	saved_errno = errno ? errno : EIO;
	if (fd >= 0)
		close(fd);
	(void)unlinkat(dirfd, temporary, 0);
	errno = saved_errno;
	return -1;
}

static int write_sram_snapshot(struct nes_host *h, const char *name,
	const uint8_t *data, size_t size)
{
	int dirfd;
	int result;
	int saved_errno;

	dirfd = open_save_directory(h);
	if (dirfd < 0) {
		fprintf(stderr, "nesd: cannot open save directory %s: %s\n",
			h->save_dir, strerror(errno));
		return -1;
	}
	result = atomic_write_at(h, dirfd, name, data, size);
	saved_errno = result == 0 ? 0 : (errno ? errno : EIO);
	if (result != 0) {
		fprintf(stderr, "nesd: cannot atomically save SRAM %s: %s\n",
			name, strerror(saved_errno));
	}
	close(dirfd);
	if (result != 0)
		errno = saved_errno;
	return result;
}

static void *sram_thread_fn(void *argument)
{
	struct nes_host *h = argument;
	struct nes_host_private *p = h->priv;

	for (;;) {
		uint8_t *data;
		size_t size;
		char name[sizeof(p->sram_pending_name)];
		uint64_t sequence;
		int result;
		int saved_errno;

		pthread_mutex_lock(&p->sram_mu);
		while (!p->sram_pending_data && !p->sram_shutdown)
			pthread_cond_wait(&p->sram_cv, &p->sram_mu);
		if (!p->sram_pending_data && p->sram_shutdown) {
			pthread_mutex_unlock(&p->sram_mu);
			break;
		}
		data = p->sram_pending_data;
		size = p->sram_pending_size;
		sequence = p->sram_pending_seq;
		snprintf(name, sizeof(name), "%s", p->sram_pending_name);
		p->sram_pending_data = NULL;
		p->sram_pending_size = 0;
		p->sram_pending_name[0] = '\0';
		p->sram_pending_seq = 0;
		pthread_mutex_unlock(&p->sram_mu);

		errno = 0;
		result = write_sram_snapshot(h, name, data, size);
		saved_errno = result == 0 ? 0 : (errno ? errno : EIO);
		if (result == 0)
			fprintf(stderr, "nesd: saved SRAM %s (%zu bytes)\n",
				name, size);
		free(data);

		pthread_mutex_lock(&p->sram_mu);
		p->sram_completed_seq = sequence;
		p->sram_completed_errno = saved_errno;
		pthread_cond_broadcast(&p->sram_cv);
		pthread_mutex_unlock(&p->sram_mu);
	}
	return NULL;
}

static int wait_for_sram_sequence(struct nes_host_private *p,
	uint64_t sequence)
{
	int error;

	if (sequence == 0)
		return 0;
	pthread_mutex_lock(&p->sram_mu);
	while (p->sram_completed_seq < sequence) {
		error = pthread_cond_wait(&p->sram_cv, &p->sram_mu);
		if (error != 0) {
			pthread_mutex_unlock(&p->sram_mu);
			errno = error;
			return -1;
		}
	}
	error = p->sram_completed_seq == sequence ?
		p->sram_completed_errno : EIO;
	pthread_mutex_unlock(&p->sram_mu);
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

static int queue_sram_snapshot(struct nes_host *h, const uint8_t *memory,
	size_t size, bool wait)
{
	struct nes_host_private *p = h->priv;
	uint8_t *snapshot;
	uint64_t sequence;

	snapshot = malloc(size);
	if (!snapshot)
		return -1;
	memcpy(snapshot, memory, size);

	pthread_mutex_lock(&p->sram_mu);
	if (!p->sram_thread_started || p->sram_shutdown) {
		pthread_mutex_unlock(&p->sram_mu);
		free(snapshot);
		errno = ESHUTDOWN;
		return -1;
	}
	free(p->sram_pending_data);
	p->sram_pending_data = snapshot;
	p->sram_pending_size = size;
	snprintf(p->sram_pending_name, sizeof(p->sram_pending_name), "%s",
		 p->sram_name);
	sequence = ++p->sram_submitted_seq;
	p->sram_pending_seq = sequence;
	pthread_cond_signal(&p->sram_cv);
	pthread_mutex_unlock(&p->sram_mu);

	return wait ? wait_for_sram_sequence(p, sequence) : 0;
}

static int wait_for_latest_sram(struct nes_host_private *p)
{
	uint64_t sequence;

	pthread_mutex_lock(&p->sram_mu);
	sequence = p->sram_submitted_seq;
	pthread_mutex_unlock(&p->sram_mu);
	return wait_for_sram_sequence(p, sequence);
}

static bool latest_sram_write_failed(struct nes_host_private *p)
{
	bool failed;

	pthread_mutex_lock(&p->sram_mu);
	failed = p->sram_completed_seq != 0 &&
		p->sram_completed_seq == p->sram_submitted_seq &&
		p->sram_completed_errno != 0;
	pthread_mutex_unlock(&p->sram_mu);
	return failed;
}

static int worker_flush_sram(struct nes_host *h, bool force, bool wait)
{
	struct nes_host_private *p = h->priv;
	uint8_t *memory, *new_shadow;
	size_t size;
	bool unchanged;
	int result;

	if (!atomic_load_explicit(&h->core_loaded, memory_order_acquire) ||
	    !atomic_load_explicit(&h->game_loaded, memory_order_acquire) ||
	    !h->core.retro_get_memory_data ||
	    !h->core.retro_get_memory_size || !p->sram_name[0])
		return 0;
	p->next_sram_flush_ms = mono_ms() + SRAM_FLUSH_INTERVAL_MS;
	memory = h->core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size = h->core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!memory || size == 0)
		return wait ? wait_for_latest_sram(p) : 0;

	unchanged = p->sram_shadow && p->sram_shadow_size == size &&
		memcmp(p->sram_shadow, memory, size) == 0;
	if (!force && unchanged && !latest_sram_write_failed(p))
		return wait ? wait_for_latest_sram(p) : 0;

	result = queue_sram_snapshot(h, memory, size, wait);
	if (result != 0)
		return -1;

	new_shadow = realloc(p->sram_shadow, size);
	if (new_shadow) {
		p->sram_shadow = new_shadow;
		memcpy(p->sram_shadow, memory, size);
		p->sram_shadow_size = size;
	}
	return 0;
}

static uint32_t state_load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] |
	       ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) |
	       ((uint32_t)bytes[3] << 24);
}

static uint16_t state_load_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint64_t state_load_le64(const uint8_t *bytes)
{
	return (uint64_t)state_load_le32(bytes) |
	       ((uint64_t)state_load_le32(bytes + 4) << 32);
}

static void state_store_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void state_store_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void state_store_le64(uint8_t *bytes, uint64_t value)
{
	state_store_le32(bytes, (uint32_t)value);
	state_store_le32(bytes + 4, (uint32_t)(value >> 32));
}

static bool state_label_valid_n(const char *text, size_t length)
{
	const uint8_t *bytes = (const uint8_t *)text;
	size_t offset = 0;

	while (offset < length) {
		uint8_t first = bytes[offset];
		size_t sequence = 0;

		if (first < 0x80) {
			if (first < 0x20 || first == 0x7f)
				return false;
			offset++;
			continue;
		}
		if (first >= 0xc2 && first <= 0xdf)
			sequence = 2;
		else if (first >= 0xe0 && first <= 0xef)
			sequence = 3;
		else if (first >= 0xf0 && first <= 0xf4)
			sequence = 4;
		else
			return false;
		if (sequence > length - offset)
			return false;
		if (sequence >= 2 &&
		    (bytes[offset + 1] < 0x80 || bytes[offset + 1] > 0xbf))
			return false;
		if (sequence >= 3 &&
		    (bytes[offset + 2] < 0x80 || bytes[offset + 2] > 0xbf))
			return false;
		if (sequence == 4 &&
		    (bytes[offset + 3] < 0x80 || bytes[offset + 3] > 0xbf))
			return false;
		if ((first == 0xe0 && bytes[offset + 1] < 0xa0) ||
		    (first == 0xed && bytes[offset + 1] > 0x9f) ||
		    (first == 0xf0 && bytes[offset + 1] < 0x90) ||
		    (first == 0xf4 && bytes[offset + 1] > 0x8f))
			return false;
		offset += sequence;
	}
	return true;
}

static bool state_label_valid(const char *label)
{
	size_t length;

	if (!label)
		return true;
	length = strlen(label);
	return length < NES_STATE_LABEL_BYTES &&
		state_label_valid_n(label, length);
}

static void state_core_digest(struct nes_host *h,
	uint8_t digest[NES_SHA256_BYTES])
{
	struct nes_sha256 context;
	static const uint8_t separator = 0;

	nes_sha256_init(&context);
	nes_sha256_update(&context, h->library_name, strlen(h->library_name));
	nes_sha256_update(&context, &separator, sizeof(separator));
	nes_sha256_update(&context, h->library_version,
		strlen(h->library_version));
	nes_sha256_final(&context, digest);
}

static void state_digest_hex(const uint8_t digest[NES_SHA256_BYTES],
	char output[NES_SHA256_BYTES * 2 + 1])
{
	static const char digits[] = "0123456789abcdef";
	unsigned i;

	for (i = 0; i < NES_SHA256_BYTES; i++) {
		output[i * 2] = digits[digest[i] >> 4];
		output[i * 2 + 1] = digits[digest[i] & 15];
	}
	output[NES_SHA256_BYTES * 2] = '\0';
}

static int state_make_name(struct nes_host *h, unsigned slot,
	char *name, size_t size)
{
	char digest_hex[NES_SHA256_BYTES * 2 + 1];
	int length;

	if (!atomic_load_explicit(&h->game_loaded, memory_order_acquire)) {
		errno = ENODATA;
		return -1;
	}
	if (slot < 1 || slot > NES_STATE_SLOT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	state_digest_hex(h->priv->rom_digest, digest_hex);
	length = snprintf(name, size, "nesstate-%s-region-%u-slot-%02u.nss",
		digest_hex, h->priv->rom_region, slot);
	if (length < 0 || (size_t)length >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int state_directory_metadata_valid(int fd, mode_t required_private)
{
	struct stat st;

	if (fstat(fd, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & required_private) != 0) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static int open_state_directory(struct nes_host *h, bool create)
{
	int save_fd;
	int state_fd;
	int saved_errno;

	save_fd = open_save_directory(h);
	if (save_fd < 0)
		return -1;
	if (state_directory_metadata_valid(save_fd, 0022) != 0)
		goto fail_save;
	state_fd = openat(save_fd, "states",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (state_fd < 0 && errno == ENOENT && create) {
		if (mkdirat(save_fd, "states", 0700) != 0 && errno != EEXIST)
			goto fail_save;
		state_fd = openat(save_fd, "states",
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	}
	if (state_fd < 0)
		goto fail_save;
	if (state_directory_metadata_valid(state_fd, 0077) != 0) {
		saved_errno = errno;
		close(state_fd);
		close(save_fd);
		errno = saved_errno;
		return -1;
	}
	close(save_fd);
	return state_fd;

fail_save:
	saved_errno = errno ? errno : EIO;
	close(save_fd);
	errno = saved_errno;
	return -1;
}

static bool state_temp_name_valid(const char *name)
{
	const char *marker;
	const char *tail;

	if (strncmp(name, ".nesstate-", 10) != 0)
		return false;
	marker = strstr(name + 10, ".nss.tmp.");
	if (!marker)
		return false;
	tail = marker + strlen(".nss.tmp.");
	if (!*tail)
		return false;
	while (*tail) {
		if ((*tail < '0' || *tail > '9') && *tail != '.')
			return false;
		tail++;
	}
	return true;
}

static int state_cleanup_temps(struct nes_host *h, int dirfd)
{
	struct nes_host_private *p = h->priv;
	DIR *directory;
	struct dirent *entry;
	int duplicate;
	int changed = 0;
	int saved_errno = 0;

	if (p->state_temps_cleaned)
		return 0;
	duplicate = dup(dirfd);
	if (duplicate < 0)
		return -1;
	directory = fdopendir(duplicate);
	if (!directory) {
		saved_errno = errno;
		close(duplicate);
		errno = saved_errno;
		return -1;
	}
	for (;;) {
		struct stat st;

		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			if (errno)
				saved_errno = errno;
			break;
		}
		if (!state_temp_name_valid(entry->d_name))
			continue;
		if (fstatat(dirfd, entry->d_name, &st,
			AT_SYMLINK_NOFOLLOW) != 0) {
			if (errno == ENOENT)
				continue;
			saved_errno = errno;
			break;
		}
		if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
		    st.st_uid != geteuid())
			continue;
		if (unlinkat(dirfd, entry->d_name, 0) != 0 && errno != ENOENT) {
			saved_errno = errno;
			break;
		}
		changed = 1;
	}
	if (closedir(directory) != 0 && !saved_errno)
		saved_errno = errno;
	if (!saved_errno && changed && fsync(dirfd) != 0 &&
	    errno != EINVAL && errno != EOPNOTSUPP)
		saved_errno = errno;
	if (saved_errno) {
		errno = saved_errno;
		return -1;
	}
	p->state_temps_cleaned = true;
	return 0;
}

static int state_check_capacity(int dirfd, const char *target,
	uint64_t new_size)
{
	DIR *directory;
	struct dirent *entry;
	struct statvfs filesystem;
	uint64_t total = 0;
	uint64_t existing = 0;
	uint64_t available;
	uint64_t required;
	unsigned entries = 0;
	int duplicate;
	int saved_errno = 0;

	duplicate = dup(dirfd);
	if (duplicate < 0)
		return -1;
	directory = fdopendir(duplicate);
	if (!directory) {
		saved_errno = errno;
		close(duplicate);
		errno = saved_errno;
		return -1;
	}
	for (;;) {
		struct stat st;
		uint64_t file_size;

		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			if (errno)
				saved_errno = errno;
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (++entries > NES_STATE_MAX_DIRECTORY_ENTRIES) {
			saved_errno = EDQUOT;
			break;
		}
		if (fstatat(dirfd, entry->d_name, &st,
			AT_SYMLINK_NOFOLLOW) != 0) {
			if (errno == ENOENT)
				continue;
			saved_errno = errno;
			break;
		}
		if (!S_ISREG(st.st_mode) || st.st_size < 0)
			continue;
		file_size = (uint64_t)st.st_size;
		if (total > UINT64_MAX - file_size) {
			saved_errno = EOVERFLOW;
			break;
		}
		total += file_size;
		if (strcmp(entry->d_name, target) == 0) {
			if (st.st_nlink != 1 || st.st_uid != geteuid()) {
				saved_errno = EPERM;
				break;
			}
			existing = file_size;
		}
	}
	if (closedir(directory) != 0 && !saved_errno)
		saved_errno = errno;
	if (saved_errno) {
		errno = saved_errno;
		return -1;
	}
	if (total < existing || total - existing > UINT64_MAX - new_size ||
	    total - existing + new_size > NES_STATE_TOTAL_QUOTA) {
		errno = EDQUOT;
		return -1;
	}
	if (fstatvfs(dirfd, &filesystem) != 0)
		return -1;
	if (filesystem.f_frsize != 0 &&
	    (uint64_t)filesystem.f_bavail >
		UINT64_MAX / (uint64_t)filesystem.f_frsize)
		available = UINT64_MAX;
	else
		available = (uint64_t)filesystem.f_bavail *
			(uint64_t)filesystem.f_frsize;
	if (new_size > UINT64_MAX - NES_STATE_FREE_RESERVE)
		required = UINT64_MAX;
	else
		required = new_size + NES_STATE_FREE_RESERVE;
	if (available < required) {
		errno = ENOSPC;
		return -1;
	}
	return 0;
}

static int state_atomic_write(struct nes_host *h, int dirfd,
	const char *name, const void *data, size_t size, bool *durable)
{
	struct nes_host_private *p = h->priv;
	char temporary[320];
	const uint8_t *bytes = data;
	size_t done = 0;
	ssize_t wrote;
	unsigned attempt;
	int fd = -1;
	int saved_errno;

	*durable = false;
	for (attempt = 0; attempt < 32; attempt++) {
		p->state_temp_serial++;
		int length = snprintf(temporary, sizeof(temporary),
			".%s.tmp.%ld.%u", name, (long)getpid(),
			p->state_temp_serial);

		if (length < 0 || (size_t)length >= sizeof(temporary)) {
			errno = EIO;
			return -1;
		}
		fd = openat(dirfd, temporary,
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
			O_NOFOLLOW, 0600);
		if (fd >= 0)
			break;
		if (errno != EEXIST)
			return -1;
	}
	if (fd < 0) {
		errno = EEXIST;
		return -1;
	}
	while (done < size) {
		wrote = write(fd, bytes + done, size - done);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			goto fail;
		done += (size_t)wrote;
	}
	if (fsync(fd) != 0)
		goto fail;
	if (close(fd) != 0) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (renameat(dirfd, temporary, dirfd, name) != 0)
		goto fail;
	if (fsync(dirfd) == 0 || errno == EINVAL || errno == EOPNOTSUPP)
		*durable = true;
	return 0;

fail:
	saved_errno = errno ? errno : EIO;
	if (fd >= 0)
		close(fd);
	(void)unlinkat(dirfd, temporary, 0);
	errno = saved_errno;
	return -1;
}

static bool state_fcs_valid(const uint8_t *payload, size_t size)
{
	size_t offset = 16;
	unsigned seen = 0;

	if (!payload || size < 16 || memcmp(payload, "FCS", 3) != 0 ||
	    state_load_le32(payload + 4) != size - 16)
		return false;
	while (offset < size) {
		uint8_t tag;
		uint32_t chunk_size;
		size_t end;
		unsigned bit;

		if (size - offset < 5)
			return false;
		tag = payload[offset];
		chunk_size = state_load_le32(payload + offset + 1);
		offset += 5;
		if ((uint64_t)chunk_size > (uint64_t)(size - offset))
			return false;
		end = offset + (size_t)chunk_size;
		switch (tag) {
		case 1: bit = 1u << 0; break;
		case 2: bit = 1u << 1; break;
		case 3: bit = 1u << 2; break;
		case 4: bit = 1u << 3; break;
		case 5: bit = 1u << 4; break;
		case 0x10: bit = 1u << 5; break;
		default: return false;
		}
		if (seen & bit)
			return false;
		seen |= bit;
		while (offset < end) {
			uint32_t field_size;

			if (end - offset < 8)
				return false;
			field_size = state_load_le32(payload + offset + 4);
			offset += 8;
			if ((uint64_t)field_size > (uint64_t)(end - offset))
				return false;
			offset += (size_t)field_size;
		}
		if (offset != end)
			return false;
	}
	return offset == size && seen == 0x3fu;
}

struct state_blob {
	uint8_t *payload;
	size_t payload_size;
	uint8_t *frame;
	size_t frame_size;
	unsigned frame_width;
	unsigned frame_height;
};

static bool state_bytes_zero(const uint8_t *bytes, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}

static int state_read_full(int fd, void *data, size_t size)
{
	uint8_t *bytes = data;
	size_t done = 0;

	while (done < size) {
		ssize_t got = read(fd, bytes + done, size - done);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			errno = got == 0 ? EIO : errno;
			return -1;
		}
		done += (size_t)got;
	}
	return 0;
}

static int state_read_file(struct nes_host *h, int dirfd, unsigned slot,
	struct state_blob *blob, struct nes_state_info *info, bool load_payload)
{
	uint8_t header[NES_STATE_HEADER_BYTES];
	uint8_t core_digest[NES_SHA256_BYTES];
	uint8_t payload_digest[NES_SHA256_BYTES];
	char name[128];
	struct stat st;
	uint64_t payload_size;
	uint64_t frame_size;
	uint64_t rom_size;
	uint64_t modified;
	uint32_t label_length;
	uint32_t frame_width;
	uint32_t frame_height;
	size_t i;
	int fd = -1;
	int saved_errno = 0;

	memset(blob, 0, sizeof(*blob));
	memset(info, 0, sizeof(*info));
	info->slot = slot;
	info->durable = true;
	if (state_make_name(h, slot, name, sizeof(name)) != 0)
		return -1;
	if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return -1;
	info->exists = true;
	if (st.st_size >= 0)
		info->size = (uint64_t)st.st_size;
	if (st.st_mtime > 0)
		info->modified = (uint64_t)st.st_mtime;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_nlink != 1 ||
	    st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) {
		errno = EPERM;
		return -1;
	}
	if ((uint64_t)st.st_size < NES_STATE_HEADER_BYTES + 16u ||
	    (uint64_t)st.st_size > NES_STATE_HEADER_BYTES +
		(uint64_t)NES_MAX_STATE_BYTES +
		(uint64_t)NES_STATE_MAX_FRAME_BYTES) {
		errno = EFBIG;
		return -1;
	}
	fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0) {
		saved_errno = errno ? errno : EIO;
		goto fail;
	}
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_nlink != 1 ||
	    st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) {
		saved_errno = EPERM;
		goto fail;
	}
	if (state_read_full(fd, header, sizeof(header)) != 0)
		goto fail;
	if (memcmp(header, state_magic, sizeof(state_magic)) != 0 ||
	    state_load_le32(header + 8) != NES_STATE_FORMAT_VERSION ||
	    state_load_le32(header + 12) != NES_STATE_HEADER_BYTES ||
	    state_load_le32(header + 16) != slot) {
		saved_errno = EBADMSG;
		goto fail;
	}
	label_length = state_load_le32(header + 20);
	modified = state_load_le64(header + 24);
	payload_size = state_load_le64(header + 32);
	rom_size = state_load_le64(header + 40);
	frame_width = state_load_le32(header + 244);
	frame_height = state_load_le32(header + 248);
	frame_size = state_load_le64(header + 256);
	if (label_length >= NES_STATE_LABEL_BYTES ||
	    payload_size < 16 || payload_size > NES_MAX_STATE_BYTES ||
	    frame_size > NES_STATE_MAX_FRAME_BYTES ||
	    payload_size + frame_size !=
		(uint64_t)st.st_size - NES_STATE_HEADER_BYTES ||
	    rom_size != h->priv->rom_size ||
	    memcmp(header + 48, h->priv->rom_digest, NES_SHA256_BYTES) != 0 ||
	    state_load_le32(header + 240) != h->priv->rom_region ||
	    ((frame_size == 0 && (frame_width != 0 || frame_height != 0)) ||
	     (frame_size != 0 &&
	      (frame_width == 0 || frame_width > NES_MAX_W ||
	       frame_height == 0 || frame_height > NES_MAX_H ||
	       frame_size != (uint64_t)frame_width * frame_height * 2u))) ||
	    !state_bytes_zero(header + 252, 4) ||
	    !state_bytes_zero(header + 264, 8) ||
	    !state_bytes_zero(header + 304, 16) ||
	    !state_label_valid_n((const char *)header + 144, label_length)) {
		saved_errno = EBADMSG;
		goto fail;
	}
	for (i = label_length; i < NES_STATE_LABEL_BYTES; i++) {
		if (header[144 + i] != 0) {
			saved_errno = EBADMSG;
			goto fail;
		}
	}
	state_core_digest(h, core_digest);
	if (memcmp(header + 80, core_digest, sizeof(core_digest)) != 0) {
		saved_errno = EPROTO;
		goto fail;
	}
	info->loadable = true;
	info->modified = modified;
	memcpy(info->label, header + 144, label_length);
	info->label[label_length] = '\0';
	if (!load_payload) {
		if (close(fd) != 0) {
			fd = -1;
			saved_errno = errno;
			goto fail;
		}
		return 0;
	}
	blob->payload = malloc((size_t)payload_size);
	if (!blob->payload) {
		saved_errno = ENOMEM;
		goto fail;
	}
	blob->payload_size = (size_t)payload_size;
	blob->frame_size = (size_t)frame_size;
	blob->frame_width = frame_width;
	blob->frame_height = frame_height;
	if (state_read_full(fd, blob->payload, blob->payload_size) != 0) {
		saved_errno = errno ? errno : EIO;
		goto fail;
	}
	if (blob->frame_size) {
		blob->frame = malloc(blob->frame_size);
		if (!blob->frame) {
			saved_errno = ENOMEM;
			goto fail;
		}
		if (state_read_full(fd, blob->frame, blob->frame_size) != 0) {
			saved_errno = errno ? errno : EIO;
			goto fail;
		}
	}
	if (close(fd) != 0) {
		fd = -1;
		saved_errno = errno;
		goto fail;
	}
	fd = -1;
	nes_sha256_digest(blob->payload, blob->payload_size, payload_digest);
	if (memcmp(header + 112, payload_digest, sizeof(payload_digest)) != 0 ||
	    !state_fcs_valid(blob->payload, blob->payload_size)) {
		saved_errno = EBADMSG;
		goto fail;
	}
	nes_sha256_digest(blob->frame, blob->frame_size, payload_digest);
	if (memcmp(header + 272, payload_digest, sizeof(payload_digest)) != 0) {
		saved_errno = EBADMSG;
		goto fail;
	}
	return 0;

fail:
	if (fd >= 0)
		close(fd);
	free(blob->payload);
	free(blob->frame);
	memset(blob, 0, sizeof(*blob));
	errno = saved_errno ? saved_errno : EIO;
	return -1;
}

static int worker_list_states(struct nes_host *h,
	struct nes_state_info *states, size_t count)
{
	int dirfd;
	unsigned slot;

	if (!states || count < NES_STATE_SLOT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	memset(states, 0, sizeof(*states) * count);
	for (slot = 1; slot <= NES_STATE_SLOT_COUNT; slot++)
		states[slot - 1].slot = slot;
	if (!atomic_load_explicit(&h->game_loaded, memory_order_acquire))
		return 0;
	dirfd = open_state_directory(h, false);
	if (dirfd < 0) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	if (state_cleanup_temps(h, dirfd) != 0) {
		int saved_errno = errno;
		close(dirfd);
		errno = saved_errno;
		return -1;
	}
	for (slot = 1; slot <= NES_STATE_SLOT_COUNT; slot++) {
		struct state_blob blob;
		struct nes_state_info info;

		if (state_read_file(h, dirfd, slot, &blob, &info, false) == 0) {
			free(blob.payload);
			free(blob.frame);
			states[slot - 1] = info;
		} else if (errno != ENOENT) {
			info.error = errno ? errno : EIO;
			states[slot - 1] = info;
		}
	}
	close(dirfd);
	return 0;
}

static void state_build_header(struct nes_host *h, uint8_t *header,
	unsigned slot, const char *label, uint64_t modified,
	const uint8_t *payload, size_t payload_size, const uint8_t *frame,
	size_t frame_size, unsigned frame_width, unsigned frame_height)
{
	uint8_t digest[NES_SHA256_BYTES];
	size_t label_length = strlen(label);

	memset(header, 0, NES_STATE_HEADER_BYTES);
	memcpy(header, state_magic, sizeof(state_magic));
	state_store_le32(header + 8, NES_STATE_FORMAT_VERSION);
	state_store_le32(header + 12, NES_STATE_HEADER_BYTES);
	state_store_le32(header + 16, slot);
	state_store_le32(header + 20, (uint32_t)label_length);
	state_store_le64(header + 24, modified);
	state_store_le64(header + 32, payload_size);
	state_store_le64(header + 40, h->priv->rom_size);
	memcpy(header + 48, h->priv->rom_digest, NES_SHA256_BYTES);
	state_core_digest(h, digest);
	memcpy(header + 80, digest, sizeof(digest));
	nes_sha256_digest(payload, payload_size, digest);
	memcpy(header + 112, digest, sizeof(digest));
	memcpy(header + 144, label, label_length);
	state_store_le32(header + 240, h->priv->rom_region);
	state_store_le32(header + 244, frame_width);
	state_store_le32(header + 248, frame_height);
	state_store_le64(header + 256, frame_size);
	nes_sha256_digest(frame, frame_size, digest);
	memcpy(header + 272, digest, sizeof(digest));
}

static int worker_save_state(struct nes_host *h, unsigned slot,
	const char *label, struct nes_state_info *state)
{
	uint8_t *container = NULL;
	uint8_t *payload;
	uint8_t *frame;
	size_t payload_size;
	size_t frame_size = 0;
	size_t total_size;
	unsigned frame_width = 0;
	unsigned frame_height = 0;
	size_t i;
	uint64_t modified;
	char name[128];
	bool durable;
	int dirfd = -1;
	int result = -1;
	int saved_errno = 0;

	if (state)
		memset(state, 0, sizeof(*state));
	if (!label)
		label = "";
	if (state_make_name(h, slot, name, sizeof(name)) != 0 ||
	    !state_label_valid(label)) {
		if (errno == 0)
			errno = EINVAL;
		return -1;
	}
	if (!h->core.retro_serialize_size || !h->core.retro_serialize) {
		errno = ENOTSUP;
		return -1;
	}
	if (worker_flush_sram(h, true, true) != 0)
		return -1;
	payload_size = h->core.retro_serialize_size();
	if (payload_size < 16 || payload_size > NES_MAX_STATE_BYTES ||
	    payload_size > SIZE_MAX - NES_STATE_HEADER_BYTES -
		NES_STATE_MAX_FRAME_BYTES) {
		errno = payload_size > NES_MAX_STATE_BYTES ? EFBIG : ENOTSUP;
		return -1;
	}
	pthread_mutex_lock(&h->frame_mu);
	if (h->frame_id > 0 && h->frame_w > 0 && h->frame_w <= NES_MAX_W &&
	    h->frame_h > 0 && h->frame_h <= NES_MAX_H) {
		frame_width = h->frame_w;
		frame_height = h->frame_h;
		frame_size = (size_t)frame_width * frame_height * 2u;
	}
	pthread_mutex_unlock(&h->frame_mu);
	total_size = NES_STATE_HEADER_BYTES + payload_size + frame_size;
	container = malloc(total_size);
	if (!container)
		return -1;
	payload = container + NES_STATE_HEADER_BYTES;
	frame = payload + payload_size;
	if (!h->core.retro_serialize(payload, payload_size) ||
	    !state_fcs_valid(payload, payload_size)) {
		saved_errno = EIO;
		goto done;
	}
	if (frame_size) {
		pthread_mutex_lock(&h->frame_mu);
		for (i = 0; i < frame_size / 2u; i++)
			state_store_le16(frame + i * 2u, h->frame_rgb565[i]);
		pthread_mutex_unlock(&h->frame_mu);
	}
	{
		time_t now = time(NULL);

		modified = now > 0 ? (uint64_t)now : 0;
	}
	state_build_header(h, container, slot, label, modified,
		payload, payload_size, frame, frame_size,
		frame_width, frame_height);
	dirfd = open_state_directory(h, true);
	if (dirfd < 0)
		goto done_errno;
	if (state_cleanup_temps(h, dirfd) != 0 ||
	    state_check_capacity(dirfd, name, total_size) != 0)
		goto done_errno;
	if (state_atomic_write(h, dirfd, name, container, total_size,
		&durable) != 0)
		goto done_errno;
	if (state) {
		state->slot = slot;
		state->exists = true;
		state->loadable = true;
		state->durable = durable;
		state->modified = modified;
		state->size = total_size;
		snprintf(state->label, sizeof(state->label), "%s", label);
	}
	result = 0;
	goto done;

done_errno:
	saved_errno = errno ? errno : EIO;
done:
	if (dirfd >= 0)
		close(dirfd);
	free(container);
	if (result != 0)
		errno = saved_errno ? saved_errno : EIO;
	return result;
}

static void state_clear_inputs(struct nes_host *h)
{
	atomic_store_explicit(&h->joy[0], 0, memory_order_relaxed);
	atomic_store_explicit(&h->joy[1], 0, memory_order_relaxed);
}

static bool state_restore_backup(struct nes_host *h, const uint8_t *backup,
	size_t backup_size)
{
	return h->core.retro_unserialize(backup, backup_size) &&
		worker_flush_sram(h, true, true) == 0;
}

static void state_apply_frame(struct nes_host *h, const struct state_blob *blob)
{
	size_t pixels;
	size_t i;

	pthread_mutex_lock(&h->frame_mu);
	memset(h->frame_rgb565, 0, sizeof(h->frame_rgb565));
	if (blob->frame_size) {
		pixels = blob->frame_size / 2u;
		for (i = 0; i < pixels; i++)
			h->frame_rgb565[i] = state_load_le16(blob->frame + i * 2u);
		h->frame_w = blob->frame_width;
		h->frame_h = blob->frame_height;
		h->pitch = (size_t)blob->frame_width * 2u;
	} else {
		h->frame_w = 0;
		h->frame_h = 0;
		h->pitch = 0;
	}
	h->frame_id++;
	if (h->frame_id == 0)
		h->frame_id = 1;
	pthread_mutex_unlock(&h->frame_mu);
}

static int worker_load_state(struct nes_host *h, unsigned slot)
{
	struct state_blob candidate;
	struct nes_state_info info;
	uint8_t *backup = NULL;
	size_t backup_size;
	int dirfd = -1;
	int saved_errno = 0;
	bool restored;
	char name[128];

	if (state_make_name(h, slot, name, sizeof(name)) != 0)
		return -1;
	if (!h->core.retro_serialize_size || !h->core.retro_serialize ||
	    !h->core.retro_unserialize) {
		errno = ENOTSUP;
		return -1;
	}
	dirfd = open_state_directory(h, false);
	if (dirfd < 0)
		return -1;
	if (state_cleanup_temps(h, dirfd) != 0 ||
	    state_read_file(h, dirfd, slot, &candidate, &info, true) != 0) {
		saved_errno = errno ? errno : EIO;
		close(dirfd);
		errno = saved_errno;
		return -1;
	}
	close(dirfd);
	backup_size = h->core.retro_serialize_size();
	if (backup_size < 16 || backup_size > NES_MAX_STATE_BYTES ||
	    candidate.payload_size > backup_size * 4u) {
		free(candidate.payload);
		free(candidate.frame);
		errno = EPROTO;
		return -1;
	}
	backup = malloc(backup_size);
	if (!backup) {
		free(candidate.payload);
		free(candidate.frame);
		return -1;
	}
	if (!h->core.retro_serialize(backup, backup_size) ||
	    !state_fcs_valid(backup, backup_size)) {
		free(backup);
		free(candidate.payload);
		free(candidate.frame);
		errno = EIO;
		return -1;
	}
	if (worker_flush_sram(h, true, true) != 0) {
		saved_errno = errno ? errno : EIO;
		free(backup);
		free(candidate.payload);
		free(candidate.frame);
		errno = saved_errno;
		return -1;
	}
	if (!h->core.retro_unserialize(candidate.payload,
		candidate.payload_size)) {
		restored = state_restore_backup(h, backup, backup_size);
		clear_audio(h);
		state_clear_inputs(h);
		free(backup);
		free(candidate.payload);
		free(candidate.frame);
		if (!restored) {
			atomic_store_explicit(&h->running, false,
				memory_order_release);
			atomic_store_explicit(&h->paused, true,
				memory_order_release);
			errno = ENOTRECOVERABLE;
		} else {
			errno = EBADMSG;
		}
		return -1;
	}
	clear_audio(h);
	state_clear_inputs(h);
	if (worker_flush_sram(h, true, true) != 0) {
		saved_errno = errno ? errno : EIO;
		restored = state_restore_backup(h, backup, backup_size);
		clear_audio(h);
		state_clear_inputs(h);
		free(backup);
		free(candidate.payload);
		free(candidate.frame);
		if (!restored) {
			atomic_store_explicit(&h->running, false,
				memory_order_release);
			atomic_store_explicit(&h->paused, true,
				memory_order_release);
			errno = ENOTRECOVERABLE;
		} else {
			errno = saved_errno;
		}
		return -1;
	}
	state_apply_frame(h, &candidate);
	free(backup);
	free(candidate.payload);
	free(candidate.frame);
	return 0;
}

static int worker_delete_state(struct nes_host *h, unsigned slot,
	bool *durable)
{
	char name[128];
	int dirfd;
	int saved_errno;

	if (durable)
		*durable = false;
	if (state_make_name(h, slot, name, sizeof(name)) != 0)
		return -1;
	dirfd = open_state_directory(h, false);
	if (dirfd < 0)
		return -1;
	if (state_cleanup_temps(h, dirfd) != 0) {
		saved_errno = errno;
		close(dirfd);
		errno = saved_errno;
		return -1;
	}
	if (unlinkat(dirfd, name, 0) != 0) {
		saved_errno = errno;
		close(dirfd);
		errno = saved_errno;
		return -1;
	}
	if (fsync(dirfd) == 0 || errno == EINVAL || errno == EOPNOTSUPP) {
		if (durable)
			*durable = true;
	}
	close(dirfd);
	return 0;
}

static int stop_sram_thread(struct nes_host *h)
{
	struct nes_host_private *p = h->priv;
	int error;

	if (!p->sram_thread_started)
		return 0;
	pthread_mutex_lock(&p->sram_mu);
	p->sram_shutdown = true;
	pthread_cond_broadcast(&p->sram_cv);
	pthread_mutex_unlock(&p->sram_mu);
	error = pthread_join(p->sram_thread, NULL);
	p->sram_thread_started = false;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

static int worker_load_game(struct nes_host *h, const char *rom_path)
{
	struct retro_game_info game;
	struct retro_system_av_info av;
	void *data = NULL;
	size_t size = 0;
	uint8_t rom_digest[NES_SHA256_BYTES];
	unsigned rom_region;

	if (!rom_path || !rom_path[0]) {
		errno = EINVAL;
		return -1;
	}
	if (strlen(rom_path) >= NES_CORE_CONTENT_PATH_BYTES) {
		/*
		 * FCEUmm uses the original path for region hints and auxiliary-file
		 * identity.  Refuse paths it cannot preserve exactly instead of
		 * silently truncating that metadata while loading the supplied bytes.
		 */
		errno = ENAMETOOLONG;
		return -1;
	}
	if (!atomic_load_explicit(&h->core_loaded, memory_order_acquire)) {
		if (worker_load_core(h, "builtin") != 0) {
			if (errno == 0)
				errno = ENODEV;
			return -1;
		}
	}

	if (read_rom_file(rom_path, &data, &size) != 0) {
		fprintf(stderr, "nesd: cannot read ROM %s: %s\n",
			rom_path, strerror(errno));
		return -1;
	}
	nes_sha256_digest(data, size, rom_digest);

	if (atomic_load_explicit(&h->game_loaded, memory_order_acquire) &&
	    worker_unload_game(h) != 0) {
		free(data);
		return -1;
	}

	memset(&game, 0, sizeof(game));
	game.path = rom_path;
	game.data = data;
	game.size = size;

	if (!h->core.retro_load_game(&game)) {
		fprintf(stderr, "nesd: retro_load_game failed: %s\n", rom_path);
		free(data);
		errno = EINVAL;
		return -1;
	}
	free(data);
	rom_region = h->core.retro_get_region();
	if (rom_region != RETRO_REGION_NTSC && rom_region != RETRO_REGION_PAL) {
		h->core.retro_unload_game();
		errno = EPROTO;
		return -1;
	}

	memset(&av, 0, sizeof(av));
	h->core.retro_get_system_av_info(&av);
	if (!apply_av_info(h, &av)) {
		h->core.retro_unload_game();
		fprintf(stderr, "nesd: rejected unsupported AV geometry for %s\n",
			rom_path);
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&h->priv->state_mu);
	snprintf(h->rom_path, sizeof(h->rom_path), "%s", rom_path);
	pthread_mutex_unlock(&h->priv->state_mu);
	atomic_store_explicit(&h->game_loaded, true, memory_order_release);
	atomic_store_explicit(&h->demo_mode, false, memory_order_release);
	memcpy(h->priv->rom_digest, rom_digest, sizeof(rom_digest));
	h->priv->rom_size = size;
	h->priv->rom_region = rom_region;
	clear_audio(h);
	clear_frame(h);
	reset_sram_tracking(h);
	make_sram_name(h, rom_path);
	if (worker_load_sram(h) != 0) {
		int saved_errno = errno ? errno : EIO;

		fprintf(stderr,
			"nesd: refusing to run without safely loaded SRAM\n");
		worker_discard_game(h);
		errno = saved_errno;
		return -1;
	}

	pthread_mutex_lock(&h->priv->state_mu);
	fprintf(stderr, "nesd: ROM loaded: %s (%ux%u @ %.4f, %.0f Hz)\n",
		rom_path, h->width, h->height, h->fps, h->sample_rate);
	pthread_mutex_unlock(&h->priv->state_mu);
	return 0;
}

static void worker_discard_game(struct nes_host *h)
{
	if (!atomic_load_explicit(&h->game_loaded, memory_order_acquire))
		return;
	if (atomic_load_explicit(&h->core_loaded, memory_order_acquire)) {
		if (h->core.retro_unload_game)
			h->core.retro_unload_game();
	}
	memset(h->priv->rom_digest, 0, sizeof(h->priv->rom_digest));
	h->priv->rom_size = 0;
	h->priv->rom_region = RETRO_REGION_NTSC;
	reset_sram_tracking(h);
	atomic_store_explicit(&h->game_loaded, false, memory_order_release);
	atomic_store_explicit(&h->demo_mode, false, memory_order_release);
	pthread_mutex_lock(&h->priv->state_mu);
	h->rom_path[0] = '\0';
	pthread_mutex_unlock(&h->priv->state_mu);
	clear_audio(h);
	clear_frame(h);
}

static int worker_unload_game(struct nes_host *h)
{
	if (!atomic_load_explicit(&h->game_loaded, memory_order_acquire))
		return 0;
	if (atomic_load_explicit(&h->core_loaded, memory_order_acquire) &&
	    worker_flush_sram(h, true, true) != 0)
		return -1;
	worker_discard_game(h);
	return 0;
}

static int process_command(struct nes_host *h, enum host_command command,
	const char *path, bool value, unsigned slot, const char *label,
	struct nes_state_info *states, size_t state_count)
{
	int result;
	int saved_errno;

	switch (command) {
	case HOST_CMD_LOAD_CORE:
		return worker_load_core(h, path);
	case HOST_CMD_UNLOAD_CORE:
		return worker_unload_core(h);
	case HOST_CMD_LOAD_GAME:
		return worker_load_game(h, path);
	case HOST_CMD_UNLOAD_GAME:
		return worker_unload_game(h);
	case HOST_CMD_START:
		if (!atomic_load_explicit(&h->game_loaded, memory_order_acquire))
			enable_demo_mode(h);
		else
			atomic_store_explicit(&h->demo_mode, false,
					      memory_order_release);
		atomic_store_explicit(&h->running, true, memory_order_release);
		return 0;
	case HOST_CMD_STOP:
		if (worker_flush_sram(h, false, true) != 0)
			return -1;
		atomic_store_explicit(&h->running, false, memory_order_release);
		clear_audio(h);
		return 0;
	case HOST_CMD_RESET:
		if (atomic_load_explicit(&h->core_loaded, memory_order_acquire) &&
		    atomic_load_explicit(&h->game_loaded, memory_order_acquire) &&
		    h->core.retro_reset) {
			if (worker_flush_sram(h, false, true) != 0)
				return -1;
			h->core.retro_reset();
			clear_audio(h);
		}
		return 0;
	case HOST_CMD_SET_PAUSED:
		if (value && worker_flush_sram(h, false, true) != 0)
			return -1;
		atomic_store_explicit(&h->paused, value, memory_order_release);
		return 0;
	case HOST_CMD_FLUSH_SRAM:
		return worker_flush_sram(h, true, true);
	case HOST_CMD_LIST_STATES:
		return worker_list_states(h, states, state_count);
	case HOST_CMD_SAVE_STATE:
		return worker_save_state(h, slot, label,
			states && state_count ? &states[0] : NULL);
	case HOST_CMD_LOAD_STATE:
		return worker_load_state(h, slot);
	case HOST_CMD_DELETE_STATE:
		return worker_delete_state(h, slot,
			states && state_count ? &states[0].durable : NULL);
	case HOST_CMD_SHUTDOWN:
		atomic_store_explicit(&h->running, false, memory_order_release);
		result = worker_unload_core(h);
		if (result != 0) {
			saved_errno = errno ? errno : EIO;
			fprintf(stderr,
				"nesd: SRAM save failed during shutdown; "
				"discarding the core and exiting with an error\n");
			worker_discard_core(h);
			errno = saved_errno;
		}
		return result;
	case HOST_CMD_NONE:
	default:
		errno = EINVAL;
		return -1;
	}
}

static void *emu_thread_fn(void *argument)
{
	struct nes_host *h = argument;
	struct nes_host_private *p = h->priv;
	uint64_t next_frame_ns = 0;
	bool shutdown = false;

	while (!shutdown) {
		enum host_command command = HOST_CMD_NONE;
		char path[sizeof(p->command_path)];
		char label[sizeof(p->command_label)];
		bool value = false;
		unsigned slot = 0;
		struct nes_state_info *states = NULL;
		size_t state_count = 0;
		int result;
		uint64_t now;
		double fps;

		pthread_mutex_lock(&p->control_mu);
		if (p->command != HOST_CMD_NONE) {
			command = p->command;
			value = p->command_bool;
			slot = p->command_slot;
			states = p->command_states;
			state_count = p->command_state_count;
			snprintf(path, sizeof(path), "%s", p->command_path);
			snprintf(label, sizeof(label), "%s", p->command_label);
		}
		pthread_mutex_unlock(&p->control_mu);

		if (command != HOST_CMD_NONE) {
			errno = 0;
			result = process_command(h, command, path, value, slot,
				label, states, state_count);
			if (result != 0 && errno == 0)
				errno = EIO;
			shutdown = command == HOST_CMD_SHUTDOWN;
			pthread_mutex_lock(&p->control_mu);
			p->command_result = result;
			p->command_errno = result == 0 ? 0 : errno;
			p->command = HOST_CMD_NONE;
			p->command_done = true;
			pthread_cond_broadcast(&p->control_cv);
			pthread_mutex_unlock(&p->control_mu);
			next_frame_ns = 0;
			continue;
		}

		now = mono_ms();
		if (p->next_sram_flush_ms != 0 &&
		    now >= p->next_sram_flush_ms)
			(void)worker_flush_sram(h, false, false);

		if (!atomic_load_explicit(&h->running, memory_order_acquire) ||
		    atomic_load_explicit(&h->paused, memory_order_acquire)) {
			next_frame_ns = 0;
			worker_wait_ns(h, 100000000ull);
			continue;
		}
		if (atomic_load_explicit(&h->viewers, memory_order_acquire) <= 0) {
			clear_audio(h);
			next_frame_ns = 0;
			worker_wait_ns(h, 100000000ull);
			continue;
		}

		if (atomic_load_explicit(&h->demo_mode, memory_order_acquire)) {
			pthread_mutex_lock(&h->frame_mu);
			demo_fill_frame(h);
			pthread_mutex_unlock(&h->frame_mu);
			demo_fill_audio(h);
			worker_pace(h, &next_frame_ns, (double)DEMO_FPS);
			continue;
		}
		if (!atomic_load_explicit(&h->game_loaded, memory_order_acquire)) {
			next_frame_ns = 0;
			worker_wait_ns(h, 100000000ull);
			continue;
		}

		h->core.retro_run();
		pthread_mutex_lock(&p->state_mu);
		fps = h->fps;
		pthread_mutex_unlock(&p->state_mu);
		/* Run at the core's native rate; stream FPS is limited separately. */
		worker_pace(h, &next_frame_ns, fps);
	}

	pthread_mutex_lock(&p->control_mu);
	atomic_store_explicit(&h->thread_alive, false, memory_order_release);
	pthread_cond_broadcast(&p->control_cv);
	pthread_mutex_unlock(&p->control_mu);
	return NULL;
}

static int submit_command_full(struct nes_host *h, enum host_command command,
	const char *path, bool value, unsigned slot, const char *label,
	struct nes_state_info *states, size_t state_count)
{
	struct nes_host_private *p;
	int result;
	int saved_errno;

	if (!h || !(p = h->priv) ||
	    !atomic_load_explicit(&h->initialized, memory_order_acquire)) {
		errno = EINVAL;
		return -1;
	}
	if (path && strlen(path) >= sizeof(p->command_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (label && strlen(label) >= sizeof(p->command_label)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* One synchronous submitter owns the single command slot at a time. */
	pthread_mutex_lock(&p->submit_mu);
	if (p->shutdown_requested ||
	    !atomic_load_explicit(&h->thread_alive, memory_order_acquire)) {
		pthread_mutex_unlock(&p->submit_mu);
		errno = ESHUTDOWN;
		return -1;
	}
	pthread_mutex_lock(&p->control_mu);
	if (command == HOST_CMD_SHUTDOWN)
		p->shutdown_requested = true;
	p->command = command;
	p->command_bool = value;
	p->command_slot = slot;
	p->command_states = states;
	p->command_state_count = state_count;
	p->command_result = -1;
	p->command_errno = 0;
	p->command_done = false;
	snprintf(p->command_path, sizeof(p->command_path), "%s",
		 path ? path : "");
	snprintf(p->command_label, sizeof(p->command_label), "%s",
		 label ? label : "");
	pthread_cond_broadcast(&p->control_cv);
	while (!p->command_done &&
	       atomic_load_explicit(&h->thread_alive, memory_order_acquire))
		pthread_cond_wait(&p->control_cv, &p->control_mu);
	result = p->command_done ? p->command_result : -1;
	saved_errno = p->command_done ? p->command_errno : EIO;
	p->command_states = NULL;
	p->command_state_count = 0;
	pthread_mutex_unlock(&p->control_mu);
	pthread_mutex_unlock(&p->submit_mu);
	if (result != 0)
		errno = saved_errno ? saved_errno : EIO;
	return result;
}

static int submit_command(struct nes_host *h, enum host_command command,
	const char *path, bool value)
{
	return submit_command_full(h, command, path, value, 0, NULL, NULL, 0);
}

int host_init(struct nes_host *h, const char *core_path,
	const char *system_dir, const char *save_dir)
{
	struct nes_host_private *p;
	struct nes_host *expected = NULL;
	pthread_condattr_t cond_attr;
	pthread_attr_t thread_attr;
	bool cond_attr_ready = false;
	bool thread_attr_ready = false;
	bool cond_ready = false;
	bool global_claimed = false;
	bool frame_ready = false, audio_ready = false;
	bool state_ready = false, control_ready = false, submit_ready = false;
	bool sram_mu_ready = false, sram_cv_ready = false;
	int error;
	int saved_errno = 0;

	if (!h ||
	    (system_dir && strlen(system_dir) >= sizeof(h->system_dir)) ||
	    (save_dir && strlen(save_dir) >= sizeof(h->save_dir)) ||
	    (core_path && strlen(core_path) >= sizeof(h->core_path))) {
		errno = EINVAL;
		return -1;
	}

	memset(h, 0, sizeof(*h));
	atomic_init(&h->core_loaded, false);
	atomic_init(&h->game_loaded, false);
	atomic_init(&h->running, false);
	atomic_init(&h->paused, false);
	atomic_init(&h->demo_mode, false);
	atomic_init(&h->thread_alive, false);
	atomic_init(&h->initialized, false);
	atomic_init(&h->pixel_fmt, RETRO_PIXEL_FORMAT_0RGB1555);
	atomic_init(&h->viewers, 0);
	atomic_init(&h->joy[0], 0);
	atomic_init(&h->joy[1], 0);

	p = calloc(1, sizeof(*p));
	if (!p)
		return -1;
	h->priv = p;

	error = pthread_mutex_init(&h->frame_mu, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	frame_ready = true;
	error = pthread_mutex_init(&h->audio_mu, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	audio_ready = true;
	error = pthread_mutex_init(&p->state_mu, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	state_ready = true;
	error = pthread_mutex_init(&p->control_mu, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	control_ready = true;
	error = pthread_mutex_init(&p->submit_mu, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	submit_ready = true;
	error = pthread_mutex_init(&p->sram_mu, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	sram_mu_ready = true;
	error = pthread_cond_init(&p->sram_cv, NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	sram_cv_ready = true;

	p->cond_clock = CLOCK_REALTIME;
	if (pthread_condattr_init(&cond_attr) == 0) {
		cond_attr_ready = true;
		if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0)
			p->cond_clock = CLOCK_MONOTONIC;
	}
	error = pthread_cond_init(&p->control_cv,
		cond_attr_ready ? &cond_attr : NULL);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	cond_ready = true;
	if (cond_attr_ready) {
		(void)pthread_condattr_destroy(&cond_attr);
		cond_attr_ready = false;
	}

	snprintf(h->system_dir, sizeof(h->system_dir), "%s",
		 system_dir ? system_dir : "/etc/nes-emulator/system");
	snprintf(h->save_dir, sizeof(h->save_dir), "%s",
		 save_dir ? save_dir : "/etc/nes-emulator/saves");
	set_idle_identity(h);

	error = pthread_attr_init(&thread_attr);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	thread_attr_ready = true;
	error = pthread_attr_setstacksize(&thread_attr,
		NES_SRAM_THREAD_STACK_BYTES);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	error = pthread_create(&p->sram_thread, &thread_attr,
			       sram_thread_fn, h);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	p->sram_thread_started = true;
	(void)pthread_attr_destroy(&thread_attr);
	thread_attr_ready = false;

	if (!atomic_compare_exchange_strong_explicit(
		    &g_host, &expected, h, memory_order_acq_rel,
		    memory_order_acquire)) {
		saved_errno = EBUSY;
		goto fail;
	}
	global_claimed = true;
	error = pthread_attr_init(&thread_attr);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	thread_attr_ready = true;
	error = pthread_attr_setstacksize(&thread_attr,
		NES_EMU_THREAD_STACK_BYTES);
	if (error != 0) {
		saved_errno = error;
		goto fail;
	}
	atomic_store_explicit(&h->thread_alive, true, memory_order_release);
	error = pthread_create(&h->emu_thread, &thread_attr, emu_thread_fn, h);
	if (error != 0) {
		saved_errno = error;
		atomic_store_explicit(&h->thread_alive, false,
				      memory_order_release);
		goto fail;
	}
	(void)pthread_attr_destroy(&thread_attr);
	thread_attr_ready = false;
	atomic_store_explicit(&h->initialized, true, memory_order_release);

	if (core_path && core_path[0] && host_load_core(h, core_path) != 0)
		fprintf(stderr,
			"nesd: core load failed; development demo remains available\n");
	return 0;

fail:
	if (global_claimed)
		atomic_store_explicit(&g_host, NULL, memory_order_release);
	atomic_store_explicit(&h->initialized, false, memory_order_release);
	atomic_store_explicit(&h->thread_alive, false, memory_order_release);
	if (thread_attr_ready)
		(void)pthread_attr_destroy(&thread_attr);
	if (p->sram_thread_started)
		(void)stop_sram_thread(h);
	if (cond_attr_ready)
		(void)pthread_condattr_destroy(&cond_attr);
	if (cond_ready)
		(void)pthread_cond_destroy(&p->control_cv);
	if (sram_cv_ready)
		(void)pthread_cond_destroy(&p->sram_cv);
	if (sram_mu_ready)
		(void)pthread_mutex_destroy(&p->sram_mu);
	if (submit_ready)
		(void)pthread_mutex_destroy(&p->submit_mu);
	if (control_ready)
		(void)pthread_mutex_destroy(&p->control_mu);
	if (state_ready)
		(void)pthread_mutex_destroy(&p->state_mu);
	if (audio_ready)
		(void)pthread_mutex_destroy(&h->audio_mu);
	if (frame_ready)
		(void)pthread_mutex_destroy(&h->frame_mu);
	free(p);
	h->priv = NULL;
	errno = saved_errno ? saved_errno : EIO;
	return -1;
}

int host_shutdown(struct nes_host *h)
{
	struct nes_host_private *p;
	struct nes_host *expected;
	int join_error;
	int sram_join_error;
	int result;
	int saved_errno;

	if (!h || !(p = h->priv) ||
	    !atomic_load_explicit(&h->initialized, memory_order_acquire))
		return 0;

	result = submit_command(h, HOST_CMD_SHUTDOWN, NULL, false);
	saved_errno = result == 0 ? 0 : errno;
	join_error = pthread_join(h->emu_thread, NULL);
	if (join_error != 0 && result == 0) {
		result = -1;
		saved_errno = join_error;
	}
	sram_join_error = stop_sram_thread(h);
	if (sram_join_error != 0 && result == 0) {
		result = -1;
		saved_errno = errno ? errno : EIO;
	}
	atomic_store_explicit(&h->initialized, false, memory_order_release);
	expected = h;
	(void)atomic_compare_exchange_strong_explicit(
		&g_host, &expected, NULL, memory_order_acq_rel,
		memory_order_acquire);

	reset_sram_tracking(h);
	free(p->sram_pending_data);
	pthread_cond_destroy(&p->sram_cv);
	pthread_mutex_destroy(&p->sram_mu);
	pthread_cond_destroy(&p->control_cv);
	pthread_mutex_destroy(&p->submit_mu);
	pthread_mutex_destroy(&p->control_mu);
	pthread_mutex_destroy(&p->state_mu);
	pthread_mutex_destroy(&h->audio_mu);
	pthread_mutex_destroy(&h->frame_mu);
	free(p);
	h->priv = NULL;
	if (result != 0)
		errno = saved_errno ? saved_errno : EIO;
	return result;
}

int host_load_core(struct nes_host *h, const char *core_path)
{
	return submit_command(h, HOST_CMD_LOAD_CORE,
			      core_path && core_path[0] ? core_path : "builtin",
			      false);
}

int host_unload_core(struct nes_host *h)
{
	return submit_command(h, HOST_CMD_UNLOAD_CORE, NULL, false);
}

int host_load_game(struct nes_host *h, const char *rom_path)
{
	if (!rom_path || !rom_path[0]) {
		errno = EINVAL;
		return -1;
	}
	if (strlen(rom_path) >= NES_CORE_CONTENT_PATH_BYTES) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return submit_command(h, HOST_CMD_LOAD_GAME, rom_path, false);
}

int host_unload_game(struct nes_host *h)
{
	return submit_command(h, HOST_CMD_UNLOAD_GAME, NULL, false);
}

int host_start(struct nes_host *h)
{
	return submit_command(h, HOST_CMD_START, NULL, false);
}

int host_stop(struct nes_host *h)
{
	return submit_command(h, HOST_CMD_STOP, NULL, false);
}

int host_reset(struct nes_host *h)
{
	return submit_command(h, HOST_CMD_RESET, NULL, false);
}

int host_set_paused(struct nes_host *h, bool paused)
{
	return submit_command(h, HOST_CMD_SET_PAUSED, NULL, paused);
}

int host_flush_sram(struct nes_host *h)
{
	return submit_command(h, HOST_CMD_FLUSH_SRAM, NULL, false);
}

int host_list_states(struct nes_host *h, struct nes_state_info *states,
	size_t count)
{
	if (!states || count < NES_STATE_SLOT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	return submit_command_full(h, HOST_CMD_LIST_STATES, NULL, false, 0,
		NULL, states, count);
}

int host_save_state(struct nes_host *h, unsigned slot, const char *label,
	struct nes_state_info *state)
{
	if (slot < 1 || slot > NES_STATE_SLOT_COUNT ||
	    !state_label_valid(label ? label : "")) {
		errno = EINVAL;
		return -1;
	}
	return submit_command_full(h, HOST_CMD_SAVE_STATE, NULL, false, slot,
		label ? label : "", state, state ? 1 : 0);
}

int host_load_state(struct nes_host *h, unsigned slot)
{
	if (slot < 1 || slot > NES_STATE_SLOT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	return submit_command_full(h, HOST_CMD_LOAD_STATE, NULL, false, slot,
		NULL, NULL, 0);
}

int host_delete_state(struct nes_host *h, unsigned slot, bool *durable)
{
	struct nes_state_info result;
	int status;

	if (slot < 1 || slot > NES_STATE_SLOT_COUNT) {
		errno = EINVAL;
		return -1;
	}
	memset(&result, 0, sizeof(result));
	status = submit_command_full(h, HOST_CMD_DELETE_STATE, NULL, false,
		slot, NULL, &result, 1);
	if (status == 0 && durable)
		*durable = result.durable;
	return status;
}

void host_set_button(struct nes_host *h, unsigned port, unsigned id,
	bool pressed)
{
	if (!h || port > 1 || id >= NES_JOY_BITS)
		return;
	if (pressed)
		(void)atomic_fetch_or_explicit(&h->joy[port], 1u << id,
			memory_order_relaxed);
	else
		(void)atomic_fetch_and_explicit(&h->joy[port], ~(1u << id),
			memory_order_relaxed);
}

void host_set_joy_mask(struct nes_host *h, unsigned port, uint16_t mask)
{
	if (!h || port > 1)
		return;
	atomic_store_explicit(&h->joy[port], mask, memory_order_relaxed);
}

void host_set_viewers(struct nes_host *h, int count)
{
	int previous;

	if (!h)
		return;
	if (count < 0)
		count = 0;
	previous = atomic_exchange_explicit(&h->viewers, count,
					    memory_order_acq_rel);
	if (previous != count)
		wake_worker(h);
}

bool host_is_running(const struct nes_host *h)
{
	return h && atomic_load_explicit(&h->running, memory_order_acquire);
}

bool host_is_paused(const struct nes_host *h)
{
	return h && atomic_load_explicit(&h->paused, memory_order_acquire);
}

void host_get_status(struct nes_host *h, struct nes_host_status *status)
{
	struct nes_host_private *p;

	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	if (!h || !(p = h->priv))
		return;

	pthread_mutex_lock(&p->submit_mu);
	status->core_loaded =
		atomic_load_explicit(&h->core_loaded, memory_order_acquire);
	status->game_loaded =
		atomic_load_explicit(&h->game_loaded, memory_order_acquire);
	status->running =
		atomic_load_explicit(&h->running, memory_order_acquire);
	status->paused =
		atomic_load_explicit(&h->paused, memory_order_acquire);
	status->demo_mode =
		atomic_load_explicit(&h->demo_mode, memory_order_acquire);
	status->viewers =
		atomic_load_explicit(&h->viewers, memory_order_acquire);

	pthread_mutex_lock(&p->state_mu);
	snprintf(status->core_path, sizeof(status->core_path), "%s",
		 h->core_path);
	snprintf(status->rom_path, sizeof(status->rom_path), "%s",
		 h->rom_path);
	snprintf(status->library_name, sizeof(status->library_name), "%s",
		 h->library_name);
	snprintf(status->library_version, sizeof(status->library_version), "%s",
		 h->library_version);
	status->width = h->width;
	status->height = h->height;
	status->fps = h->fps;
	status->sample_rate = h->sample_rate;
	pthread_mutex_unlock(&p->state_mu);
	pthread_mutex_lock(&h->frame_mu);
	status->frame_id = h->frame_id;
	if (h->frame_id > 0 && h->frame_w > 0 && h->frame_h > 0) {
		status->width = h->frame_w;
		status->height = h->frame_h;
	}
	pthread_mutex_unlock(&h->frame_mu);
	pthread_mutex_unlock(&p->submit_mu);
}

void host_apply_router_limits(void)
{
	int current_nice;
	int effective_nice;
	int oom_fd;
	int oom_adjusted = 0;
	static const char oom_score[] = "800\n";

	errno = 0;
	current_nice = getpriority(PRIO_PROCESS, 0);
	if (current_nice == -1 && errno != 0)
		current_nice = 0;
	if (setpriority(PRIO_PROCESS, 0, NES_ROUTER_NICE) != 0) {
		/*
		 * nice() is relative. Only use it when that moves towards the
		 * requested value; an older launcher may already have set nice=15,
		 * and blindly adding five would make starvation even worse.
		 */
		if (current_nice < NES_ROUTER_NICE) {
			errno = 0;
			if (nice(NES_ROUTER_NICE - current_nice) == -1 &&
			    errno != 0)
				fprintf(stderr,
					"nesd: cannot lower CPU priority: %s\n",
					strerror(errno));
		} else if (current_nice > NES_ROUTER_NICE) {
			fprintf(stderr,
				"nesd: launcher supplied nice=%d; cannot raise it "
				"to %d without privilege\n",
				current_nice, NES_ROUTER_NICE);
		}
	}
	errno = 0;
	effective_nice = getpriority(PRIO_PROCESS, 0);
	if (effective_nice == -1 && errno != 0)
		effective_nice = current_nice;

#if defined(__linux__)
	/*
	 * Never assume which CPU handles Wi-Fi/NAT interrupts: that differs
	 * between targets. Let the scheduler place us, but make nesd the first
	 * disposable process if the router enters genuine memory pressure.
	 */
	oom_fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
	if (oom_fd >= 0) {
		oom_adjusted =
			write(oom_fd, oom_score, sizeof(oom_score) - 1) ==
				(ssize_t)(sizeof(oom_score) - 1);
		close(oom_fd);
	}
#else
	oom_fd = -1;
	(void)oom_fd;
#endif
	fprintf(stderr,
		"nesd: nice=%d%s (router services retain scheduler priority)\n",
		effective_nice,
		oom_adjusted ? ", oom_score_adj=800" : "");
}

bool host_copy_frame(struct nes_host *h, uint16_t *dst, size_t dst_pixels,
	unsigned *width, unsigned *height, uint64_t *frame_id)
{
	size_t pixels;
	bool copied = false;

	if (!h || !dst)
		return false;
	pthread_mutex_lock(&h->frame_mu);
	if (h->frame_id > 0 && h->frame_w > 0 && h->frame_h > 0) {
		pixels = (size_t)h->frame_w * h->frame_h;
		if (pixels <= dst_pixels) {
			memcpy(dst, h->frame_rgb565,
			       pixels * sizeof(*h->frame_rgb565));
			if (width)
				*width = h->frame_w;
			if (height)
				*height = h->frame_h;
			if (frame_id)
				*frame_id = h->frame_id;
			copied = true;
		}
	}
	pthread_mutex_unlock(&h->frame_mu);
	return copied;
}

size_t host_copy_audio(struct nes_host *h, int16_t *dst, size_t max_frames,
	unsigned *sample_rate, unsigned *channels)
{
	size_t count, first, recent_frames;
	double rate;

	if (!h || !dst || max_frames == 0)
		return 0;

	/*
	 * Hold state_mu through the audio snapshot so a dynamic sample-rate change
	 * cannot label old queued PCM with the new rate. apply_av_info() takes the
	 * same state_mu -> audio_mu order when it changes and clears the stream.
	 */
	pthread_mutex_lock(&h->priv->state_mu);
	rate = h->sample_rate;
	if (!(rate >= 1000.0 && rate <= 384000.0))
		rate = 48000.0;
	recent_frames = (size_t)(rate * (double)NES_AUDIO_RECENT_MS / 1000.0 +
		0.5);
	if (recent_frames == 0 || recent_frames > NES_AUDIO_RING_FRAMES)
		recent_frames = NES_AUDIO_RING_FRAMES;

	pthread_mutex_lock(&h->audio_mu);
	/*
	 * A blocked network/event loop must not replay up to a second of stale
	 * sound when it resumes. Keep only a short, recent window; normal pulls
	 * happen well below this threshold and therefore remain lossless.
	 */
	if (h->audio_count > recent_frames) {
		size_t stale = h->audio_count - recent_frames;

		h->audio_r = (h->audio_r + stale) % NES_AUDIO_RING_FRAMES;
		h->audio_count = recent_frames;
	}
	count = h->audio_count < max_frames ? h->audio_count : max_frames;
	first = NES_AUDIO_RING_FRAMES - h->audio_r;
	if (first > count)
		first = count;
	memcpy(dst, h->audio_ring + h->audio_r * 2,
	       first * 2 * sizeof(*dst));
	if (first < count) {
		memcpy(dst + first * 2, h->audio_ring,
		       (count - first) * 2 * sizeof(*dst));
	}
	h->audio_r = (h->audio_r + count) % NES_AUDIO_RING_FRAMES;
	h->audio_count -= count;
	pthread_mutex_unlock(&h->audio_mu);
	pthread_mutex_unlock(&h->priv->state_mu);

	if (sample_rate)
		*sample_rate = (unsigned)rate;
	if (channels)
		*channels = 2;
	return count;
}
