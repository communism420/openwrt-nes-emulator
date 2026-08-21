#define _GNU_SOURCE

/*
 * Keep this as a white-box contract: pull_latest_audio() and the bounded
 * per-client FIFO are deliberately private to the single-threaded transport
 * implementation. Including the source exercises the real retention and
 * head-of-line behavior without adding a production-only test API.
 */
#include "../package/nes-emulator/src/http.c"

#include <assert.h>

static unsigned mock_step;
static unsigned mock_mode;
static size_t mock_pending_frames;
static size_t mock_frame_cursor;

size_t host_copy_audio(struct nes_host *host, int16_t *dst, size_t max_frames,
	unsigned *sample_rate, unsigned *channels)
{
	size_t i;

	(void)host;
	assert(max_frames == AUDIO_PULL_FRAMES);
	if (mock_mode == 0 && mock_step++ == 0) {
		*sample_rate = 44100;
		*channels = 2;
		for (i = 0; i < max_frames * *channels; i++)
			dst[i] = (int16_t)(i + 1);
		return max_frames;
	}
	if (mock_mode == 1 && mock_step++ == 0) {
		*sample_rate = 44100;
		*channels = 2;
		memset(dst, 0x11, max_frames * *channels * sizeof(*dst));
		return max_frames;
	}
	if (mock_mode == 1 && mock_step == 2) {
		*sample_rate = 22050;
		*channels = 1;
		for (i = 0; i < max_frames; i++)
			dst[i] = (int16_t)(1000 + i);
		return max_frames;
	}
	if (mock_mode == 2 && mock_pending_frames) {
		size_t copied = mock_pending_frames < max_frames ?
			mock_pending_frames : max_frames;

		*sample_rate = 48000;
		*channels = 2;
		for (i = 0; i < copied; i++) {
			int16_t sample = (int16_t)(mock_frame_cursor + i);

			dst[i * 2] = sample;
			dst[i * 2 + 1] = sample;
		}
		mock_frame_cursor += copied;
		mock_pending_frames -= copied;
		return copied;
	}

	/* Simulate an AV-info update that clears the ring before the next pull. */
	*sample_rate = 48000;
	*channels = 2;
	return 0;
}

static void test_empty_pull_does_not_relabel_pcm(void)
{
	struct nes_http server;
	unsigned rate = 0;
	unsigned channels = 0;
	size_t frames;

	memset(&server, 0, sizeof(server));
	server.host = (struct nes_host *)(uintptr_t)1;
	mock_mode = 0;
	mock_step = 0;
	frames = pull_latest_audio(&server, &rate, &channels);
	assert(frames == AUDIO_PULL_FRAMES);
	assert(rate == 44100);
	assert(channels == 2);
	assert(audio_scratch[0] == 1);
	assert(audio_scratch[AUDIO_PULL_FRAMES * 2 - 1] ==
		(int16_t)(AUDIO_PULL_FRAMES * 2));
}

static void test_latest_full_chunk_owns_metadata(void)
{
	struct nes_http server;
	unsigned rate = 0;
	unsigned channels = 0;
	size_t frames;

	memset(&server, 0, sizeof(server));
	server.host = (struct nes_host *)(uintptr_t)1;
	mock_mode = 1;
	mock_step = 0;
	frames = pull_latest_audio(&server, &rate, &channels);
	assert(frames == AUDIO_PULL_FRAMES);
	assert(rate == 22050);
	assert(channels == 1);
	assert(audio_scratch[0] == 1000);
	assert(audio_scratch[AUDIO_PULL_FRAMES - 1] ==
		(int16_t)(1000 + AUDIO_PULL_FRAMES - 1));
}

static void test_normal_pacer_jitter_keeps_contiguous_pcm(void)
{
	struct nes_http server;
	unsigned rate = 0;
	unsigned channels = 0;
	size_t frames;
	size_t i;

	memset(&server, 0, sizeof(server));
	server.host = (struct nes_host *)(uintptr_t)1;
	mock_mode = 2;
	mock_step = 0;
	/* Three ~60 Hz core batches can land between adjacent 40 ms pulls. */
	mock_pending_frames = 2400;
	mock_frame_cursor = 0;
	frames = pull_latest_audio(&server, &rate, &channels);
	assert(frames == 2400);
	assert(rate == 48000);
	assert(channels == 2);
	assert(mock_pending_frames == 0);
	for (i = 0; i < frames; i++) {
		assert(audio_scratch[i * 2] == (int16_t)i);
		assert(audio_scratch[i * 2 + 1] == (int16_t)i);
	}
}

