#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Include the implementation so the contract can exercise the producer and
 * consumer together without exporting test-only API from nesd. Unused host
 * sections are discarded by the focused test's linker flags.
 */
#include "../package/nes-emulator/src/host.c"

static int16_t sample_for(size_t frame, unsigned channel)
{
	return (int16_t)((frame * 37u + channel * 11u) & 0x7fffu);
}

static void fill_samples(int16_t *samples, size_t first_frame, size_t frames)
{
	size_t i;

	for (i = 0; i < frames; i++) {
		samples[i * 2] = sample_for(first_frame + i, 0);
		samples[i * 2 + 1] = sample_for(first_frame + i, 1);
	}
}

static void expect_samples(const int16_t *samples, size_t first_frame,
	size_t frames)
{
	size_t i;

	for (i = 0; i < frames; i++) {
		assert(samples[i * 2] == sample_for(first_frame + i, 0));
		assert(samples[i * 2 + 1] == sample_for(first_frame + i, 1));
	}
}

int main(void)
{
	struct nes_host host;
	struct nes_host_private private;
	int16_t normal[1000 * 2];
	int16_t backlog[10000 * 2];
	int16_t output[2048 * 2];
	unsigned rate = 0, channels = 0;
	size_t copied;

	memset(&host, 0, sizeof(host));
	memset(&private, 0, sizeof(private));
	host.priv = &private;
	host.sample_rate = 48000.0;
	atomic_init(&host.joy[0], 0);
	atomic_init(&host.joy[1], 0);
	assert(pthread_mutex_init(&host.audio_mu, NULL) == 0);
	assert(pthread_mutex_init(&private.state_mu, NULL) == 0);

	/* Normal cadence is lossless and begins with the oldest queued sample. */
	fill_samples(normal, 0, 1000);
	audio_push_frames(&host, normal, 1000);
	copied = host_copy_audio(&host, output, 2048, &rate, &channels);
	assert(copied == 1000);
	assert(rate == 48000);
	assert(channels == 2);
	expect_samples(output, 0, copied);

	/*
	 * A 208 ms backlog is trimmed to the newest 100 ms (4800 frames) before
	 * the first packet is copied, rather than replaying stale samples.
	 */
	fill_samples(backlog, 0, 10000);
	audio_push_frames(&host, backlog, 10000);
	copied = host_copy_audio(&host, output, 2048, &rate, &channels);
	assert(copied == 2048);
	expect_samples(output, 10000 - 4800, copied);
	assert(host.audio_count == 4800 - copied);

	/* The retained window remains ordered for the next pull. */
	copied = host_copy_audio(&host, output, 2048, &rate, &channels);
	assert(copied == 2048);
	expect_samples(output, 10000 - 4800 + 2048, copied);

	/* Input updates remain composable without a mutex in the core callback. */
	atomic_store_explicit(&g_host, &host, memory_order_release);
	host_set_button(&host, 0, RETRO_DEVICE_ID_JOYPAD_A, true);
	host_set_button(&host, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, true);
	assert(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
		RETRO_DEVICE_ID_JOYPAD_A) == 1);
	assert(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
		RETRO_DEVICE_ID_JOYPAD_RIGHT) == 1);
	host_set_button(&host, 0, RETRO_DEVICE_ID_JOYPAD_A, false);
	assert(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
		RETRO_DEVICE_ID_JOYPAD_A) == 0);
	host_set_joy_mask(&host, 0, (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_START));
	assert(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
		RETRO_DEVICE_ID_JOYPAD_RIGHT) == 0);
	assert(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
		RETRO_DEVICE_ID_JOYPAD_START) == 1);
	atomic_store_explicit(&g_host, NULL, memory_order_release);

	assert(pthread_mutex_destroy(&private.state_mu) == 0);
	assert(pthread_mutex_destroy(&host.audio_mu) == 0);
	puts("host audio/input contract: OK");
	return 0;
}
