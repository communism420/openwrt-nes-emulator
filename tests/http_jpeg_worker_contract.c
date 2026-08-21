#define _GNU_SOURCE

/* White-box coverage for the private, bounded transport worker. */
#include "../package/nes-emulator/src/http.c"

#include <assert.h>
#include <sys/socket.h>

static pthread_mutex_t encoder_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t encoder_cv = PTHREAD_COND_INITIALIZER;
static unsigned encoder_calls;
static unsigned encoder_allowed;
static unsigned encoder_fail_call;
static uint16_t encoder_markers[16];
static unsigned failing_worker_init_calls;

static int failing_worker_init(struct nes_jpeg_worker *worker)
{
	(void)worker;
	failing_worker_init_calls++;
	errno = ENOMEM;
	return -1;
}

static struct timespec deadline_after_seconds(unsigned seconds)
{
	struct timespec deadline;

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += (time_t)seconds;
	return deadline;
}

static size_t controlled_encoder(const uint16_t *pixels, unsigned width,
	unsigned height, int quality, uint8_t *out, size_t out_cap)
{
	unsigned call;

	assert(pixels != NULL);
	assert(width == 2);
	assert(height == 2);
	assert(quality == 75);
	assert(out_cap >= 5);
	pthread_mutex_lock(&encoder_mu);
	call = ++encoder_calls;
	assert(call <= sizeof(encoder_markers) / sizeof(encoder_markers[0]));
	encoder_markers[call - 1] = pixels[0];
	pthread_cond_broadcast(&encoder_cv);
	while (encoder_allowed < call)
		assert(pthread_cond_wait(&encoder_cv, &encoder_mu) == 0);
	pthread_mutex_unlock(&encoder_mu);
	if (call == encoder_fail_call)
		return 0;
	out[0] = 0xff;
	out[1] = 0xd8;
	out[2] = (uint8_t)pixels[0];
	out[3] = 0xff;
	out[4] = 0xd9;
	return 5;
}

static int controlled_worker_init(struct nes_jpeg_worker *worker)
{
	return jpeg_worker_init_with_encoder(worker, controlled_encoder);
}

static void wait_for_encoder_calls(unsigned wanted)
{
	struct timespec deadline = deadline_after_seconds(3);

	pthread_mutex_lock(&encoder_mu);
	while (encoder_calls < wanted) {
		int error = pthread_cond_timedwait(&encoder_cv, &encoder_mu,
			&deadline);

		assert(error == 0);
	}
	pthread_mutex_unlock(&encoder_mu);
}

static void allow_encoder_call(unsigned call)
{
	pthread_mutex_lock(&encoder_mu);
	if (encoder_allowed < call)
		encoder_allowed = call;
	pthread_cond_broadcast(&encoder_cv);
	pthread_mutex_unlock(&encoder_mu);
}

static void wait_for_worker_completion(struct nes_jpeg_worker *worker)
{
	struct timespec deadline = deadline_after_seconds(3);

	pthread_mutex_lock(&worker->mu);
	while (!worker->completed_ready) {
		int error = pthread_cond_timedwait(&worker->cv, &worker->mu,
			&deadline);

		assert(error == 0);
	}
	pthread_mutex_unlock(&worker->mu);
}

static void wait_for_worker_idle(struct nes_jpeg_worker *worker)
{
	struct timespec deadline = deadline_after_seconds(3);

	pthread_mutex_lock(&worker->mu);
	while (worker->busy) {
		int error = pthread_cond_timedwait(&worker->cv, &worker->mu,
			&deadline);

		assert(error == 0);
	}
	pthread_mutex_unlock(&worker->mu);
}

static int bytes_contain(const uint8_t *bytes, size_t length,
	const char *needle)
{
	size_t needle_length = strlen(needle);
	size_t i;

	if (needle_length > length)
		return 0;
	for (i = 0; i <= length - needle_length; i++) {
		if (memcmp(bytes + i, needle, needle_length) == 0)
			return 1;
	}
	return 0;
}