static void test_pal_sixty_ms_batch_keeps_contiguous_pcm(void)
{
	struct nes_http server;
	unsigned rate = 0;
	unsigned channels = 0;
	size_t frames;
	size_t i;

	memset(&server, 0, sizeof(server));
	server.host = (struct nes_host *)(uintptr_t)1;
	mock_mode = 2;
	mock_step = 0;
	/* Three 20 ms PAL core batches at 48 kHz must remain sample-contiguous. */
	mock_pending_frames = 2880;
	mock_frame_cursor = 0;
	frames = pull_latest_audio(&server, &rate, &channels);
	assert(frames == 2880);
	assert(rate == 48000);
	assert(channels == 2);
	assert(mock_pending_frames == 0);
	for (i = 0; i < frames; i++) {
		assert(audio_scratch[i * 2] == (int16_t)i);
		assert(audio_scratch[i * 2 + 1] == (int16_t)i);
	}
}

static void hold_partially_sent_video(struct client *client)
{
	uint8_t *frame = malloc(8);

	assert(frame != NULL);
	memset(client, 0, sizeof(*client));
	memset(frame, 0xa5, 8);
	websocket_activate(client, frame, 8, 0x2, WS_OUT_VIDEO);
	client->out_off = 1;
}

static int queue_audio_sequence(struct client *client, uint8_t sequence,
	uint64_t duration_us)
{
	return websocket_queue_audio(client, &sequence, sizeof(sequence),
		duration_us);
}

static uint8_t active_audio_sequence(const struct client *client)
{
	assert(client->out != NULL);
	assert(client->out_kind == WS_OUT_AUDIO);
	assert(client->out_len == 3);
	assert(client->out[0] == 0x82);
	assert(client->out[1] == 1);
	return client->out[2];
}

static uint8_t active_sequence(const struct client *client,
	enum websocket_output_kind kind)
{
	assert(client->out != NULL);
	assert(client->out_kind == kind);
	assert(client->out_len == 3);
	assert(client->out[1] == 1);
	return client->out[2];
}

static void promote_and_expect_audio(struct client *client, uint8_t sequence)
{
	websocket_promote_next(client, now_ms());
	assert(active_audio_sequence(client) == sequence);
	websocket_clear_active(client);
}

static void test_audio_fifo_preserves_order_behind_partial_video(void)
{
	struct client client;

	hold_partially_sent_video(&client);
	assert(queue_audio_sequence(&client, 1, 40000) == 0);
	assert(queue_audio_sequence(&client, 2, 40000) == 0);
	assert(queue_audio_sequence(&client, 3, 40000) == 0);
	assert(client.out_kind == WS_OUT_VIDEO);
	assert(client.out_off == 1);
	assert(client.audio_queue_count == 3);
	assert(client.audio_queue_duration_us == 120000);
	assert(client.dropped_packets == 0);

	websocket_clear_active(&client);
	promote_and_expect_audio(&client, 1);
	promote_and_expect_audio(&client, 2);
	promote_and_expect_audio(&client, 3);
	assert(client.audio_queue_count == 0);
	assert(client.audio_queue_duration_us == 0);
}

static void test_audio_fifo_drops_oldest_at_budget(void)
{
	struct client client;

	hold_partially_sent_video(&client);
	assert(queue_audio_sequence(&client, 1, 40000) == 0);
	assert(queue_audio_sequence(&client, 2, 40000) == 0);
	assert(queue_audio_sequence(&client, 3, 40000) == 0);
	assert(queue_audio_sequence(&client, 4, 40000) == 0);
	assert(client.audio_queue_count == 3);
	assert(client.audio_queue_duration_us == 120000);
	assert(client.dropped_packets == 1);

	websocket_clear_active(&client);
	promote_and_expect_audio(&client, 2);
	promote_and_expect_audio(&client, 3);
	promote_and_expect_audio(&client, 4);
	assert(client.audio_queue_count == 0);

	/* A maximum-size packet cannot make the bounded queue grow past 120 ms. */
	hold_partially_sent_video(&client);
	assert(queue_audio_sequence(&client, 5, 100000) == 0);
	assert(queue_audio_sequence(&client, 6, 40000) == 0);
	assert(client.audio_queue_count == 1);
	assert(client.audio_queue_duration_us == 40000);
	assert(client.dropped_packets == 1);
	websocket_clear_active(&client);
	promote_and_expect_audio(&client, 6);
}

