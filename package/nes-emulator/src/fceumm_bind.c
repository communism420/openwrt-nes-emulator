/*
 * Bind FCEUmm when it is statically linked into nesd (no dlopen).
 * Requires FCEUMM_BUILTIN and linking against FCEUmm object files.
 */
#include "host.h"
#include "libretro.h"

#include <stdio.h>
#include <string.h>

#ifdef FCEUMM_BUILTIN

/* Symbols exported by FCEUmm libretro driver */
void retro_init(void);
void retro_deinit(void);
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
void retro_get_system_av_info(struct retro_system_av_info *info);
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);
void retro_set_controller_port_device(unsigned port, unsigned device);
void retro_reset(void);
void retro_run(void);
size_t retro_serialize_size(void);
bool retro_serialize(void *data, size_t size);
bool retro_unserialize(const void *data, size_t size);
void retro_cheat_reset(void);
void retro_cheat_set(unsigned index, bool enabled, const char *code);
bool retro_load_game(const struct retro_game_info *game);
bool retro_load_game_special(unsigned game_type,
	const struct retro_game_info *info, size_t num_info);
void retro_unload_game(void);
unsigned retro_get_region(void);
void *retro_get_memory_data(unsigned id);
size_t retro_get_memory_size(unsigned id);

int host_bind_builtin_fceumm(struct nes_host *h)
{
	struct retro_core *c = &h->core;

	memset(c, 0, sizeof(*c));
	c->retro_init = retro_init;
	c->retro_deinit = retro_deinit;
	c->retro_api_version = retro_api_version;
	c->retro_get_system_info = retro_get_system_info;
	c->retro_get_system_av_info = retro_get_system_av_info;
	c->retro_set_environment = retro_set_environment;
	c->retro_set_video_refresh = retro_set_video_refresh;
	c->retro_set_audio_sample = retro_set_audio_sample;
	c->retro_set_audio_sample_batch = retro_set_audio_sample_batch;
	c->retro_set_input_poll = retro_set_input_poll;
	c->retro_set_input_state = retro_set_input_state;
	c->retro_set_controller_port_device = retro_set_controller_port_device;
	c->retro_reset = retro_reset;
	c->retro_run = retro_run;
	c->retro_serialize_size = retro_serialize_size;
	c->retro_serialize = retro_serialize;
	c->retro_unserialize = retro_unserialize;
	c->retro_cheat_reset = retro_cheat_reset;
	c->retro_cheat_set = retro_cheat_set;
	c->retro_load_game = retro_load_game;
	c->retro_load_game_special = retro_load_game_special;
	c->retro_unload_game = retro_unload_game;
	c->retro_get_region = retro_get_region;
	c->retro_get_memory_data = retro_get_memory_data;
	c->retro_get_memory_size = retro_get_memory_size;

	if (c->retro_api_version() != RETRO_API_VERSION) {
		fprintf(stderr, "builtin FCEUmm: unsupported API\n");
		return -1;
	}

	h->dl = NULL; /* not dynamically loaded; host.c publishes the path */
	return 0;
}

#else

int host_bind_builtin_fceumm(struct nes_host *h)
{
	(void)h;
	return -1;
}

#endif