static void assert_control_queues_remain_responsive(void)
{
	static const char status[] = "{\"t\":\"status\",\"ok\":true}";
	struct client client;
	uint8_t received[512];
	ssize_t length;
	int sockets[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	client_init(&client, sockets[0]);
	client.phase = CLIENT_WEBSOCKET;
	assert(websocket_queue_heartbeat(&client, 91) == 0);
	assert(websocket_queue(&client, 0x1, status, sizeof(status) - 1, 1) == 0);
	assert(websocket_flush(&client) == 0);
	length = recv(sockets[1], received, sizeof(received), 0);
	assert(length > 0);
	assert(bytes_contain(received, (size_t)length, "\"seq\":91"));
	assert(bytes_contain(received, (size_t)length, "\"t\":\"status\""));
	client_destroy(&client);
	close(sockets[1]);
}

static void fill_frame(uint16_t frame[4], uint16_t marker)
{
	unsigned i;

	for (i = 0; i < 4; i++)
		frame[i] = marker;
}

static void *destroy_worker_thread(void *opaque)
{
	jpeg_worker_destroy(opaque);
	return NULL;
}

int main(void)
{
	struct nes_jpeg_worker worker;
	struct nes_http server = { 0 };
	uint16_t frame[4];
	uint8_t packet[WS_RAW_PACKET_MAX];
	size_t packet_length = 0;
	uint64_t session;
	pthread_t destroy_thread;

	/* Optional JPEG resources must fail open instead of taking nesd down. */
	server.stream.use_jpeg = 1;
	server.jpeg_worker = (struct nes_jpeg_worker *)(uintptr_t)1;
	assert(jpeg_worker_enable_optional(&server, &worker,
		failing_worker_init) == 0);
	assert(failing_worker_init_calls == 1);
	assert(server.stream.use_jpeg == 0);
	assert(server.jpeg_worker == NULL);
	assert(jpeg_worker_enable_optional(&server, &worker,
		failing_worker_init) == 0);
	assert(failing_worker_init_calls == 1);

	server.stream.use_jpeg = 1;
	assert(jpeg_worker_enable_optional(&server, &worker,
		controlled_worker_init) == 1);
	assert(server.stream.use_jpeg == 1);
	assert(server.jpeg_worker == &worker);
	session = jpeg_worker_new_session(&worker);
	assert(session != 0);
	jpeg_worker_set_session(&worker, session);

	fill_frame(frame, 1);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 1) == 1);
	wait_for_encoder_calls(1);

	/* The event-loop-side queues must run while JPEG encoding is blocked. */
	assert_control_queues_remain_responsive();
	pthread_mutex_lock(&encoder_mu);
	assert(encoder_calls == 1);
	pthread_mutex_unlock(&encoder_mu);

	/* One active encode plus one latest pending frame: frame 2 is replaced. */
	fill_frame(frame, 2);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 2) == 1);
	fill_frame(frame, 3);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 3) == 1);
	pthread_mutex_lock(&worker.mu);
	assert(worker.pending_ready);
	assert(worker.pending_frame_id == 3);
	pthread_mutex_unlock(&worker.mu);

	allow_encoder_call(1);
	wait_for_worker_completion(&worker);
	assert(jpeg_worker_collect(&worker, packet, sizeof(packet),
		&packet_length) == 1);
	assert(packet[0] == NES_PKT_VIDEO_JPEG);
	assert(packet[10] == 1);
	assert(packet[12 + 2] == 1);

	wait_for_encoder_calls(2);
	pthread_mutex_lock(&encoder_mu);
	assert(encoder_markers[1] == 3);
	pthread_mutex_unlock(&encoder_mu);
	allow_encoder_call(2);
	wait_for_worker_completion(&worker);
	assert(jpeg_worker_collect(&worker, packet, sizeof(packet),
		&packet_length) == 1);
	assert(packet[10] == 3);
	assert(packet[12 + 2] == 3);

	/* Resetting state while an encode runs must make its result unobservable. */
	fill_frame(frame, 4);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 4) == 1);
	wait_for_encoder_calls(3);
	jpeg_worker_reset(&worker);
	allow_encoder_call(3);
	wait_for_worker_idle(&worker);
	assert(jpeg_worker_collect(&worker, packet, sizeof(packet),
		&packet_length) == 0);

	/* A JPEG failure is delivered as the same frame in bounded raw RGB565. */
	pthread_mutex_lock(&encoder_mu);
	encoder_fail_call = 4;
	pthread_mutex_unlock(&encoder_mu);
	fill_frame(frame, 5);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 5) == 1);
	wait_for_encoder_calls(4);
	allow_encoder_call(4);
	wait_for_worker_completion(&worker);
	assert(jpeg_worker_collect(&worker, packet, sizeof(packet),
		&packet_length) == 1);
	assert(packet_length == 20);
	assert(packet[0] == NES_PKT_VIDEO_RAW);
	assert(packet[6] == 5);
	assert(packet[12] == 5 && packet[13] == 0);

	/* A disconnect/reconnect generation also rejects the old in-flight job. */
	fill_frame(frame, 6);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 6) == 1);
	wait_for_encoder_calls(5);
	jpeg_worker_set_session(&worker, 0);
	jpeg_worker_set_session(&worker, jpeg_worker_new_session(&worker));
	allow_encoder_call(5);
	wait_for_worker_idle(&worker);
	assert(jpeg_worker_collect(&worker, packet, sizeof(packet),
		&packet_length) == 0);

	/* Shutdown requests join the encoder cleanly even during an active job. */
	fill_frame(frame, 7);
	assert(jpeg_worker_submit(&worker, frame, 2, 2, 75, 7) == 1);
	wait_for_encoder_calls(6);
	assert(pthread_create(&destroy_thread, NULL, destroy_worker_thread,
		&worker) == 0);
	pthread_mutex_lock(&worker.mu);
	while (!worker.stop) {
		struct timespec deadline = deadline_after_seconds(3);
		int error = pthread_cond_timedwait(&worker.cv, &worker.mu,
			&deadline);

		assert(error == 0);
	}
	pthread_mutex_unlock(&worker.mu);
	allow_encoder_call(6);
	assert(pthread_join(destroy_thread, NULL) == 0);
	assert(worker.wake_read_fd == -1);
	assert(worker.wake_write_fd == -1);
	assert(pthread_cond_destroy(&encoder_cv) == 0);
	assert(pthread_mutex_destroy(&encoder_mu) == 0);
	return 0;
}