static void test_audio_fifo_keeps_full_priority_order(void)
{
	struct client client;
	uint8_t video_sequence = 6;
	uint8_t status_sequence = 8;
	uint8_t control_sequence = 10;
	uint8_t *active;

	memset(&client, 0, sizeof(client));
	active = malloc(8);
	assert(active != NULL);
	memset(active, 0xa5, 8);
	websocket_activate(&client, active, 8, 0x2, WS_OUT_AUDIO);
	client.out_off = 1;
	assert(websocket_queue_video(&client, &video_sequence,
		sizeof(video_sequence)) == 0);
	assert(queue_audio_sequence(&client, 7, 40000) == 0);
	assert(websocket_queue(&client, 0x1, &status_sequence,
		sizeof(status_sequence), 1) == 0);
	assert(websocket_queue_heartbeat(&client, 9) == 0);
	assert(websocket_queue(&client, 0x0a, &control_sequence,
		sizeof(control_sequence), 1) == 0);

	websocket_clear_active(&client);
	websocket_promote_next(&client, now_ms());
	assert(active_sequence(&client, WS_OUT_CONTROL) == control_sequence);
	websocket_clear_active(&client);
	websocket_promote_next(&client, now_ms());
	assert(client.out != NULL);
	assert(client.out_kind == WS_OUT_HEARTBEAT);
	websocket_clear_active(&client);
	websocket_promote_next(&client, now_ms());
	assert(active_sequence(&client, WS_OUT_STATUS) == status_sequence);
	websocket_clear_active(&client);
	promote_and_expect_audio(&client, 7);
	websocket_promote_next(&client, now_ms());
	assert(active_sequence(&client, WS_OUT_VIDEO) == video_sequence);
	websocket_clear_active(&client);
}

static void test_audio_fifo_expires_only_stale_prefix(void)
{
	struct client client;
	unsigned second;
	unsigned third;
	uint64_t checked_at;

	hold_partially_sent_video(&client);
	assert(queue_audio_sequence(&client, 1, 40000) == 0);
	assert(queue_audio_sequence(&client, 2, 40000) == 0);
	assert(queue_audio_sequence(&client, 3, 40000) == 0);
	second = (client.audio_queue_head + 1u) % WS_AUDIO_QUEUE_SLOTS;
	third = (client.audio_queue_head + 2u) % WS_AUDIO_QUEUE_SLOTS;
	checked_at = now_ms();
	assert(checked_at > WS_AUDIO_QUEUE_MAX_AGE_MS);
	client.audio_queue[client.audio_queue_head].queued_ms =
		checked_at - WS_AUDIO_QUEUE_MAX_AGE_MS - 1u;
	client.audio_queue[second].queued_ms = checked_at;
	client.audio_queue[third].queued_ms = checked_at;
	websocket_expire_stale_media(&client, checked_at);
	assert(client.audio_queue_count == 2);
	assert(client.audio_queue_duration_us == 80000);
	assert(client.dropped_packets == 1);

	websocket_clear_active(&client);
	promote_and_expect_audio(&client, 2);
	promote_and_expect_audio(&client, 3);
}

static void test_websocket_output_age_never_wraps(void)
{
	struct client client;
	uint64_t sampled_now = WS_OUTPUT_MAX_AGE_MS + 10000u;

	memset(&client, 0, sizeof(client));
	client.out = malloc(1);
	assert(client.out != NULL);
	/* websocket_activate() may run just after the event loop sampled now. */
	client.out_progress_ms = sampled_now + 1u;
	client.out_since_ms = sampled_now + 1u;
	assert(!websocket_output_expired(&client, sampled_now));

	client.out_progress_ms = sampled_now - WS_OUTPUT_STALL_TIMEOUT_MS;
	assert(websocket_output_expired(&client, sampled_now));
	client.out_progress_ms = sampled_now + 1u;
	client.out_since_ms = sampled_now - WS_OUTPUT_MAX_AGE_MS;
	assert(websocket_output_expired(&client, sampled_now));
	free(client.out);
	client.out = NULL;
	assert(!websocket_output_expired(&client, sampled_now));
}

int main(void)
{
	test_empty_pull_does_not_relabel_pcm();
	test_latest_full_chunk_owns_metadata();
	test_normal_pacer_jitter_keeps_contiguous_pcm();
	test_pal_sixty_ms_batch_keeps_contiguous_pcm();
	test_audio_fifo_preserves_order_behind_partial_video();
	test_audio_fifo_drops_oldest_at_budget();
	test_audio_fifo_keeps_full_priority_order();
	test_audio_fifo_expires_only_stale_prefix();
	test_websocket_output_age_never_wraps();
	return 0;
}
