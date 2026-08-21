#define _GNU_SOURCE
#include "http.h"
#include "jpeg_soft.h"
#include "play.html.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
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

#define MAX_CLIENTS 8
#define MAX_STREAM_CLIENTS 1
#define MAX_ACTIVE_UPLOADS 1
#define ACCEPT_BUDGET 8
#define HTTP_HEADER_MAX (16u * 1024u)
#define HTTP_BODY_MAX (64u * 1024u)
#define MAX_UPLOAD_DEFAULT NES_MAX_ROM_BYTES
#define REJECT_DRAIN_MAX MAX_UPLOAD_DEFAULT
#define ROM_QUOTA_DEFAULT (128ull * 1024ull * 1024ull)
#define MIN_FREE_DEFAULT (8ull * 1024ull * 1024ull)
#define MAX_ROM_JSON (256u * 1024u)
#define ROM_JSON_TAIL_RESERVE (64u * 1024u)
#define MAX_SCAN_ENTRIES 8192u
#define MAX_SCAN_DEPTH 6
/*
 * Keep one complete 100 ms freshness window at the bundled core's 48 kHz
 * ceiling.  The 40 ms packet pacer can legitimately observe about 50--60 ms
 * of PCM when a libretro frame and the network poll deadline straddle each
 * other; the former 2048-frame (42.7 ms) buffer silently cut samples from
 * those normal bursts and produced periodic clicks in the browser.
 */
#define AUDIO_PULL_FRAMES 4800u
#define JPEG_BUF_MAX (256u * 1024u)
#define JPEG_THREAD_STACK_BYTES (256u * 1024u)
#define WS_INPUT_CAP (16u * 1024u)
#define WS_MESSAGE_MAX 4096u
#define WS_RAW_PACKET_MAX (12u + NES_MAX_W * NES_MAX_H * 2u)
#define WS_JPEG_PACKET_MAX (12u + JPEG_BUF_MAX)
#define WS_OUTPUT_MAX \
	(WS_RAW_PACKET_MAX > WS_JPEG_PACKET_MAX ? WS_RAW_PACKET_MAX : WS_JPEG_PACKET_MAX)
#define HEADER_TIMEOUT_MS 5000ull
#define BODY_TIMEOUT_MS 15000ull
#define ERROR_DRAIN_TIMEOUT_MS 2000ull
#define UPLOAD_BASE_TIMEOUT_MS 30000ull
#define UPLOAD_INACTIVITY_TIMEOUT_MS 20000ull
#define UPLOAD_MIN_PROGRESS_BYTES_PER_SEC (8ull * 1024ull)
#define UPLOAD_MAX_TIMEOUT_MS 1800000ull
#define WS_IDLE_TIMEOUT_MS 30000ull
#define WS_PING_INTERVAL_MS 10000ull
#define WS_PONG_TIMEOUT_MS 12000ull
#define WS_OUTPUT_STALL_TIMEOUT_MS 4000ull
#define WS_OUTPUT_MAX_AGE_MS 12000ull
#define WS_CLOSE_TIMEOUT_MS 1000ull
#define WS_MEDIA_POLL_MAX_MS 20
#define WS_INPUT_LEASE_MS 3500ull
#define WS_APPLICATION_HEARTBEAT_TIMEOUT_MS 6000ull
#define WS_HEARTBEAT_SEQ_MAX 9007199254740991ull
#define WS_AUDIO_MAX_AGE_MS 250ull
#define WS_AUDIO_QUEUE_SLOTS 3u
#define WS_AUDIO_QUEUE_MAX_AGE_MS 120ull
#define WS_AUDIO_QUEUE_MAX_DURATION_US (120ull * 1000ull)
#define WS_VIDEO_MAX_AGE_MS 350ull
#define WS_FLUSH_BUDGET_BYTES (128u * 1024u)
#define WS_FLUSH_BUDGET_SYSCALLS 4u
#define WS_SOCKET_SNDBUF (64 * 1024)
#define WS_TCP_USER_TIMEOUT_MS 15000u
#define WS_TCP_NOTSENT_LOWAT_BYTES (8u * 1024u)
#define HTTP_OUTPUT_STALL_TIMEOUT_MS 10000ull
#define HTTP_OUTPUT_MAX_AGE_MS 30000ull
#define ROM_SCAN_ACTIVE_BUDGET_NS (20ull * 1000ull * 1000ull)
#define ROM_SCAN_IDLE_BUDGET_NS (250ull * 1000ull * 1000ull)
#define AUDIO_PACKET_PERIOD_NS (40ull * 1000ull * 1000ull)
#define HTTP_SOCKET_SNDBUF (16 * 1024)
#define IDLE_POLL_MAX_MS 250

enum client_phase {
	CLIENT_HTTP_HEADERS = 0,
	CLIENT_HTTP_BODY,
	CLIENT_HTTP_UPLOAD,
	CLIENT_HTTP_DISCARD,
	CLIENT_WEBSOCKET
};

enum websocket_output_kind {
	WS_OUT_NONE = 0,
	WS_OUT_HTTP,
	WS_OUT_CONTROL,
	WS_OUT_HEARTBEAT,
	WS_OUT_STATUS,
	WS_OUT_AUDIO,
	WS_OUT_VIDEO
};

struct websocket_audio_packet {
	uint8_t *frame;
	size_t length;
	uint64_t queued_ms;
	uint64_t duration_us;
};

struct client {
	int fd;
	int dead;
	int closing;
	int close_frame_sent;
	int close_frame_received;
	enum client_phase phase;
	uint64_t accepted_ms;
	uint64_t deadline_ms;
	uint64_t last_rx_ms;
	uint64_t last_ping_ms;
	uint64_t out_since_ms;
	uint64_t out_progress_ms;
	uint64_t close_deadline_ms;
	uint64_t last_input_ms;
	uint64_t last_heartbeat_seq;
	int application_heartbeat_required;
	uint64_t ping_sent_ms;
	uint64_t upload_absolute_deadline_ms;
	uint8_t ping_payload[8];
	int ping_queued;
	int ping_outstanding;
	int http_shutdown_after_flush;

	uint8_t in[HTTP_HEADER_MAX + 1];
	size_t in_len;
	char method[8];
	char target[1024];
	char path[512];
	char request_origin[NES_HTTP_ORIGIN_MAX];
	int cors_allowed;
	int preflight_private_network;
	size_t content_length;
	uint8_t *body;
	size_t body_got;

	int upload_fd;
	int upload_dir_fd;
	size_t upload_got;
	char upload_temp[128];
	char upload_name[256];
	char upload_path[PATH_MAX];

	uint8_t ws_in[WS_INPUT_CAP];
	size_t ws_in_len;
	uint8_t ws_message[WS_MESSAGE_MAX + 1];
	size_t ws_message_len;
	uint8_t ws_fragment_opcode;
	uint16_t joy[2];
	uint64_t stream_session_id;

	uint8_t *out;
	size_t out_len;
	size_t out_off;
	uint8_t out_opcode;
	enum websocket_output_kind out_kind;
	uint8_t *control_out;
	size_t control_len;
	uint8_t control_opcode;
	uint8_t *status_out;
	size_t status_len;
	uint8_t *heartbeat_out;
	size_t heartbeat_len;
	struct websocket_audio_packet audio_queue[WS_AUDIO_QUEUE_SLOTS];
	unsigned audio_queue_head;
	unsigned audio_queue_count;
	uint64_t audio_queue_duration_us;
	uint8_t *video_out;
	size_t video_len;
	uint64_t video_queued_ms;
	unsigned long dropped_packets;
};

struct request_info {
	char method[8];
	char target[1024];
	char path[512];
	char host[256];
	char origin[NES_HTTP_ORIGIN_MAX];
	char authorization[256];
	char content_type[128];
	char x_filename[256];
	char ws_key[128];
	char ws_version[32];
	char requested_method[16];
	char requested_headers[256];
	size_t content_length;
	int has_content_length;
	int has_transfer_encoding;
	int upgrade_websocket;
	int connection_upgrade;
	int private_network;
};

struct strbuf {
	char *data;
	size_t len;
	size_t cap;
	size_t max;
	int failed;
};

static uint16_t frame_scratch[NES_MAX_W * NES_MAX_H];
static int16_t audio_scratch[AUDIO_PULL_FRAMES * 2];
static int16_t audio_drain_scratch[AUDIO_PULL_FRAMES * 2];
static uint8_t raw_packet[12 + NES_MAX_W * NES_MAX_H * 2];

typedef size_t (*jpeg_encode_fn)(const uint16_t *, unsigned, unsigned, int,
	uint8_t *, size_t);

/*
 * One job may be encoding while exactly one newer frame waits. Submissions
 * overwrite that pending slot, so a slow CPU can never accumulate a video
 * backlog. The completed packet occupies the sole output slot until the
 * network thread consumes or expires it; meanwhile the pending input keeps
 * coalescing to the newest emulated frame.
 */
struct nes_jpeg_worker {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	pthread_t thread;
	int mutex_ready;
	int cond_ready;
	int thread_started;
	int stop;
	int wake_read_fd;
	int wake_write_fd;

	uint16_t *active_pixels;
	uint16_t *pending_pixels;
	uint8_t *packet;
	jpeg_encode_fn encode;

	int busy;
	int pending_ready;
	unsigned pending_width;
	unsigned pending_height;
	int pending_quality;
	uint64_t pending_frame_id;
	uint64_t pending_generation;

	int completed_ready;
	size_t completed_length;
	uint64_t completed_generation;
	uint64_t completed_ms;

	uint64_t generation;
	uint64_t viewer_session;
	uint64_t next_session;
};

struct video_pacer {
	uint64_t next_deadline_ns;
	uint64_t next_audio_deadline_ns;
	uint64_t last_video_id;
	unsigned retry_backoff_ms;
	int stream_fps;
	int active;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull +
	       (uint64_t)ts.tv_nsec;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000ull +
	       (uint64_t)ts.tv_nsec / 1000000ull;
}

static int set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD, 0);

	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static uint64_t jpeg_worker_next_generation(uint64_t generation)
{
	generation++;
	return generation ? generation : 1;
}

static size_t jpeg_worker_make_raw_packet(uint8_t *packet,
	const uint16_t *pixels, unsigned width, unsigned height,
	uint64_t frame_id)
{
	size_t count;
	size_t i;

	if (!packet || !pixels || !width || !height || width > NES_MAX_W ||
	    height > NES_MAX_H)
		return 0;
	count = (size_t)width * height;
	if (count > (WS_RAW_PACKET_MAX - 12u) / 2u)
		return 0;
	packet[0] = NES_PKT_VIDEO_RAW;
	packet[1] = 0;
	packet[2] = (uint8_t)width;
	packet[3] = (uint8_t)(width >> 8);
	packet[4] = (uint8_t)height;
	packet[5] = (uint8_t)(height >> 8);
	packet[6] = (uint8_t)frame_id;
	packet[7] = (uint8_t)(frame_id >> 8);
	packet[8] = (uint8_t)(frame_id >> 16);
	packet[9] = (uint8_t)(frame_id >> 24);
	packet[10] = 0;
	packet[11] = 0;
	for (i = 0; i < count; i++) {
		uint16_t value = pixels[i];

		packet[12 + i * 2] = (uint8_t)value;
		packet[13 + i * 2] = (uint8_t)(value >> 8);
	}
	return 12u + count * 2u;
}

static size_t jpeg_worker_encode_packet(struct nes_jpeg_worker *worker,
	const uint16_t *pixels, unsigned width, unsigned height, int quality,
	uint64_t frame_id)
{
	size_t jpeg_length;

	jpeg_length = worker->encode(pixels, width, height, quality,
		worker->packet + 12, JPEG_BUF_MAX);
	if (!jpeg_length || jpeg_length > JPEG_BUF_MAX)
		return jpeg_worker_make_raw_packet(worker->packet, pixels,
			width, height, frame_id);
	worker->packet[0] = NES_PKT_VIDEO_JPEG;
	worker->packet[1] = (uint8_t)quality;
	worker->packet[2] = (uint8_t)width;
	worker->packet[3] = (uint8_t)(width >> 8);
	worker->packet[4] = (uint8_t)height;
	worker->packet[5] = (uint8_t)(height >> 8);
	worker->packet[6] = (uint8_t)jpeg_length;
	worker->packet[7] = (uint8_t)(jpeg_length >> 8);
	worker->packet[8] = (uint8_t)(jpeg_length >> 16);
	worker->packet[9] = (uint8_t)(jpeg_length >> 24);
	worker->packet[10] = (uint8_t)frame_id;
	worker->packet[11] = (uint8_t)(frame_id >> 8);
	return 12u + jpeg_length;
}

static void jpeg_worker_lower_priority(void)
{
	int current;

	/*
	 * Linux (and therefore OpenWrt) applies PRIO_PROCESS with id 0 to the
	 * calling kernel thread. This is best effort: an unusual libc/kernel or
	 * policy may reject it, which must never make streaming fail.
	 */
	errno = 0;
	current = getpriority(PRIO_PROCESS, 0);
	if (errno == 0 && current < 19) {
		int wanted = current > 14 ? 19 : current + 5;

		(void)setpriority(PRIO_PROCESS, 0, wanted);
	}
}

static void jpeg_worker_signal_network(struct nes_jpeg_worker *worker)
{
	uint8_t byte = 1;
	ssize_t written;

	do {
		written = write(worker->wake_write_fd, &byte, sizeof(byte));
	} while (written < 0 && errno == EINTR);
	/* EAGAIN means an earlier wake byte is already pending. */
}

static void *jpeg_worker_thread(void *opaque)
{
	struct nes_jpeg_worker *worker = opaque;

	jpeg_worker_lower_priority();
	for (;;) {
		uint16_t *swap;
		unsigned width;
		unsigned height;
		int quality;
		uint64_t frame_id;
		uint64_t generation;
		size_t packet_length;
		int notify = 0;

		pthread_mutex_lock(&worker->mu);
		while (!worker->stop &&
		       (!worker->pending_ready || worker->completed_ready))
			pthread_cond_wait(&worker->cv, &worker->mu);
		if (worker->stop) {
			pthread_mutex_unlock(&worker->mu);
			break;
		}
		swap = worker->active_pixels;
		worker->active_pixels = worker->pending_pixels;
		worker->pending_pixels = swap;
		width = worker->pending_width;
		height = worker->pending_height;
		quality = worker->pending_quality;
		frame_id = worker->pending_frame_id;
		generation = worker->pending_generation;
		worker->pending_ready = 0;
		worker->busy = 1;
		pthread_cond_broadcast(&worker->cv);
		pthread_mutex_unlock(&worker->mu);

		packet_length = jpeg_worker_encode_packet(worker,
			worker->active_pixels, width, height, quality, frame_id);

		pthread_mutex_lock(&worker->mu);
		worker->busy = 0;
		if (!worker->stop && packet_length && worker->viewer_session &&
		    generation == worker->generation) {
			worker->completed_ready = 1;
			worker->completed_length = packet_length;
			worker->completed_generation = generation;
			worker->completed_ms = now_ms();
			notify = 1;
		}
		pthread_cond_broadcast(&worker->cv);
		pthread_mutex_unlock(&worker->mu);
		if (notify)
			jpeg_worker_signal_network(worker);
	}
	return NULL;
}

static void jpeg_worker_destroy(struct nes_jpeg_worker *worker)
{
	if (!worker)
		return;
	if (worker->thread_started) {
		pthread_mutex_lock(&worker->mu);
		worker->stop = 1;
		pthread_cond_broadcast(&worker->cv);
		pthread_mutex_unlock(&worker->mu);
		(void)pthread_join(worker->thread, NULL);
		worker->thread_started = 0;
	}
	if (worker->wake_read_fd >= 0)
		close(worker->wake_read_fd);
	if (worker->wake_write_fd >= 0)
		close(worker->wake_write_fd);
	free(worker->packet);
	free(worker->pending_pixels);
	free(worker->active_pixels);
	if (worker->cond_ready)
		(void)pthread_cond_destroy(&worker->cv);
	if (worker->mutex_ready)
		(void)pthread_mutex_destroy(&worker->mu);
	memset(worker, 0, sizeof(*worker));
	worker->wake_read_fd = -1;
	worker->wake_write_fd = -1;
}

static int jpeg_worker_init_with_encoder(struct nes_jpeg_worker *worker,
	jpeg_encode_fn encode)
{
	pthread_attr_t attr;
	int attr_ready = 0;
	int pipe_fds[2] = { -1, -1 };
	int error;

	if (!worker || !encode) {
		errno = EINVAL;
		return -1;
	}
	memset(worker, 0, sizeof(*worker));
	worker->wake_read_fd = -1;
	worker->wake_write_fd = -1;
	worker->generation = 1;
	worker->encode = encode;
	error = pthread_mutex_init(&worker->mu, NULL);
	if (error != 0) {
		errno = error;
		return -1;
	}
	worker->mutex_ready = 1;
	error = pthread_cond_init(&worker->cv, NULL);
	if (error != 0) {
		errno = error;
		goto fail;
	}
	worker->cond_ready = 1;
	worker->active_pixels = malloc(sizeof(frame_scratch));
	worker->pending_pixels = malloc(sizeof(frame_scratch));
	worker->packet = malloc(WS_RAW_PACKET_MAX);
	if (!worker->active_pixels || !worker->pending_pixels || !worker->packet) {
		errno = ENOMEM;
		goto fail;
	}
	if (pipe(pipe_fds) != 0)
		goto fail;
	worker->wake_read_fd = pipe_fds[0];
	worker->wake_write_fd = pipe_fds[1];
	if (set_nonblock(worker->wake_read_fd) != 0 ||
	    set_nonblock(worker->wake_write_fd) != 0 ||
	    set_cloexec(worker->wake_read_fd) != 0 ||
	    set_cloexec(worker->wake_write_fd) != 0)
		goto fail;
	if (pthread_attr_init(&attr) == 0) {
		attr_ready = 1;
		if (pthread_attr_setstacksize(&attr,
		    JPEG_THREAD_STACK_BYTES) != 0) {
			(void)pthread_attr_destroy(&attr);
			attr_ready = 0;
		}
	}
	error = pthread_create(&worker->thread, attr_ready ? &attr : NULL,
		jpeg_worker_thread, worker);
	if (attr_ready)
		(void)pthread_attr_destroy(&attr);
	if (error != 0) {
		errno = error;
		goto fail;
	}
	worker->thread_started = 1;
	return 0;

fail:
	error = errno ? errno : EIO;
	jpeg_worker_destroy(worker);
	errno = error;
	return -1;
}

static int jpeg_worker_init(struct nes_jpeg_worker *worker)
{
	return jpeg_worker_init_with_encoder(worker, jpeg_encode_rgb565);
}

typedef int (*jpeg_worker_init_fn)(struct nes_jpeg_worker *worker);

static int jpeg_worker_enable_optional(struct nes_http *srv,
	struct nes_jpeg_worker *worker, jpeg_worker_init_fn init_worker)
{
	int saved_error;

	if (!srv->stream.use_jpeg)
		return 0;
	srv->jpeg_worker = NULL;
	if (init_worker(worker) == 0) {
		srv->jpeg_worker = worker;
		return 1;
	}
	saved_error = errno ? errno : EIO;
	fprintf(stderr,
		"nesd: JPEG worker unavailable (%s); falling back to raw RGB565\n",
		strerror(saved_error));
	srv->stream.use_jpeg = 0;
	return 0;
}

static uint64_t jpeg_worker_new_session(struct nes_jpeg_worker *worker)
{
	uint64_t session;

	if (!worker)
		return 0;
	pthread_mutex_lock(&worker->mu);
	worker->next_session = jpeg_worker_next_generation(worker->next_session);
	session = worker->next_session;
	pthread_mutex_unlock(&worker->mu);
	return session;
}

static void jpeg_worker_set_session(struct nes_jpeg_worker *worker,
	uint64_t session)
{
	if (!worker)
		return;
	pthread_mutex_lock(&worker->mu);
	if (worker->viewer_session != session) {
		worker->generation = jpeg_worker_next_generation(worker->generation);
		worker->viewer_session = session;
		worker->pending_ready = 0;
		worker->completed_ready = 0;
		worker->completed_length = 0;
		worker->completed_ms = 0;
		pthread_cond_broadcast(&worker->cv);
	}
	pthread_mutex_unlock(&worker->mu);
}

static void jpeg_worker_reset(struct nes_jpeg_worker *worker)
{
	if (!worker)
		return;
	pthread_mutex_lock(&worker->mu);
	worker->generation = jpeg_worker_next_generation(worker->generation);
	worker->pending_ready = 0;
	worker->completed_ready = 0;
	worker->completed_length = 0;
	worker->completed_ms = 0;
	pthread_cond_broadcast(&worker->cv);
	pthread_mutex_unlock(&worker->mu);
}

static int jpeg_worker_submit(struct nes_jpeg_worker *worker,
	const uint16_t *pixels, unsigned width, unsigned height, int quality,
	uint64_t frame_id)
{
	size_t count;

	if (!worker || !pixels || !width || !height || width > NES_MAX_W ||
	    height > NES_MAX_H || quality < 1 || quality > 100)
		return -1;
	count = (size_t)width * height;
	pthread_mutex_lock(&worker->mu);
	if (!worker->viewer_session || worker->stop) {
		pthread_mutex_unlock(&worker->mu);
		return 0;
	}
	memcpy(worker->pending_pixels, pixels, count * sizeof(*pixels));
	worker->pending_width = width;
	worker->pending_height = height;
	worker->pending_quality = quality;
	worker->pending_frame_id = frame_id;
	worker->pending_generation = worker->generation;
	worker->pending_ready = 1;
	pthread_cond_broadcast(&worker->cv);
	pthread_mutex_unlock(&worker->mu);
	return 1;
}

static void jpeg_worker_expire(struct nes_jpeg_worker *worker, uint64_t now)
{
	if (!worker)
		return;
	pthread_mutex_lock(&worker->mu);
	if (worker->completed_ready && now && worker->completed_ms &&
	    now >= worker->completed_ms &&
	    now - worker->completed_ms > WS_VIDEO_MAX_AGE_MS) {
		worker->completed_ready = 0;
		worker->completed_length = 0;
		worker->completed_ms = 0;
		pthread_cond_broadcast(&worker->cv);
	}
	pthread_mutex_unlock(&worker->mu);
}

static int jpeg_worker_collect(struct nes_jpeg_worker *worker,
	uint8_t *packet, size_t capacity, size_t *packet_length)
{
	int result = 0;

	if (!worker || !packet || !packet_length)
		return -1;
	pthread_mutex_lock(&worker->mu);
	if (worker->completed_ready && worker->viewer_session &&
	    worker->completed_generation == worker->generation) {
		if (worker->completed_length > capacity) {
			result = -1;
		} else {
			memcpy(packet, worker->packet, worker->completed_length);
			*packet_length = worker->completed_length;
			result = 1;
		}
		worker->completed_ready = 0;
		worker->completed_length = 0;
		worker->completed_ms = 0;
		pthread_cond_broadcast(&worker->cv);
	}
	pthread_mutex_unlock(&worker->mu);
	return result;
}

static void jpeg_worker_drain_wake(struct nes_jpeg_worker *worker)
{
	uint8_t bytes[64];

	if (!worker)
		return;
	for (;;) {
		ssize_t count = read(worker->wake_read_fd, bytes, sizeof(bytes));

		if (count > 0)
			continue;
		if (count < 0 && errno == EINTR)
			continue;
		break;
	}
}

/*
 * Keep latency bounded when Wi-Fi throughput collapses.  A large autotuned
 * send buffer can otherwise accept several old video frames before the
 * application notices backpressure.  Every option is an optimisation: older
 * OpenWrt kernels may reject the Linux-specific TCP options, so failures are
 * deliberately non-fatal.
 */
static void websocket_tune_socket(int fd)
{
	int enabled = 1;
	int send_buffer = WS_SOCKET_SNDBUF;

	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
	(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer,
		sizeof(send_buffer));
#ifdef TCP_USER_TIMEOUT
	{
		unsigned timeout = WS_TCP_USER_TIMEOUT_MS;

		(void)setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout,
			sizeof(timeout));
	}
#endif
#ifdef TCP_NOTSENT_LOWAT
	{
		unsigned low_water = WS_TCP_NOTSENT_LOWAT_BYTES;

		(void)setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &low_water,
			sizeof(low_water));
	}
#endif
}

static int send_all_to(int fd, const void *data, size_t len, int timeout_ms)
{
	const uint8_t *p = data;
	uint64_t deadline = now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n > 0) {
			p += (size_t)n;
			len -= (size_t)n;
			continue;
		}
		if (n == 0)
			return -1;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			struct pollfd pfd;
			uint64_t now = now_ms();
			int wait_ms;

			if (timeout_ms <= 0 || now >= deadline)
				return -1;
			wait_ms = (int)(deadline - now);
			if (wait_ms > 50)
				wait_ms = 50;
			pfd.fd = fd;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			if (poll(&pfd, 1, wait_ms) < 0 && errno != EINTR)
				return -1;
			continue;
		}
		return -1;
	}
	return 0;
}

static int write_all_fd(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n > 0) {
			p += (size_t)n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static int safe_add_size(size_t a, size_t b, size_t *out)
{
	if (a > SIZE_MAX - b)
		return -1;
	*out = a + b;
	return 0;
}

static int sb_init(struct strbuf *sb, size_t initial, size_t max)
{
	if (!sb || initial == 0 || initial > max)
		return -1;
	memset(sb, 0, sizeof(*sb));
	sb->data = malloc(initial);
	if (!sb->data)
		return -1;
	sb->cap = initial;
	sb->max = max;
	sb->data[0] = '\0';
	return 0;
}

static void sb_free(struct strbuf *sb)
{
	if (!sb)
		return;
	free(sb->data);
	memset(sb, 0, sizeof(*sb));
}

static int sb_reserve(struct strbuf *sb, size_t extra)
{
	size_t need;
	size_t cap;
	char *next;

	if (!sb || sb->failed || safe_add_size(sb->len, extra, &need) != 0 ||
	    safe_add_size(need, 1, &need) != 0 || need > sb->max) {
		if (sb)
			sb->failed = 1;
		return -1;
	}
	if (need <= sb->cap)
		return 0;
	cap = sb->cap;
	while (cap < need) {
		size_t grown = cap <= sb->max / 2 ? cap * 2 : sb->max;

		if (grown <= cap) {
			sb->failed = 1;
			return -1;
		}
		cap = grown;
	}
	next = realloc(sb->data, cap);
	if (!next) {
		sb->failed = 1;
		return -1;
	}
	sb->data = next;
	sb->cap = cap;
	return 0;
}

static int sb_append_mem(struct strbuf *sb, const void *data, size_t len)
{
	if (sb_reserve(sb, len) != 0)
		return -1;
	if (len)
		memcpy(sb->data + sb->len, data, len);
	sb->len += len;
	sb->data[sb->len] = '\0';
	return 0;
}

static int sb_append(struct strbuf *sb, const char *text)
{
	return sb_append_mem(sb, text, strlen(text));
}

static int sb_appendf(struct strbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_list aq;
	int needed;

	va_start(ap, fmt);
	va_copy(aq, ap);
	needed = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (needed < 0 || sb_reserve(sb, (size_t)needed) != 0) {
		va_end(ap);
		return -1;
	}
	(void)vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
	va_end(ap);
	sb->len += (size_t)needed;
	return 0;
}

static size_t utf8_sequence_length(const unsigned char *p)
{
	if (p[0] >= 0xc2 && p[0] <= 0xdf &&
	    p[1] >= 0x80 && p[1] <= 0xbf)
		return 2;
	if (p[0] == 0xe0 &&
	    p[1] >= 0xa0 && p[1] <= 0xbf &&
	    p[2] >= 0x80 && p[2] <= 0xbf)
		return 3;
	if (((p[0] >= 0xe1 && p[0] <= 0xec) ||
	     (p[0] >= 0xee && p[0] <= 0xef)) &&
	    p[1] >= 0x80 && p[1] <= 0xbf &&
	    p[2] >= 0x80 && p[2] <= 0xbf)
		return 3;
	if (p[0] == 0xed &&
	    p[1] >= 0x80 && p[1] <= 0x9f &&
	    p[2] >= 0x80 && p[2] <= 0xbf)
		return 3;
	if (p[0] == 0xf0 &&
	    p[1] >= 0x90 && p[1] <= 0xbf &&
	    p[2] >= 0x80 && p[2] <= 0xbf &&
	    p[3] >= 0x80 && p[3] <= 0xbf)
		return 4;
	if (p[0] >= 0xf1 && p[0] <= 0xf3 &&
	    p[1] >= 0x80 && p[1] <= 0xbf &&
	    p[2] >= 0x80 && p[2] <= 0xbf &&
	    p[3] >= 0x80 && p[3] <= 0xbf)
		return 4;
	if (p[0] == 0xf4 &&
	    p[1] >= 0x80 && p[1] <= 0x8f &&
	    p[2] >= 0x80 && p[2] <= 0xbf &&
	    p[3] >= 0x80 && p[3] <= 0xbf)
		return 4;
	return 0;
}

static int utf8_valid_string(const char *text)
{
	const unsigned char *p = (const unsigned char *)(text ? text : "");

	while (*p) {
		size_t length;

		if (*p < 0x80) {
			p++;
			continue;
		}
		length = utf8_sequence_length(p);
		if (!length)
			return 0;
		p += length;
	}
	return 1;
}

static size_t utf8_encode(uint32_t codepoint, char out[4])
{
	if (codepoint <= 0x7f) {
		out[0] = (char)codepoint;
		return 1;
	}
	if (codepoint <= 0x7ff) {
		out[0] = (char)(0xc0u | (codepoint >> 6));
		out[1] = (char)(0x80u | (codepoint & 0x3fu));
		return 2;
	}
	if (codepoint >= 0xd800 && codepoint <= 0xdfff)
		return 0;
	if (codepoint <= 0xffff) {
		out[0] = (char)(0xe0u | (codepoint >> 12));
		out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
		out[2] = (char)(0x80u | (codepoint & 0x3fu));
		return 3;
	}
	if (codepoint <= 0x10ffff) {
		out[0] = (char)(0xf0u | (codepoint >> 18));
		out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
		out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
		out[3] = (char)(0x80u | (codepoint & 0x3fu));
		return 4;
	}
	return 0;
}

static int sb_append_json_string(struct strbuf *sb, const char *text)
{
	const unsigned char *p = (const unsigned char *)(text ? text : "");

	if (sb_append(sb, "\"") != 0)
		return -1;
	while (*p) {
		char esc[7];

		switch (*p) {
		case '"':
			if (sb_append(sb, "\\\"") != 0)
				return -1;
			break;
		case '\\':
			if (sb_append(sb, "\\\\") != 0)
				return -1;
			break;
		case '\b':
			if (sb_append(sb, "\\b") != 0)
				return -1;
			break;
		case '\f':
			if (sb_append(sb, "\\f") != 0)
				return -1;
			break;
		case '\n':
			if (sb_append(sb, "\\n") != 0)
				return -1;
			break;
		case '\r':
			if (sb_append(sb, "\\r") != 0)
				return -1;
			break;
		case '\t':
			if (sb_append(sb, "\\t") != 0)
				return -1;
			break;
		default:
			if (*p < 0x20 || *p == 0x7f) {
				(void)snprintf(esc, sizeof(esc), "\\u%04x", *p);
				if (sb_append(sb, esc) != 0)
					return -1;
			} else if (*p >= 0x80) {
				size_t length = utf8_sequence_length(p);

				if (length) {
					if (sb_append_mem(sb, p, length) != 0)
						return -1;
					p += length;
					continue;
				}
				if (sb_append(sb, "\\ufffd") != 0)
					return -1;
			} else if (sb_append_mem(sb, p, 1) != 0) {
				return -1;
			}
			break;
		}
		p++;
	}
	return sb_append(sb, "\"");
}

static int contains_ctl_or_crlf(const char *s)
{
	const unsigned char *p = (const unsigned char *)(s ? s : "");

	for (; *p; p++) {
		if (*p < 0x20 || *p == 0x7f)
			return 1;
	}
	return 0;
}

static int constant_time_equal(const char *a, const char *b)
{
	size_t alen = a ? strlen(a) : 0;
	size_t blen = b ? strlen(b) : 0;
	size_t n = alen > blen ? alen : blen;
	unsigned diff = (unsigned)(alen ^ blen);
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned ac = i < alen ? (unsigned char)a[i] : 0;
		unsigned bc = i < blen ? (unsigned char)b[i] : 0;
		diff |= ac ^ bc;
	}
	return diff == 0;
}

static const char *status_text(int code)
{
	switch (code) {
	case 200: return "OK";
	case 101: return "Switching Protocols";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Content Too Large";
	case 415: return "Unsupported Media Type";
	case 426: return "Upgrade Required";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	case 507: return "Insufficient Storage";
	default: return "Error";
	}
}

static int http_response(struct nes_http *srv, struct client *c, int code,
	const char *ctype, const void *body, size_t body_len, const char *extra)
{
	char header[1536];
	uint8_t *response;
	size_t response_len;
	uint64_t queued_at;
	int n;
	const char *cors = "";
	char cors_header[512];

	cors_header[0] = '\0';
	if (srv->allowed_origin[0] && c->cors_allowed) {
		n = snprintf(cors_header, sizeof(cors_header),
			"Access-Control-Allow-Origin: %s\r\n"
			"Vary: Origin\r\n", srv->allowed_origin);
		if (n < 0 || (size_t)n >= sizeof(cors_header))
			return -1;
		cors = cors_header;
	}
	n = snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"X-Frame-Options: DENY\r\n"
		"Referrer-Policy: no-referrer\r\n"
		"Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n"
		"%s%s"
		"\r\n",
		code, status_text(code), ctype, body_len, cors, extra ? extra : "");
	if (n < 0 || (size_t)n >= sizeof(header))
		return -1;
	if (c->out || safe_add_size((size_t)n, body_len, &response_len) != 0)
		return -1;
	response = malloc(response_len ? response_len : 1);
	if (!response)
		return -1;
	memcpy(response, header, (size_t)n);
	if (body_len)
		memcpy(response + (size_t)n, body, body_len);
	queued_at = now_ms();
	c->out = response;
	c->out_len = response_len;
	c->out_off = 0;
	c->out_opcode = 0;
	c->out_kind = WS_OUT_HTTP;
	c->out_since_ms = queued_at;
	c->out_progress_ms = queued_at;
	return 0;
}

static void http_json(struct nes_http *srv, struct client *c, int code,
	const char *json)
{
	(void)http_response(srv, c, code, "application/json; charset=utf-8",
		json, strlen(json), code == 401
			? "WWW-Authenticate: Bearer realm=\"nes-emulator\"\r\n" : NULL);
}

static void http_error(struct nes_http *srv, struct client *c, int code,
	const char *message)
{
	struct strbuf sb;

	if (sb_init(&sb, 128, 1024) != 0) {
		http_json(srv, c, 500, "{\"error\":\"internal error\"}");
		return;
	}
	(void)sb_append(&sb, "{\"error\":");
	(void)sb_append_json_string(&sb, message);
	(void)sb_append(&sb, "}");
	if (sb.failed)
		http_json(srv, c, 500, "{\"error\":\"internal error\"}");
	else
		http_json(srv, c, code, sb.data);
	sb_free(&sb);
}

static void begin_http_discard_lengths(struct client *c,
	size_t declared_length, size_t received_length, size_t limit,
	uint64_t timeout_ms)
{
	size_t drain_length;

	if (c->out && c->out_kind == WS_OUT_HTTP)
		c->http_shutdown_after_flush = 1;
	else
		(void)shutdown(c->fd, SHUT_WR);
	if (declared_length == 0 || limit == 0) {
		c->dead = 1;
		return;
	}
	drain_length = declared_length > limit ? limit : declared_length;
	c->content_length = drain_length;
	c->body_got = received_length > drain_length
		? drain_length : received_length;
	c->in_len = 0;
	if (c->body_got == c->content_length) {
		c->dead = 1;
		return;
	}
	c->phase = CLIENT_HTTP_DISCARD;
	c->deadline_ms = now_ms() + timeout_ms;
}

static void begin_http_discard(struct client *c,
	const struct request_info *request, size_t initial_length)
{
	if (!request->has_content_length) {
		if (c->out && c->out_kind == WS_OUT_HTTP)
			c->http_shutdown_after_flush = 1;
		else
			(void)shutdown(c->fd, SHUT_WR);
		c->dead = 1;
		return;
	}
	begin_http_discard_lengths(c, request->content_length, initial_length,
		REJECT_DRAIN_MAX, ERROR_DRAIN_TIMEOUT_MS);
}

static void reject_http_request(struct nes_http *srv, struct client *c,
	const struct request_info *request, size_t initial_length,
	int code, const char *message)
{
	http_error(srv, c, code, message);
	begin_http_discard(c, request, initial_length);
}

static uint32_t rol32(uint32_t value, unsigned amount)
{
	return (value << amount) | (value >> (32u - amount));
}

static int sha1_digest(const uint8_t *data, size_t len, uint8_t out[20])
{
	uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe;
	uint32_t h3 = 0x10325476, h4 = 0xc3d2e1f0;
	uint8_t *message;
	size_t padded;
	size_t offset;
	uint64_t bit_len;

	if (len > (SIZE_MAX - 72) || len > UINT64_MAX / 8)
		return -1;
	padded = ((len + 9 + 63) / 64) * 64;
	message = calloc(1, padded);
	if (!message)
		return -1;
	memcpy(message, data, len);
	message[len] = 0x80;
	bit_len = (uint64_t)len * 8;
	message[padded - 8] = (uint8_t)(bit_len >> 56);
	message[padded - 7] = (uint8_t)(bit_len >> 48);
	message[padded - 6] = (uint8_t)(bit_len >> 40);
	message[padded - 5] = (uint8_t)(bit_len >> 32);
	message[padded - 4] = (uint8_t)(bit_len >> 24);
	message[padded - 3] = (uint8_t)(bit_len >> 16);
	message[padded - 2] = (uint8_t)(bit_len >> 8);
	message[padded - 1] = (uint8_t)bit_len;

	for (offset = 0; offset < padded; offset += 64) {
		uint32_t w[80];
		uint32_t a, b, cc, d, e;
		unsigned i;

		for (i = 0; i < 16; i++) {
			size_t p = offset + i * 4;
			w[i] = ((uint32_t)message[p] << 24) |
			       ((uint32_t)message[p + 1] << 16) |
			       ((uint32_t)message[p + 2] << 8) |
			       message[p + 3];
		}
		for (i = 16; i < 80; i++)
			w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		a = h0;
		b = h1;
		cc = h2;
		d = h3;
		e = h4;
		for (i = 0; i < 80; i++) {
			uint32_t f;
			uint32_t k;
			uint32_t temp;

			if (i < 20) {
				f = (b & cc) | ((~b) & d);
				k = 0x5a827999;
			} else if (i < 40) {
				f = b ^ cc ^ d;
				k = 0x6ed9eba1;
			} else if (i < 60) {
				f = (b & cc) | (b & d) | (cc & d);
				k = 0x8f1bbcdc;
			} else {
				f = b ^ cc ^ d;
				k = 0xca62c1d6;
			}
			temp = rol32(a, 5) + f + e + k + w[i];
			e = d;
			d = cc;
			cc = rol32(b, 30);
			b = a;
			a = temp;
		}
		h0 += a;
		h1 += b;
		h2 += cc;
		h3 += d;
		h4 += e;
	}
	free(message);
	out[0] = (uint8_t)(h0 >> 24); out[1] = (uint8_t)(h0 >> 16);
	out[2] = (uint8_t)(h0 >> 8); out[3] = (uint8_t)h0;
	out[4] = (uint8_t)(h1 >> 24); out[5] = (uint8_t)(h1 >> 16);
	out[6] = (uint8_t)(h1 >> 8); out[7] = (uint8_t)h1;
	out[8] = (uint8_t)(h2 >> 24); out[9] = (uint8_t)(h2 >> 16);
	out[10] = (uint8_t)(h2 >> 8); out[11] = (uint8_t)h2;
	out[12] = (uint8_t)(h3 >> 24); out[13] = (uint8_t)(h3 >> 16);
	out[14] = (uint8_t)(h3 >> 8); out[15] = (uint8_t)h3;
	out[16] = (uint8_t)(h4 >> 24); out[17] = (uint8_t)(h4 >> 16);
	out[18] = (uint8_t)(h4 >> 8); out[19] = (uint8_t)h4;
	return 0;
}

static int base64_encode(const uint8_t *in, size_t len, char *out, size_t out_size)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;
	size_t o = 0;

	if (out_size < ((len + 2) / 3) * 4 + 1)
		return -1;
	for (i = 0; i < len; i += 3) {
		uint32_t value = (uint32_t)in[i] << 16;

		if (i + 1 < len)
			value |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < len)
			value |= in[i + 2];
		out[o++] = table[(value >> 18) & 63];
		out[o++] = table[(value >> 12) & 63];
		out[o++] = i + 1 < len ? table[(value >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? table[value & 63] : '=';
	}
	out[o] = '\0';
	return 0;
}

static int base64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int websocket_key_valid(const char *key)
{
	size_t len = strlen(key);
	size_t i;
	size_t decoded = 0;

	if (len != 24 || key[22] != '=' || key[23] != '=')
		return 0;
	for (i = 0; i < 22; i++) {
		if (base64_value((unsigned char)key[i]) < 0)
			return 0;
	}
	for (i = 0; i < len; i += 4) {
		int a = base64_value((unsigned char)key[i]);
		int b = base64_value((unsigned char)key[i + 1]);
		int c = key[i + 2] == '=' ? -2 : base64_value((unsigned char)key[i + 2]);
		int d = key[i + 3] == '=' ? -2 : base64_value((unsigned char)key[i + 3]);

		if (a < 0 || b < 0 || c == -1 || d == -1)
			return 0;
		decoded++;
		if (c >= 0)
			decoded++;
		if (d >= 0)
			decoded++;
	}
	return decoded == 16;
}

static int websocket_handshake(struct client *c, const struct request_info *request)
{
	char source[256];
	char accept[64];
	char response[512];
	uint8_t digest[20];
	int n;

	n = snprintf(source, sizeof(source),
		"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", request->ws_key);
	if (n < 0 || (size_t)n >= sizeof(source) ||
	    sha1_digest((const uint8_t *)source, (size_t)n, digest) != 0 ||
	    base64_encode(digest, sizeof(digest), accept, sizeof(accept)) != 0)
		return -1;
	n = snprintf(response, sizeof(response),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n"
		"Cache-Control: no-store\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"\r\n", accept);
	if (n < 0 || (size_t)n >= sizeof(response))
		return -1;
	/* The upgrade is tiny and the accepted socket starts with an empty buffer. */
	return send_all_to(c->fd, response, (size_t)n, 50);
}

static int websocket_frame_alloc(int opcode, const void *payload, size_t length,
	uint8_t **out, size_t *out_len)
{
	uint8_t header[10];
	size_t header_len;
	size_t total;
	uint8_t *frame;
	unsigned i;

	if (length > WS_OUTPUT_MAX)
		return -1;
	header[0] = (uint8_t)(0x80 | (opcode & 0x0f));
	if (length < 126) {
		header[1] = (uint8_t)length;
		header_len = 2;
	} else if (length <= 0xffff) {
		header[1] = 126;
		header[2] = (uint8_t)(length >> 8);
		header[3] = (uint8_t)length;
		header_len = 4;
	} else {
		uint64_t value = (uint64_t)length;

		header[1] = 127;
		for (i = 0; i < 8; i++)
			header[2 + i] = (uint8_t)(value >> (56 - i * 8));
		header_len = 10;
	}
	if (safe_add_size(header_len, length, &total) != 0)
		return -1;
	frame = malloc(total);
	if (!frame)
		return -1;
	memcpy(frame, header, header_len);
	if (length)
		memcpy(frame + header_len, payload, length);
	*out = frame;
	*out_len = total;
	return 0;
}

static void websocket_activate(struct client *c, uint8_t *frame,
	size_t frame_len, uint8_t opcode, enum websocket_output_kind kind)
{
	uint64_t now = now_ms();

	c->out = frame;
	c->out_len = frame_len;
	c->out_off = 0;
	c->out_opcode = opcode;
	c->out_kind = kind;
	c->out_since_ms = now;
	c->out_progress_ms = now;
}

static void websocket_clear_active(struct client *c)
{
	free(c->out);
	c->out = NULL;
	c->out_len = 0;
	c->out_off = 0;
	c->out_opcode = 0;
	c->out_kind = WS_OUT_NONE;
	c->out_since_ms = 0;
	c->out_progress_ms = 0;
}

static struct websocket_audio_packet *websocket_audio_oldest(struct client *c)
{
	if (!c || !c->audio_queue_count)
		return NULL;
	return &c->audio_queue[c->audio_queue_head];
}

static void websocket_audio_drop_oldest(struct client *c, int count_drop)
{
	struct websocket_audio_packet *packet = websocket_audio_oldest(c);

	if (!packet)
		return;
	free(packet->frame);
	if (c->audio_queue_duration_us >= packet->duration_us)
		c->audio_queue_duration_us -= packet->duration_us;
	else
		c->audio_queue_duration_us = 0;
	memset(packet, 0, sizeof(*packet));
	c->audio_queue_head =
		(c->audio_queue_head + 1u) % WS_AUDIO_QUEUE_SLOTS;
	c->audio_queue_count--;
	if (!c->audio_queue_count) {
		c->audio_queue_head = 0;
		c->audio_queue_duration_us = 0;
	}
	if (count_drop)
		c->dropped_packets++;
}

static void websocket_audio_clear(struct client *c)
{
	while (c && c->audio_queue_count)
		websocket_audio_drop_oldest(c, 0);
}

static int websocket_audio_take_oldest(struct client *c, uint8_t **frame,
	size_t *length)
{
	struct websocket_audio_packet *packet = websocket_audio_oldest(c);

	if (!packet || !frame || !length)
		return 0;
	*frame = packet->frame;
	*length = packet->length;
	packet->frame = NULL;
	websocket_audio_drop_oldest(c, 0);
	return 1;
}

/*
 * priority is used for control and application status frames. Once a frame is
 * active it must finish before another frame may begin; separate bounded slots
 * keep RFC 6455 control traffic ahead of application status and media.
 */
static int websocket_queue(struct client *c, int opcode, const void *payload,
	size_t length, int priority)
{
	uint8_t *frame;
	size_t frame_len;
	int closing_frame = opcode == 0x8;

	if (websocket_frame_alloc(opcode, payload, length, &frame, &frame_len) != 0)
		return -1;
	if (closing_frame) {
		websocket_audio_clear(c);
		free(c->video_out);
		c->video_out = NULL;
		c->video_len = 0;
		free(c->status_out);
		c->status_out = NULL;
		c->status_len = 0;
		free(c->heartbeat_out);
		c->heartbeat_out = NULL;
		c->heartbeat_len = 0;
		free(c->control_out);
		if (c->control_opcode == 0x9)
			c->ping_queued = 0;
		c->control_out = NULL;
		c->control_len = 0;
		c->control_opcode = 0;
		/* A frame that has not emitted a byte can be safely pre-empted. */
		if (c->out && c->out_off == 0) {
			if (c->out_opcode == 0x9)
				c->ping_queued = 0;
			websocket_clear_active(c);
		}
	}
	if (!c->out) {
		websocket_activate(c, frame, frame_len, (uint8_t)opcode,
			(opcode & 0x08) ? WS_OUT_CONTROL : WS_OUT_STATUS);
		if (opcode == 0x9)
			c->ping_queued = 1;
		return 0;
	}
	if (opcode & 0x08) {
		/*
		 * RFC 6455 control traffic must not sit behind queued application
		 * status. A Pong may replace another pending control frame: RFC 6455
		 * explicitly permits responding only to the most recent queued Ping.
		 */
		if (!c->control_out || opcode == 0x0a) {
			if (c->control_opcode == 0x9)
				c->ping_queued = 0;
			free(c->control_out);
			c->control_out = frame;
			c->control_len = frame_len;
			c->control_opcode = (uint8_t)opcode;
			if (opcode == 0x9)
				c->ping_queued = 1;
			return 0;
		}
	} else if (priority) {
		if (c->status_out)
			c->dropped_packets++;
		free(c->status_out);
		c->status_out = frame;
		c->status_len = frame_len;
		return 0;
	}
	free(frame);
	c->dropped_packets++;
	return 1;
}

static int websocket_queue_heartbeat(struct client *c, uint64_t sequence)
{
	char reply[80];
	uint8_t *frame;
	size_t frame_len;
	int reply_len;

	if (c->closing)
		return 1;
	reply_len = snprintf(reply, sizeof(reply),
		"{\"t\":\"heartbeat\",\"seq\":%llu}",
		(unsigned long long)sequence);
	if (reply_len < 0 || (size_t)reply_len >= sizeof(reply) ||
	    websocket_frame_alloc(0x1, reply, (size_t)reply_len,
	    &frame, &frame_len) != 0)
		return -1;
	/* Never let an older unsent ACK follow this latest sequence. */
	if (c->heartbeat_out) {
		free(c->heartbeat_out);
		c->heartbeat_out = NULL;
		c->heartbeat_len = 0;
		c->dropped_packets++;
	}
	if (c->out && c->out_kind == WS_OUT_HEARTBEAT && c->out_off == 0) {
		websocket_clear_active(c);
		c->dropped_packets++;
	}
	if (!c->out) {
		websocket_activate(c, frame, frame_len, 0x1, WS_OUT_HEARTBEAT);
		return 0;
	}
	c->heartbeat_out = frame;
	c->heartbeat_len = frame_len;
	return 0;
}

/*
 * Video is latency-sensitive but expendable. Keep at most one frame behind
 * active control/status/audio, but never keep a second video behind an active
 * video. The producer skips copying and JPEG encoding while that single video
 * is occupied, and expires an unsent pending video quickly if the link stalls.
 * Control, status and audio retain priority when the active frame completes.
 */
static int websocket_queue_video(struct client *c, const void *payload,
	size_t length)
{
	uint8_t *frame;
	size_t frame_len;

	if (c->closing)
		return 1;
	if (websocket_frame_alloc(0x2, payload, length, &frame, &frame_len) != 0)
		return -1;
	if (!c->out) {
		websocket_activate(c, frame, frame_len, 0x2, WS_OUT_VIDEO);
		return 0;
	}
	if (c->out_kind == WS_OUT_VIDEO || c->video_out) {
		free(frame);
		c->dropped_packets++;
		return 1;
	}
	c->video_out = frame;
	c->video_len = frame_len;
	c->video_queued_ms = now_ms();
	return 0;
}

static int websocket_queue_audio(struct client *c, const void *payload,
	size_t length, uint64_t duration_us)
{
	struct websocket_audio_packet *packet;
	uint8_t *frame;
	size_t frame_len;
	unsigned tail;

	if (c->closing)
		return 1;
	if (!duration_us || duration_us > WS_AUDIO_QUEUE_MAX_DURATION_US) {
		c->dropped_packets++;
		return 1;
	}
	if (websocket_frame_alloc(0x2, payload, length, &frame, &frame_len) != 0)
		return -1;
	if (!c->out) {
		websocket_activate(c, frame, frame_len, 0x2, WS_OUT_AUDIO);
		return 0;
	}
	/*
	 * Preserve short, ordered PCM jitter behind an in-flight WebSocket frame.
	 * Bound both slots and decoded duration: unlike video, replacing every
	 * pending audio packet would create an audible hole whenever a raw frame
	 * takes more than one 40 ms audio period to leave the TCP socket.
	 */
	while (c->audio_queue_count &&
	    (c->audio_queue_count >= WS_AUDIO_QUEUE_SLOTS ||
	     duration_us > WS_AUDIO_QUEUE_MAX_DURATION_US -
		c->audio_queue_duration_us))
		websocket_audio_drop_oldest(c, 1);
	if (c->audio_queue_count >= WS_AUDIO_QUEUE_SLOTS ||
	    duration_us > WS_AUDIO_QUEUE_MAX_DURATION_US -
		c->audio_queue_duration_us) {
		free(frame);
		c->dropped_packets++;
		return 1;
	}
	tail = (c->audio_queue_head + c->audio_queue_count) %
		WS_AUDIO_QUEUE_SLOTS;
	packet = &c->audio_queue[tail];
	packet->frame = frame;
	packet->length = frame_len;
	packet->queued_ms = now_ms();
	packet->duration_us = duration_us;
	c->audio_queue_count++;
	c->audio_queue_duration_us += duration_us;
	return 0;
}

static void websocket_promote_next(struct client *c, uint64_t now)
{
	if (c->out)
		return;
	if (c->control_out) {
		uint8_t opcode = c->control_opcode;
		uint8_t *frame = c->control_out;
		size_t frame_len = c->control_len;

		c->control_out = NULL;
		c->control_len = 0;
		c->control_opcode = 0;
		websocket_activate(c, frame, frame_len, opcode, WS_OUT_CONTROL);
		return;
	}
	if (c->heartbeat_out) {
		uint8_t *frame = c->heartbeat_out;
		size_t frame_len = c->heartbeat_len;

		c->heartbeat_out = NULL;
		c->heartbeat_len = 0;
		websocket_activate(c, frame, frame_len, 0x1, WS_OUT_HEARTBEAT);
		return;
	}
	if (c->status_out) {
		uint8_t *frame = c->status_out;
		size_t frame_len = c->status_len;

		c->status_out = NULL;
		c->status_len = 0;
		websocket_activate(c, frame, frame_len, 0x1, WS_OUT_STATUS);
		return;
	}
	while (c->audio_queue_count) {
		struct websocket_audio_packet *packet =
			websocket_audio_oldest(c);

		if (packet && now - packet->queued_ms <=
		    WS_AUDIO_QUEUE_MAX_AGE_MS) {
			uint8_t *frame;
			size_t frame_len;

			if (!websocket_audio_take_oldest(c, &frame, &frame_len))
				break;
			websocket_activate(c, frame, frame_len, 0x2, WS_OUT_AUDIO);
			return;
		}
		websocket_audio_drop_oldest(c, 1);
	}
	if (c->video_out) {
		if (now - c->video_queued_ms <= WS_VIDEO_MAX_AGE_MS) {
			uint8_t *frame = c->video_out;
			size_t frame_len = c->video_len;

			c->video_out = NULL;
			c->video_len = 0;
			c->video_queued_ms = 0;
			websocket_activate(c, frame, frame_len, 0x2, WS_OUT_VIDEO);
			return;
		}
		free(c->video_out);
		c->video_out = NULL;
		c->video_len = 0;
		c->video_queued_ms = 0;
		c->dropped_packets++;
	}
}

static void websocket_expire_stale_media(struct client *c, uint64_t now)
{
	for (;;) {
		struct websocket_audio_packet *packet =
			websocket_audio_oldest(c);

		if (!packet || now - packet->queued_ms <=
		    WS_AUDIO_QUEUE_MAX_AGE_MS)
			break;
		websocket_audio_drop_oldest(c, 1);
	}
	if (c->video_out && now - c->video_queued_ms > WS_VIDEO_MAX_AGE_MS) {
		free(c->video_out);
		c->video_out = NULL;
		c->video_len = 0;
		c->video_queued_ms = 0;
		c->dropped_packets++;
	}
	if (c->out && c->out_off == 0 &&
	    ((c->out_kind == WS_OUT_AUDIO &&
	      now - c->out_since_ms > WS_AUDIO_MAX_AGE_MS) ||
	     (c->out_kind == WS_OUT_VIDEO &&
	      now - c->out_since_ms > WS_VIDEO_MAX_AGE_MS))) {
		websocket_clear_active(c);
		c->dropped_packets++;
		websocket_promote_next(c, now);
	}
}

static int websocket_output_expired(const struct client *c, uint64_t now)
{
	if (!c || !c->out)
		return 0;
	/*
	 * The event-loop timestamp is sampled before it may queue a Ping or Close.
	 * A later websocket_activate() timestamp can therefore be one tick ahead;
	 * guard ordering before subtraction so unsigned age never wraps.
	 */
	return (now >= c->out_progress_ms &&
		now - c->out_progress_ms >= WS_OUTPUT_STALL_TIMEOUT_MS) ||
		(now >= c->out_since_ms &&
		now - c->out_since_ms >= WS_OUTPUT_MAX_AGE_MS);
}

static int websocket_flush(struct client *c)
{
	size_t byte_budget = WS_FLUSH_BUDGET_BYTES;
	unsigned syscall_budget = WS_FLUSH_BUDGET_SYSCALLS;

	while (c->out && byte_budget && syscall_budget) {
		size_t remaining = c->out_len - c->out_off;
		size_t wanted = remaining < byte_budget ? remaining : byte_budget;
		size_t old_offset = c->out_off;
		ssize_t n = send(c->fd, c->out + c->out_off,
			wanted, MSG_NOSIGNAL);

		syscall_budget--;

		if (n > 0) {
			uint64_t progressed_at = now_ms();
			uint8_t completed_opcode;

			if (c->out_opcode == 0x9 && c->ping_queued && !old_offset) {
				c->ping_queued = 0;
				c->ping_outstanding = 1;
				c->ping_sent_ms = progressed_at;
				c->last_ping_ms = progressed_at;
			}
			c->out_off += (size_t)n;
			c->out_progress_ms = progressed_at;
			byte_budget -= (size_t)n;
			if (c->out_off < c->out_len)
				continue;
			completed_opcode = c->out_opcode;
			websocket_clear_active(c);
			if (completed_opcode == 0x8) {
				c->close_frame_sent = 1;
				c->close_deadline_ms = progressed_at +
					WS_CLOSE_TIMEOUT_MS;
			}
			websocket_promote_next(c, progressed_at);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
	return 0;
}

static void websocket_close(struct client *c, uint16_t code)
{
	uint8_t payload[2];

	if (c->closing)
		return;
	payload[0] = (uint8_t)(code >> 8);
	payload[1] = (uint8_t)code;
	c->closing = 1;
	/*
	 * The close timeout starts only once the frame has reached the kernel.
	 * Until then the normal bounded output-stall deadlines protect the socket.
	 */
	if (websocket_queue(c, 0x8, payload, sizeof(payload), 1) != 0)
		c->dead = 1;
}

static int websocket_protocol_error(struct client *c, uint16_t code)
{
	/* A malformed reply after our Close cannot complete the handshake. */
	if (c->closing)
		c->dead = 1;
	else
		websocket_close(c, code);
	return -1;
}

static void websocket_receive_close(struct client *c, const uint8_t *payload,
	size_t length)
{
	c->close_frame_received = 1;
	if (c->closing)
		return;
	c->closing = 1;
	/* RFC 6455 requires echoing a peer-initiated Close before TCP teardown. */
	if (websocket_queue(c, 0x8, payload, length, 1) != 0)
		c->dead = 1;
}

static int token_char(unsigned char c)
{
	static const char separators[] = "()<>@,;:\\\"/[]?={} \t";

	return c > 0x20 && c < 0x7f && strchr(separators, c) == NULL;
}

static char *trim_value(char *value)
{
	char *end;

	while (*value == ' ' || *value == '\t')
		value++;
	end = value + strlen(value);
	while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return value;
}

static int header_has_token(const char *value, const char *wanted)
{
	const char *p = value;
	size_t wanted_len = strlen(wanted);

	while (*p) {
		const char *start;
		const char *end;

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		start = p;
		while (*p && *p != ',')
			p++;
		end = p;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		if ((size_t)(end - start) == wanted_len &&
		    strncasecmp(start, wanted, wanted_len) == 0)
			return 1;
	}
	return 0;
}

static int copy_header_once(char *dst, size_t dst_size, const char *value)
{
	size_t len = strlen(value);

	if (dst[0] || len >= dst_size)
		return -1;
	memcpy(dst, value, len + 1);
	return 0;
}

static int parse_content_length_value(const char *value, size_t *result)
{
	size_t parsed = 0;
	const unsigned char *p = (const unsigned char *)value;

	if (!*p)
		return -1;
	for (; *p; p++) {
		unsigned digit;

		if (!isdigit(*p))
			return -1;
		digit = *p - '0';
		if (parsed > (SIZE_MAX - digit) / 10)
			return -1;
		parsed = parsed * 10 + digit;
	}
	*result = parsed;
	return 0;
}

static int parse_request(const uint8_t *bytes, size_t length,
	struct request_info *request)
{
	char header[HTTP_HEADER_MAX + 1];
	char *line;
	char *cursor;
	char *line_end;
	char *space1;
	char *space2;
	unsigned header_count = 0;
	unsigned seen_headers = 0;
	enum {
		SEEN_HOST = 1u << 0,
		SEEN_ORIGIN = 1u << 1,
		SEEN_AUTHORIZATION = 1u << 2,
		SEEN_CONTENT_TYPE = 1u << 3,
		SEEN_FILENAME = 1u << 4,
		SEEN_WS_KEY = 1u << 5,
		SEEN_WS_VERSION = 1u << 6,
		SEEN_PRIVATE_NETWORK = 1u << 7,
		SEEN_REQUEST_METHOD = 1u << 8,
		SEEN_REQUEST_HEADERS = 1u << 9
	};

	if (!request || length > HTTP_HEADER_MAX ||
	    memchr(bytes, '\0', length) != NULL)
		return -1;
	memcpy(header, bytes, length);
	header[length] = '\0';
	memset(request, 0, sizeof(*request));
	line_end = strstr(header, "\r\n");
	if (!line_end)
		return -1;
	*line_end = '\0';
	line = header;
	space1 = strchr(line, ' ');
	if (!space1)
		return -1;
	*space1++ = '\0';
	space2 = strchr(space1, ' ');
	if (!space2)
		return -1;
	*space2++ = '\0';
	if (!line[0] || !space1[0] ||
	    strchr(space2, ' ') || strchr(space2, '\t') ||
	    (strcmp(space2, "HTTP/1.1") != 0 && strcmp(space2, "HTTP/1.0") != 0) ||
	    strlen(line) >= sizeof(request->method) ||
	    strlen(space1) >= sizeof(request->target) || space1[0] != '/')
		return -1;
	for (cursor = line; *cursor; cursor++) {
		if (!token_char((unsigned char)*cursor))
			return -1;
	}
	if (strcmp(line, "GET") != 0 && strcmp(line, "POST") != 0 &&
	    strcmp(line, "OPTIONS") != 0)
		return -2;
	for (cursor = space1; *cursor; cursor++) {
		unsigned char c = (unsigned char)*cursor;

		if (c < 0x21 || c > 0x7e || c == '#')
			return -1;
	}
	strcpy(request->method, line);
	strcpy(request->target, space1);
	{
		const char *query = strchr(space1, '?');
		size_t path_len = query ? (size_t)(query - space1) : strlen(space1);

		if (path_len == 0 || path_len >= sizeof(request->path))
			return -1;
		memcpy(request->path, space1, path_len);
		request->path[path_len] = '\0';
	}

	cursor = line_end + 2;
	while (*cursor) {
		char *colon;
		char *name;
		char *value;
		size_t i;

		line_end = strstr(cursor, "\r\n");
		if (!line_end)
			return -1;
		if (line_end == cursor)
			break;
		if (++header_count > 100)
			return -1;
		*line_end = '\0';
		if (*cursor == ' ' || *cursor == '\t')
			return -1;
		colon = strchr(cursor, ':');
		if (!colon || colon == cursor)
			return -1;
		*colon = '\0';
		name = cursor;
		for (i = 0; name[i]; i++) {
			if (!token_char((unsigned char)name[i]))
				return -1;
		}
		value = trim_value(colon + 1);
		if (contains_ctl_or_crlf(value))
			return -1;

		if (strcasecmp(name, "Host") == 0) {
			if (seen_headers & SEEN_HOST)
				return -1;
			seen_headers |= SEEN_HOST;
			if (copy_header_once(request->host, sizeof(request->host), value) != 0)
				return -1;
		} else if (strcasecmp(name, "Origin") == 0) {
			if (seen_headers & SEEN_ORIGIN)
				return -1;
			seen_headers |= SEEN_ORIGIN;
			if (copy_header_once(request->origin, sizeof(request->origin), value) != 0)
				return -1;
		} else if (strcasecmp(name, "Authorization") == 0) {
			if (seen_headers & SEEN_AUTHORIZATION)
				return -1;
			seen_headers |= SEEN_AUTHORIZATION;
			if (copy_header_once(request->authorization,
				sizeof(request->authorization), value) != 0)
				return -1;
		} else if (strcasecmp(name, "Content-Type") == 0) {
			if (seen_headers & SEEN_CONTENT_TYPE)
				return -1;
			seen_headers |= SEEN_CONTENT_TYPE;
			if (copy_header_once(request->content_type,
				sizeof(request->content_type), value) != 0)
				return -1;
		} else if (strcasecmp(name, "X-Filename") == 0) {
			if (seen_headers & SEEN_FILENAME)
				return -1;
			seen_headers |= SEEN_FILENAME;
			if (copy_header_once(request->x_filename,
				sizeof(request->x_filename), value) != 0)
				return -1;
		} else if (strcasecmp(name, "Content-Length") == 0) {
			size_t parsed;

			if (parse_content_length_value(value, &parsed) != 0)
				return -1;
			if (request->has_content_length)
				return -1;
			request->has_content_length = 1;
			request->content_length = parsed;
		} else if (strcasecmp(name, "Transfer-Encoding") == 0) {
			request->has_transfer_encoding = 1;
		} else if (strcasecmp(name, "Connection") == 0) {
			if (header_has_token(value, "upgrade"))
				request->connection_upgrade = 1;
		} else if (strcasecmp(name, "Upgrade") == 0) {
			if (strcasecmp(value, "websocket") == 0)
				request->upgrade_websocket = 1;
		} else if (strcasecmp(name, "Sec-WebSocket-Key") == 0) {
			if (seen_headers & SEEN_WS_KEY)
				return -1;
			seen_headers |= SEEN_WS_KEY;
			if (copy_header_once(request->ws_key,
				sizeof(request->ws_key), value) != 0)
				return -1;
		} else if (strcasecmp(name, "Sec-WebSocket-Version") == 0) {
			if (seen_headers & SEEN_WS_VERSION)
				return -1;
			seen_headers |= SEEN_WS_VERSION;
			if (copy_header_once(request->ws_version,
				sizeof(request->ws_version), value) != 0)
				return -1;
		} else if (strcasecmp(name,
			"Access-Control-Request-Private-Network") == 0) {
			if (seen_headers & SEEN_PRIVATE_NETWORK)
				return -1;
			seen_headers |= SEEN_PRIVATE_NETWORK;
			if (strcasecmp(value, "true") == 0)
				request->private_network = 1;
		} else if (strcasecmp(name, "Access-Control-Request-Method") == 0) {
			if (seen_headers & SEEN_REQUEST_METHOD)
				return -1;
			seen_headers |= SEEN_REQUEST_METHOD;
			if (copy_header_once(request->requested_method,
				sizeof(request->requested_method), value) != 0)
				return -1;
		} else if (strcasecmp(name, "Access-Control-Request-Headers") == 0) {
			if (seen_headers & SEEN_REQUEST_HEADERS)
				return -1;
			seen_headers |= SEEN_REQUEST_HEADERS;
			if (copy_header_once(request->requested_headers,
				sizeof(request->requested_headers), value) != 0)
				return -1;
		}
		cursor = line_end + 2;
	}
	if (request->has_transfer_encoding)
		return -1;
	if (!request->host[0] && strstr(space2, "1.1"))
		return -1;
	return 0;
}

static ssize_t find_header_end(const uint8_t *data, size_t length)
{
	size_t i;

	for (i = 3; i < length; i++) {
		if (data[i - 3] == '\r' && data[i - 2] == '\n' &&
		    data[i - 1] == '\r' && data[i] == '\n')
			return (ssize_t)(i + 1);
	}
	return -1;
}

static int hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int url_decode_component(const char *start, size_t length,
	char *out, size_t out_size)
{
	size_t i;
	size_t o = 0;

	if (!out_size)
		return -1;
	for (i = 0; i < length; i++) {
		unsigned char c = (unsigned char)start[i];

		if (c == '%') {
			int hi;
			int lo;

			if (i + 2 >= length ||
			    (hi = hex_value((unsigned char)start[i + 1])) < 0 ||
			    (lo = hex_value((unsigned char)start[i + 2])) < 0)
				return -1;
			c = (unsigned char)((hi << 4) | lo);
			i += 2;
		} else if (c == '+') {
			c = ' ';
		}
		if (c == '\0' || c == '\r' || c == '\n' || o + 1 >= out_size)
			return -1;
		out[o++] = (char)c;
	}
	out[o] = '\0';
	return 0;
}

/* 1 = found, 0 = absent, -1 = malformed or duplicated. */
static int query_value(const char *target, const char *name,
	char *out, size_t out_size)
{
	const char *query = strchr(target, '?');
	size_t name_len = strlen(name);
	int found = 0;

	out[0] = '\0';
	if (!query)
		return 0;
	query++;
	while (*query && *query != '#') {
		const char *end = query;
		const char *equal;

		while (*end && *end != '&' && *end != '#')
			end++;
		equal = memchr(query, '=', (size_t)(end - query));
		if (equal && (size_t)(equal - query) == name_len &&
		    memcmp(query, name, name_len) == 0) {
			if (found || url_decode_component(equal + 1,
				(size_t)(end - equal - 1), out, out_size) != 0)
				return -1;
			found = 1;
		}
		query = *end == '&' ? end + 1 : end;
	}
	return found;
}

static int bearer_token(const char *authorization, char *out, size_t out_size)
{
	const char *p = authorization;
	size_t length;

	out[0] = '\0';
	if (!p || !*p)
		return 0;
	if (strncasecmp(p, "Bearer", 6) != 0 ||
	    (p[6] != ' ' && p[6] != '\t'))
		return -1;
	p += 6;
	while (*p == ' ' || *p == '\t')
		p++;
	length = strlen(p);
	while (length && (p[length - 1] == ' ' || p[length - 1] == '\t'))
		length--;
	if (!length || length >= out_size)
		return -1;
	memcpy(out, p, length);
	out[length] = '\0';
	return 1;
}

static int request_authorized(const struct nes_http *srv,
	const struct request_info *request, int websocket)
{
	char supplied[NES_HTTP_AUTH_TOKEN_MAX];
	int result;

	if (!srv->auth_token[0])
		return 1;
	result = bearer_token(request->authorization, supplied, sizeof(supplied));
	if (result == 1 && constant_time_equal(supplied, srv->auth_token))
		return 1;
	if (!websocket)
		return 0;
	result = query_value(request->target, "token", supplied, sizeof(supplied));
	return result == 1 && constant_time_equal(supplied, srv->auth_token);
}

static int request_origin_allowed(const struct nes_http *srv,
	const struct request_info *request)
{
	char expected[512];
	int n;

	if (!request->origin[0])
		return 1;
	if (request->host[0]) {
		n = snprintf(expected, sizeof(expected), "http://%s", request->host);
		if (n > 0 && (size_t)n < sizeof(expected) &&
		    constant_time_equal(request->origin, expected))
			return 1;
	}
	return srv->allowed_origin[0] &&
	       constant_time_equal(request->origin, srv->allowed_origin);
}

static int preflight_headers_allowed(const char *headers)
{
	const char *p = headers;

	while (*p) {
		const char *start;
		const char *end;
		const char *scan;
		size_t length;

		while (*p == ' ' || *p == '\t')
			p++;
		start = p;
		while (*p && *p != ',')
			p++;
		end = p;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		length = (size_t)(end - start);
		for (scan = start; scan < end; scan++) {
			if (!token_char((unsigned char)*scan))
				return 0;
		}
		if (!length ||
		    !((length == strlen("Authorization") &&
		       strncasecmp(start, "Authorization", length) == 0) ||
		      (length == strlen("Content-Type") &&
		       strncasecmp(start, "Content-Type", length) == 0) ||
		      (length == strlen("X-Filename") &&
		       strncasecmp(start, "X-Filename", length) == 0)))
			return 0;
		if (*p == ',')
			p++;
	}
	return 1;
}

static int valid_unauthenticated_preflight(const struct nes_http *srv,
	const struct request_info *request)
{
	if (strcmp(request->method, "OPTIONS") != 0 ||
	    strncmp(request->path, "/api/", 5) != 0 || !srv->allowed_origin[0] ||
	    !request->origin[0] ||
	    !constant_time_equal(request->origin, srv->allowed_origin) ||
	    (strcmp(request->requested_method, "GET") != 0 &&
	     strcmp(request->requested_method, "POST") != 0) ||
	    !preflight_headers_allowed(request->requested_headers))
		return 0;
	return !request->has_content_length || request->content_length == 0;
}

static void basename_safe(const char *path, char *out, size_t out_size)
{
	const char *base = strrchr(path ? path : "", '/');
	size_t length;

	base = base ? base + 1 : (path ? path : "");
	if (!out_size)
		return;
	length = strlen(base);
	if (length >= out_size)
		length = out_size - 1;
	memcpy(out, base, length);
	out[length] = '\0';
}

static int is_rom_ext(const char *name)
{
	const char *extension = strrchr(name ? name : "", '.');

	return extension &&
	       (strcasecmp(extension, ".nes") == 0 ||
		strcasecmp(extension, ".fds") == 0 ||
		strcasecmp(extension, ".unf") == 0 ||
		strcasecmp(extension, ".unif") == 0);
}

static int sanitize_filename(const char *input, char *out, size_t out_size)
{
	const char *base;
	const char *windows_base;
	size_t o = 0;

	if (!input || !out || out_size < 2)
		return -1;
	base = strrchr(input, '/');
	base = base ? base + 1 : input;
	windows_base = strrchr(base, '\\');
	if (windows_base)
		base = windows_base + 1;
	while (*base) {
		unsigned char c = (unsigned char)*base++;

		if (o + 1 >= out_size)
			return -1;
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		    c == '.' || c == '-' || c == '_' || c == ' ')
			out[o++] = (char)c;
		else
			out[o++] = '_';
	}
	out[o] = '\0';
	if (!o || strcmp(out, ".") == 0 || strcmp(out, "..") == 0 ||
	    out[0] == '.' || !is_rom_ext(out))
		return -1;
	return 0;
}

static int rom_fd_valid(int fd, const char *name, uint64_t max_size,
	uint64_t expected_size)
{
	struct stat st;
	uint8_t magic[4];
	ssize_t got;
	const char *extension = strrchr(name, '.');
	uint64_t size;

	if (!extension || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size <= 0)
		return 0;
	size = (uint64_t)st.st_size;
	if (size > max_size || (expected_size && size != expected_size))
		return 0;
	got = pread(fd, magic, sizeof(magic), 0);
	if (got != (ssize_t)sizeof(magic))
		return 0;
	if (strcasecmp(extension, ".nes") == 0)
		return size >= 16 && memcmp(magic, "NES\x1a", 4) == 0;
	if (strcasecmp(extension, ".unf") == 0 ||
	    strcasecmp(extension, ".unif") == 0)
		return size >= 32 && memcmp(magic, "UNIF", 4) == 0;
	if (strcasecmp(extension, ".fds") == 0) {
		if (memcmp(magic, "FDS\x1a", 4) == 0)
			return size >= 16 + 65500 && (size - 16) % 65500 == 0;
		return size % 65500 == 0;
	}
	return 0;
}

static int path_under_root(const char *path, const char *root)
{
	size_t root_length;

	if (!path || !root || !root[0])
		return 0;
	root_length = strlen(root);
	if (strncmp(path, root, root_length) != 0)
		return 0;
	return path[root_length] == '\0' || path[root_length] == '/';
}

static int canonical_path_allowed(const struct nes_http *srv, const char *path,
	char *canonical, size_t canonical_size)
{
	char resolved[PATH_MAX];
	int i;

	if (!realpath(path, resolved) || strlen(resolved) >= canonical_size)
		return 0;
	for (i = 0; i < srv->rom_root_count; i++) {
		if (path_under_root(resolved, srv->rom_roots[i])) {
			strcpy(canonical, resolved);
			return 1;
		}
	}
	return 0;
}

static int validate_rom_path(const struct nes_http *srv, const char *path,
	char *canonical, size_t canonical_size)
{
	int fd;
	int valid;

	if (!is_rom_ext(path) ||
	    !canonical_path_allowed(srv, path, canonical, canonical_size))
		return -1;
	fd = open(canonical, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM)
			return -2;
		return -1;
	}
	valid = rom_fd_valid(fd, canonical, srv->max_upload_bytes, 0);
	close(fd);
	return valid ? 0 : -1;
}

static int resolve_rom_path(const struct nes_http *srv, const char *requested,
	char *full, size_t full_size)
{
	char candidate[PATH_MAX];
	int unreadable = 0;
	int i;
	int n;

	if (!requested || !requested[0] || !is_rom_ext(requested))
		return -1;
	if (requested[0] == '/')
		return validate_rom_path(srv, requested, full, full_size);
	if (strchr(requested, '\\') || strstr(requested, "../") ||
	    strcmp(requested, "..") == 0 || requested[0] == '/')
		return -1;
	n = snprintf(candidate, sizeof(candidate), "%s/%s",
		srv->rom_dir, requested);
	if (n > 0 && (size_t)n < sizeof(candidate)) {
		int result = validate_rom_path(srv, candidate, full, full_size);

		if (result == 0)
			return 0;
		unreadable = result == -2;
	}
	for (i = 0; i < srv->rom_root_count; i++) {
		int n = snprintf(candidate, sizeof(candidate), "%s/%s",
			srv->rom_roots[i], requested);

		if (n > 0 && (size_t)n < sizeof(candidate)) {
			int result = validate_rom_path(srv, candidate, full, full_size);

			if (result == 0)
				return 0;
			if (result == -2)
				unreadable = 1;
		}
	}
	return unreadable ? -2 : -1;
}

static int directory_usage(const char *path, int depth, unsigned *entries,
	uint64_t *total)
{
	DIR *directory;
	struct dirent *entry;

	if (depth > MAX_SCAN_DEPTH || *entries >= MAX_SCAN_ENTRIES) {
		errno = EOVERFLOW;
		return -1;
	}
	directory = opendir(path);
	if (!directory)
		return -1;
	while ((entry = readdir(directory)) != NULL) {
		char child[PATH_MAX];
		struct stat st;
		int n;

		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (++*entries > MAX_SCAN_ENTRIES) {
			closedir(directory);
			errno = EOVERFLOW;
			return -1;
		}
		n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(child)) {
			closedir(directory);
			errno = ENAMETOOLONG;
			return -1;
		}
		if (lstat(child, &st) != 0) {
			int saved_errno = errno;

			closedir(directory);
			errno = saved_errno;
			return -1;
		}
		if (S_ISDIR(st.st_mode)) {
			if (directory_usage(child, depth + 1, entries, total) != 0) {
				int saved_errno = errno;

				closedir(directory);
				errno = saved_errno;
				return -1;
			}
		} else if (S_ISREG(st.st_mode)) {
			uint64_t size = st.st_size > 0 ? (uint64_t)st.st_size : 0;

			if (*total > UINT64_MAX - size)
				*total = UINT64_MAX;
			else
				*total += size;
		}
	}
	closedir(directory);
	return 0;
}

static int storage_available(const struct nes_http *srv, const char *final_path,
	uint64_t upload_size)
{
	struct statvfs fs;
	struct stat existing;
	unsigned entries = 0;
	uint64_t used = 0;
	uint64_t replacing = 0;
	uint64_t required;
	uint64_t free_bytes;

	if (statvfs(srv->rom_dir, &fs) != 0)
		return -1;
	if (fs.f_frsize && (uint64_t)fs.f_bavail > UINT64_MAX / fs.f_frsize)
		free_bytes = UINT64_MAX;
	else
		free_bytes = (uint64_t)fs.f_bavail * fs.f_frsize;
	if (upload_size > UINT64_MAX - srv->min_free_bytes ||
	    free_bytes < upload_size + srv->min_free_bytes) {
		errno = ENOSPC;
		return -1;
	}
	if (!srv->rom_quota_bytes)
		return 0;
	if (directory_usage(srv->rom_dir, 0, &entries, &used) != 0)
		return -1;
	if (lstat(final_path, &existing) == 0) {
		if (!S_ISREG(existing.st_mode)) {
			errno = EINVAL;
			return -1;
		}
		replacing = existing.st_size > 0 ? (uint64_t)existing.st_size : 0;
	}
	required = upload_size > replacing ? upload_size - replacing : 0;
	if (used > srv->rom_quota_bytes ||
	    required > srv->rom_quota_bytes - used) {
		errno = EDQUOT;
		return -1;
	}
	return 0;
}

static int storage_commit_allowed(const struct nes_http *srv,
	const char *final_path)
{
	struct statvfs fs;
	struct stat existing;
	unsigned entries = 0;
	uint64_t used = 0;
	uint64_t replacing = 0;
	uint64_t projected;
	uint64_t free_bytes;

	if (statvfs(srv->rom_dir, &fs) != 0)
		return -1;
	if (fs.f_frsize && (uint64_t)fs.f_bavail > UINT64_MAX / fs.f_frsize)
		free_bytes = UINT64_MAX;
	else
		free_bytes = (uint64_t)fs.f_bavail * fs.f_frsize;
	if (free_bytes < srv->min_free_bytes) {
		errno = ENOSPC;
		return -1;
	}
	if (!srv->rom_quota_bytes)
		return 0;
	if (directory_usage(srv->rom_dir, 0, &entries, &used) != 0)
		return -1;
	if (lstat(final_path, &existing) == 0) {
		if (!S_ISREG(existing.st_mode)) {
			errno = EINVAL;
			return -1;
		}
		replacing = existing.st_size > 0 ? (uint64_t)existing.st_size : 0;
	}
	projected = used >= replacing ? used - replacing : 0;
	if (projected > srv->rom_quota_bytes) {
		errno = EDQUOT;
		return -1;
	}
	return 0;
}

static int broad_directory(const char *path)
{
	static const char *const denied[] = {
		"/", "/bin", "/boot", "/dev", "/etc", "/home", "/lib",
		"/lib64", "/mnt", "/opt", "/overlay", "/proc", "/root",
		"/run", "/sbin", "/sys", "/tmp", "/usr", "/var", NULL
	};
	int i;

	for (i = 0; denied[i]; i++) {
		if (strcmp(path, denied[i]) == 0)
			return 1;
	}
	return 0;
}

static int mkdir_tree(const char *path)
{
	char copy[PATH_MAX];
	size_t i;

	if (!path || path[0] != '/' || strlen(path) >= sizeof(copy) ||
	    strstr(path, "/../") || strstr(path, "/./") ||
	    strcmp(path, "/..") == 0 || broad_directory(path)) {
		errno = EINVAL;
		return -1;
	}
	strcpy(copy, path);
	for (i = 1; copy[i]; i++) {
		if (copy[i] != '/')
			continue;
		copy[i] = '\0';
		if (mkdir(copy, 0750) != 0 && errno != EEXIST)
			return -1;
		copy[i] = '/';
	}
	if (mkdir(copy, 0750) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int add_root(struct nes_http *srv, const char *directory)
{
	char resolved[PATH_MAX];
	struct stat st;
	int i;

	if (!directory || !directory[0] ||
	    srv->rom_root_count >= NES_MAX_ROM_ROOTS ||
	    !realpath(directory, resolved) || broad_directory(resolved) ||
	    !utf8_valid_string(resolved) ||
	    stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode) ||
	    strlen(resolved) >= sizeof(srv->rom_roots[0]))
		return -1;
	for (i = 0; i < srv->rom_root_count; i++) {
		if (strcmp(srv->rom_roots[i], resolved) == 0)
			return 0;
	}
	strcpy(srv->rom_roots[srv->rom_root_count++], resolved);
	return 0;
}

static int append_rom_item(struct strbuf *json, int *first,
	const char *root, const char *path, off_t size, int readable,
	const char *error)
{
	struct strbuf item;
	char name[256];
	size_t limit = json->max > ROM_JSON_TAIL_RESERVE
		? json->max - ROM_JSON_TAIL_RESERVE : 0;
	int result = -1;

	if (sb_init(&item, 512, 32u * 1024u) != 0)
		return -1;
	basename_safe(path, name, sizeof(name));
	if (sb_append(&item, *first ? "" : ",") == 0 &&
	    sb_append(&item, "{\"name\":") == 0 &&
	    sb_append_json_string(&item, name) == 0 &&
	    sb_append(&item, ",\"path\":") == 0 &&
	    sb_append_json_string(&item, path) == 0 &&
	    sb_append(&item, ",\"source\":") == 0 &&
	    sb_append_json_string(&item, root) == 0 &&
	    sb_appendf(&item, ",\"size\":%lld,\"readable\":%s",
		    (long long)size, readable ? "true" : "false") == 0 &&
	    (!error || (sb_append(&item, ",\"error\":") == 0 &&
		    sb_append_json_string(&item, error) == 0)) &&
	    sb_append(&item, "}") == 0 &&
	    item.len <= limit && json->len <= limit - item.len &&
	    sb_append_mem(json, item.data, item.len) == 0) {
		*first = 0;
		result = 0;
	}
	sb_free(&item);
	return result;
}

static void scan_roms(struct nes_http *srv, struct strbuf *json, int *first,
	const char *root, const char *relative, int depth, unsigned *entries,
	int *truncated, uint64_t deadline_ns)
{
	char directory_path[PATH_MAX];
	DIR *directory;
	struct dirent *entry;
	int n;

	if (*truncated || depth > MAX_SCAN_DEPTH || *entries >= MAX_SCAN_ENTRIES ||
	    (deadline_ns && now_ns() >= deadline_ns)) {
		*truncated = 1;
		return;
	}
	n = relative[0] ? snprintf(directory_path, sizeof(directory_path), "%s/%s",
		root, relative) : snprintf(directory_path, sizeof(directory_path), "%s", root);
	if (n < 0 || (size_t)n >= sizeof(directory_path)) {
		*truncated = 1;
		return;
	}
	directory = opendir(directory_path);
	if (!directory)
		return;
	while ((entry = readdir(directory)) != NULL) {
		char child_relative[PATH_MAX];
		char full[PATH_MAX];
		struct stat st;

		if (deadline_ns && now_ns() >= deadline_ns) {
			*truncated = 1;
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0 ||
		    entry->d_name[0] == '.' ||
		    !utf8_valid_string(entry->d_name))
			continue;
		if (++*entries > MAX_SCAN_ENTRIES) {
			*truncated = 1;
			break;
		}
		n = relative[0] ? snprintf(child_relative, sizeof(child_relative),
			"%s/%s", relative, entry->d_name) :
			snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(child_relative))
			continue;
		n = snprintf(full, sizeof(full), "%s/%s", root, child_relative);
		if (n < 0 || (size_t)n >= sizeof(full) || lstat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			scan_roms(srv, json, first, root, child_relative,
				depth + 1, entries, truncated, deadline_ns);
		} else if (S_ISREG(st.st_mode) && is_rom_ext(entry->d_name)) {
			int fd = open(full, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			int open_error = fd < 0 ? errno : 0;
			int valid = fd >= 0 &&
				rom_fd_valid(fd, full, srv->max_upload_bytes, 0);

			if (fd >= 0)
				close(fd);
			if (valid && append_rom_item(json, first, root, full,
				st.st_size, 1, NULL) != 0) {
				*truncated = 1;
				break;
			}
			if (!valid && fd < 0 &&
			    (open_error == EACCES || open_error == EPERM) &&
			    append_rom_item(json, first, root, full, st.st_size, 0,
				    "ROM is not readable by nesd; upload it through "
				    "LuCI or set group nesd and mode 0640") != 0) {
				*truncated = 1;
				break;
			}
		}
	}
	closedir(directory);
}

static char *make_status_json(struct nes_http *srv, const char *event)
{
	struct strbuf sb;
	struct nes_host_status status;
	char rom_name[256];

	host_get_status(srv->host, &status);

	if (sb_init(&sb, 512, 32u * 1024u) != 0)
		return NULL;
	basename_safe(status.rom_path, rom_name, sizeof(rom_name));
	if (sb_append(&sb, "{\"t\":\"status\",\"event\":") != 0 ||
	    sb_append_json_string(&sb, event ? event : "status") != 0 ||
	    sb_appendf(&sb,
		",\"running\":%s,\"paused\":%s,\"game_loaded\":%s,"
		"\"viewers\":%d,"
		"\"core_loaded\":%s,\"demo\":%s,\"core\":",
		status.running ? "true" : "false",
		status.paused ? "true" : "false",
		status.game_loaded ? "true" : "false",
		status.viewers,
		status.core_loaded ? "true" : "false",
		status.demo_mode ? "true" : "false") != 0 ||
	    sb_append_json_string(&sb, status.library_name) != 0 ||
	    sb_append(&sb, ",\"core_version\":") != 0 ||
	    sb_append_json_string(&sb, status.library_version) != 0 ||
	    sb_append(&sb, ",\"rom\":") != 0 ||
	    sb_append_json_string(&sb, rom_name) != 0 ||
	    sb_append(&sb, ",\"rom_path\":") != 0 ||
	    sb_append_json_string(&sb, status.rom_path) != 0 ||
	    sb_appendf(&sb,
		",\"width\":%u,\"height\":%u,\"fps\":%.2f,\"sample_rate\":%.0f,"
		"\"frame_id\":%llu,\"render\":\"software\","
		"\"stream_format\":\"%s\",\"jpeg_quality\":%d,\"stream_fps\":%d,"
		"\"show_fps\":%s,\"show_touch_controls\":%s,"
		"\"architecture\":\"router-cpu-thin-client\"}",
		status.width, status.height, status.fps,
		status.sample_rate > 1.0 ? status.sample_rate : 48000.0,
		(unsigned long long)status.frame_id,
		srv->stream.use_jpeg ? "jpeg" : "rgb565",
		srv->stream.jpeg_quality, srv->stream.stream_fps,
		srv->stream.show_fps ? "true" : "false",
		srv->stream.show_touch_controls ? "true" : "false") != 0) {
		sb_free(&sb);
		return NULL;
	}
	return sb.data;
}

static void handle_status(struct nes_http *srv, struct client *c)
{
	char *json = make_status_json(srv, "status");

	if (!json) {
		http_json(srv, c, 500, "{\"error\":\"out of memory\"}");
		return;
	}
	http_json(srv, c, 200, json);
	free(json);
}

static void handle_roms(struct nes_http *srv, struct client *c)
{
	struct nes_host_status status;
	struct strbuf json;
	uint64_t scan_started;
	uint64_t scan_budget;
	uint64_t scan_deadline = 0;
	int first = 1;
	int truncated = 0;
	unsigned entries = 0;
	int i;

	host_get_status(srv->host, &status);
	scan_budget = status.viewers ? ROM_SCAN_ACTIVE_BUDGET_NS :
		ROM_SCAN_IDLE_BUDGET_NS;
	scan_started = now_ns();
	if (scan_started)
		scan_deadline = scan_started > UINT64_MAX - scan_budget ?
			UINT64_MAX : scan_started + scan_budget;

	if (sb_init(&json, 8192, MAX_ROM_JSON) != 0) {
		http_json(srv, c, 500, "{\"error\":\"out of memory\"}");
		return;
	}
	(void)sb_append(&json, "{\"roms\":[");
	for (i = 0; i < srv->rom_root_count && !truncated; i++)
		scan_roms(srv, &json, &first, srv->rom_roots[i], "", 0,
			&entries, &truncated, scan_deadline);
	if (sb_append(&json, "],\"dir\":") != 0 ||
	    sb_append_json_string(&json, srv->rom_dir) != 0 ||
	    sb_append(&json, ",\"roots\":[") != 0) {
		sb_free(&json);
		http_json(srv, c, 500, "{\"error\":\"ROM list too large\"}");
		return;
	}
	for (i = 0; i < srv->rom_root_count; i++) {
		if ((i && sb_append(&json, ",") != 0) ||
		    sb_append_json_string(&json, srv->rom_roots[i]) != 0) {
			sb_free(&json);
			http_json(srv, c, 500, "{\"error\":\"ROM list too large\"}");
			return;
		}
	}
	if (sb_appendf(&json, "],\"truncated\":%s}", truncated ? "true" : "false") != 0) {
		sb_free(&json);
		http_json(srv, c, 500, "{\"error\":\"ROM list too large\"}");
		return;
	}
	http_json(srv, c, 200, json.data);
	sb_free(&json);
}

static const char *json_skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

struct json_cursor {
	const unsigned char *p;
	const unsigned char *end;
};

static void json_validate_skip_ws(struct json_cursor *cursor)
{
	while (cursor->p < cursor->end &&
	       (*cursor->p == ' ' || *cursor->p == '\t' ||
		*cursor->p == '\r' || *cursor->p == '\n'))
		cursor->p++;
}

static int json_validate_hex4(struct json_cursor *cursor, uint32_t *value)
{
	uint32_t parsed = 0;
	int i;

	if ((size_t)(cursor->end - cursor->p) < 4)
		return -1;
	for (i = 0; i < 4; i++) {
		int digit = hex_value(cursor->p[i]);

		if (digit < 0)
			return -1;
		parsed = (parsed << 4) | (uint32_t)digit;
	}
	cursor->p += 4;
	*value = parsed;
	return 0;
}

static int json_validate_string_token(struct json_cursor *cursor)
{
	if (cursor->p >= cursor->end || *cursor->p++ != '"')
		return -1;
	while (cursor->p < cursor->end) {
		unsigned char c = *cursor->p++;

		if (c == '"')
			return 0;
		if (c == '\\') {
			uint32_t high;

			if (cursor->p >= cursor->end)
				return -1;
			c = *cursor->p++;
			if (c == '"' || c == '\\' || c == '/' ||
			    c == 'b' || c == 'f' || c == 'n' ||
			    c == 'r' || c == 't')
				continue;
			if (c != 'u' ||
			    json_validate_hex4(cursor, &high) != 0)
				return -1;
			if (high >= 0xd800 && high <= 0xdbff) {
				uint32_t low;

				if ((size_t)(cursor->end - cursor->p) < 6 ||
				    cursor->p[0] != '\\' ||
				    cursor->p[1] != 'u')
					return -1;
				cursor->p += 2;
				if (json_validate_hex4(cursor, &low) != 0 ||
				    low < 0xdc00 || low > 0xdfff)
					return -1;
			} else if (high >= 0xdc00 && high <= 0xdfff) {
				return -1;
			}
			continue;
		}
		if (c < 0x20)
			return -1;
		if (c >= 0x80) {
			size_t remaining = (size_t)(cursor->end - (cursor->p - 1));
			size_t expected;
			size_t actual;

			if ((c & 0xe0) == 0xc0)
				expected = 2;
			else if ((c & 0xf0) == 0xe0)
				expected = 3;
			else if ((c & 0xf8) == 0xf0)
				expected = 4;
			else
				return -1;
			if (remaining < expected)
				return -1;
			actual = utf8_sequence_length(cursor->p - 1);
			if (actual != expected)
				return -1;
			cursor->p += expected - 1;
		}
	}
	return -1;
}

static int json_validate_value(struct json_cursor *cursor, unsigned depth);

static int json_validate_array(struct json_cursor *cursor, unsigned depth)
{
	if (cursor->p >= cursor->end || *cursor->p++ != '[')
		return -1;
	json_validate_skip_ws(cursor);
	if (cursor->p < cursor->end && *cursor->p == ']') {
		cursor->p++;
		return 0;
	}
	for (;;) {
		if (json_validate_value(cursor, depth + 1) != 0)
			return -1;
		json_validate_skip_ws(cursor);
		if (cursor->p >= cursor->end)
			return -1;
		if (*cursor->p == ']') {
			cursor->p++;
			return 0;
		}
		if (*cursor->p++ != ',')
			return -1;
		json_validate_skip_ws(cursor);
	}
}

static int json_validate_object_token(struct json_cursor *cursor,
	unsigned depth)
{
	if (cursor->p >= cursor->end || *cursor->p++ != '{')
		return -1;
	json_validate_skip_ws(cursor);
	if (cursor->p < cursor->end && *cursor->p == '}') {
		cursor->p++;
		return 0;
	}
	for (;;) {
		if (json_validate_string_token(cursor) != 0)
			return -1;
		json_validate_skip_ws(cursor);
		if (cursor->p >= cursor->end || *cursor->p++ != ':')
			return -1;
		json_validate_skip_ws(cursor);
		if (json_validate_value(cursor, depth + 1) != 0)
			return -1;
		json_validate_skip_ws(cursor);
		if (cursor->p >= cursor->end)
			return -1;
		if (*cursor->p == '}') {
			cursor->p++;
			return 0;
		}
		if (*cursor->p++ != ',')
			return -1;
		json_validate_skip_ws(cursor);
	}
}

static int json_validate_number(struct json_cursor *cursor)
{
	const unsigned char *p = cursor->p;

	if (p < cursor->end && *p == '-')
		p++;
	if (p >= cursor->end)
		return -1;
	if (*p == '0') {
		p++;
		if (p < cursor->end && isdigit(*p))
			return -1;
	} else {
		if (*p < '1' || *p > '9')
			return -1;
		do {
			p++;
		} while (p < cursor->end && isdigit(*p));
	}
	if (p < cursor->end && *p == '.') {
		p++;
		if (p >= cursor->end || !isdigit(*p))
			return -1;
		do {
			p++;
		} while (p < cursor->end && isdigit(*p));
	}
	if (p < cursor->end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < cursor->end && (*p == '+' || *p == '-'))
			p++;
		if (p >= cursor->end || !isdigit(*p))
			return -1;
		do {
			p++;
		} while (p < cursor->end && isdigit(*p));
	}
	cursor->p = p;
	return 0;
}

static int json_validate_value(struct json_cursor *cursor, unsigned depth)
{
	size_t remaining;

	if (depth > 16 || cursor->p >= cursor->end)
		return -1;
	switch (*cursor->p) {
	case '"':
		return json_validate_string_token(cursor);
	case '{':
		return json_validate_object_token(cursor, depth);
	case '[':
		return json_validate_array(cursor, depth);
	case 't':
		remaining = (size_t)(cursor->end - cursor->p);
		if (remaining < 4 || memcmp(cursor->p, "true", 4) != 0)
			return -1;
		cursor->p += 4;
		return 0;
	case 'f':
		remaining = (size_t)(cursor->end - cursor->p);
		if (remaining < 5 || memcmp(cursor->p, "false", 5) != 0)
			return -1;
		cursor->p += 5;
		return 0;
	case 'n':
		remaining = (size_t)(cursor->end - cursor->p);
		if (remaining < 4 || memcmp(cursor->p, "null", 4) != 0)
			return -1;
		cursor->p += 4;
		return 0;
	default:
		return json_validate_number(cursor);
	}
}

static int json_validate_object(const char *json, size_t length)
{
	struct json_cursor cursor;

	if (!json)
		return 0;
	cursor.p = (const unsigned char *)json;
	cursor.end = cursor.p + length;
	json_validate_skip_ws(&cursor);
	if (json_validate_object_token(&cursor, 0) != 0)
		return 0;
	json_validate_skip_ws(&cursor);
	return cursor.p == cursor.end;
}

static int json_decode_string(const char **cursor, char *out, size_t out_size)
{
	const unsigned char *p = (const unsigned char *)*cursor;
	size_t o = 0;

	if (*p++ != '"' || !out_size)
		return -1;
	while (*p && *p != '"') {
		unsigned char c = *p++;

		if (c == '\\') {
			uint32_t value = 0;
			char encoded[4];
			size_t encoded_length;
			int i;

			c = *p++;
			switch (c) {
			case '"': case '\\': case '/': break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u':
				for (i = 0; i < 4; i++) {
					int h = hex_value(*p++);
					if (h < 0)
						return -1;
					value = (value << 4) | (unsigned)h;
				}
				if (value >= 0xd800 && value <= 0xdbff) {
					uint32_t low = 0;

					if (p[0] != '\\' || p[1] != 'u')
						return -1;
					p += 2;
					for (i = 0; i < 4; i++) {
						int h = hex_value(*p++);

						if (h < 0)
							return -1;
						low = (low << 4) | (unsigned)h;
					}
					if (low < 0xdc00 || low > 0xdfff)
						return -1;
					value = 0x10000u +
						((value - 0xd800u) << 10) +
						(low - 0xdc00u);
				} else if (value >= 0xdc00 && value <= 0xdfff) {
					return -1;
				}
				if (value < 0x20 || value == 0x7f)
					return -1;
				encoded_length = utf8_encode(value, encoded);
				if (!encoded_length || o + encoded_length >= out_size)
					return -1;
				memcpy(out + o, encoded, encoded_length);
				o += encoded_length;
				continue;
			default:
				return -1;
			}
			if (c < 0x20 || c == 0x7f || o + 1 >= out_size)
				return -1;
			out[o++] = (char)c;
		} else if (c < 0x80) {
			if (c < 0x20 || c == 0x7f || o + 1 >= out_size)
				return -1;
			out[o++] = (char)c;
		} else {
			const unsigned char *start = p - 1;
			size_t length = utf8_sequence_length(start);

			if (!length || o + length >= out_size)
				return -1;
			memcpy(out + o, start, length);
			o += length;
			p = start + length;
		}
	}
	if (*p != '"')
		return -1;
	out[o] = '\0';
	*cursor = (const char *)(p + 1);
	return 0;
}

static const char *json_skip_value(const char *p)
{
	int depth = 0;
	int in_string = 0;
	int escaped = 0;

	for (; *p; p++) {
		if (in_string) {
			if (escaped)
				escaped = 0;
			else if (*p == '\\')
				escaped = 1;
			else if (*p == '"')
				in_string = 0;
			continue;
		}
		if (*p == '"') {
			in_string = 1;
		} else if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			if (depth == 0)
				return p;
			depth--;
		} else if (*p == ',' && depth == 0) {
			return p;
		}
	}
	return p;
}

static int json_member(const char *json, const char *wanted, const char **value)
{
	const char *p = json_skip_ws(json ? json : "");
	const char *found_value = NULL;
	int found = 0;

	if (*p++ != '{')
		return 0;
	for (;;) {
		char key[80];

		p = json_skip_ws(p);
		if (*p == '}') {
			if (found)
				*value = found_value;
			return found;
		}
		if (json_decode_string(&p, key, sizeof(key)) != 0)
			return 0;
		p = json_skip_ws(p);
		if (*p++ != ':')
			return 0;
		p = json_skip_ws(p);
		if (strcmp(key, wanted) == 0) {
			if (found)
				return -1;
			found = 1;
			found_value = p;
		}
		p = json_skip_value(p);
		p = json_skip_ws(p);
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}') {
			if (found)
				*value = found_value;
			return found;
		}
		return 0;
	}
}

static int json_get_string(const char *json, const char *key,
	char *out, size_t out_size)
{
	const char *value;
	int member;

	out[0] = '\0';
	member = json_member(json, key, &value);
	if (member <= 0)
		return member;
	if (json_decode_string(&value, out, out_size) != 0)
		return -1;
	value = json_skip_ws(value);
	return *value == ',' || *value == '}' ? 1 : -1;
}

static int json_get_uint(const char *json, const char *key, uint64_t *out)
{
	const char *value;
	uint64_t parsed = 0;
	int member;

	member = json_member(json, key, &value);
	if (member <= 0)
		return member;
	if (!isdigit((unsigned char)*value))
		return -1;
	while (isdigit((unsigned char)*value)) {
		unsigned digit = (unsigned)(*value++ - '0');

		if (parsed > (UINT64_MAX - digit) / 10)
			return -1;
		parsed = parsed * 10 + digit;
	}
	value = json_skip_ws(value);
	if (*value != ',' && *value != '}')
		return -1;
	*out = parsed;
	return 1;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
	const char *value;
	int member;

	member = json_member(json, key, &value);
	if (member <= 0)
		return member;
	if (strncmp(value, "true", 4) == 0) {
		value = json_skip_ws(value + 4);
		if (*value != ',' && *value != '}')
			return -1;
		*out = 1;
		return 1;
	}
	if (strncmp(value, "false", 5) == 0) {
		value = json_skip_ws(value + 5);
		if (*value != ',' && *value != '}')
			return -1;
		*out = 0;
		return 1;
	}
	return -1;
}

static void recompute_inputs(struct nes_http *srv, struct client *clients,
	int client_count)
{
	uint16_t masks[2] = { 0, 0 };
	int i;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0 && !clients[i].dead && !clients[i].closing &&
		    clients[i].phase == CLIENT_WEBSOCKET) {
			masks[0] |= clients[i].joy[0];
			masks[1] |= clients[i].joy[1];
		}
	}
	if (atomic_load_explicit(&srv->host->joy[0], memory_order_relaxed) !=
	    masks[0])
		host_set_joy_mask(srv->host, 0, masks[0]);
	if (atomic_load_explicit(&srv->host->joy[1], memory_order_relaxed) !=
	    masks[1])
		host_set_joy_mask(srv->host, 1, masks[1]);
}

static int expire_input_leases(struct client *clients, int client_count,
	uint64_t now)
{
	int changed = 0;
	int i;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd < 0 || clients[i].dead || clients[i].closing ||
		    clients[i].phase != CLIENT_WEBSOCKET ||
		    (!clients[i].joy[0] && !clients[i].joy[1]) ||
		    now - clients[i].last_input_ms < WS_INPUT_LEASE_MS)
			continue;
		clients[i].joy[0] = 0;
		clients[i].joy[1] = 0;
		changed = 1;
	}
	return changed;
}

static int apply_input_json(struct nes_http *srv, struct client *client,
	struct client *clients, int client_count, const char *json,
	size_t json_length)
{
	uint64_t port = 0;
	uint64_t mask = 0;
	uint64_t id = 0;
	int pressed = 0;
	int has_port;
	int has_mask;
	int has_id;
	int has_pressed;
	uint16_t *target;

	if (!json_validate_object(json, json_length))
		return -1;
	has_port = json_get_uint(json, "port", &port);
	has_mask = json_get_uint(json, "mask", &mask);
	has_id = json_get_uint(json, "id", &id);
	has_pressed = json_get_bool(json, "pressed", &pressed);
	if (has_port < 0 || has_mask < 0 || has_id < 0 || has_pressed < 0 ||
	    port > 1)
		return -1;
	if (!client)
		return -1;
	target = client->joy;
	if (has_mask == 1) {
		if (mask > 0xffff)
			return -1;
		target[port] = (uint16_t)mask;
	} else if (has_id == 1 && has_pressed == 1) {
		if (id >= NES_JOY_BITS)
			return -1;
		if (pressed)
			target[port] |= (uint16_t)(1u << id);
		else
			target[port] &= (uint16_t)~(1u << id);
	} else {
		return -1;
	}
	client->last_input_ms = now_ms();
	recompute_inputs(srv, clients, client_count);
	return 0;
}

static int utf8_valid(const uint8_t *data, size_t length)
{
	size_t i = 0;

	while (i < length) {
		uint8_t c = data[i++];
		unsigned need;
		uint32_t value;
		uint32_t minimum;

		if (c < 0x80)
			continue;
		if ((c & 0xe0) == 0xc0) {
			need = 1; value = c & 0x1f; minimum = 0x80;
		} else if ((c & 0xf0) == 0xe0) {
			need = 2; value = c & 0x0f; minimum = 0x800;
		} else if ((c & 0xf8) == 0xf0) {
			need = 3; value = c & 0x07; minimum = 0x10000;
		} else {
			return 0;
		}
		if (need > length - i)
			return 0;
		while (need--) {
			uint8_t continuation = data[i++];

			if ((continuation & 0xc0) != 0x80)
				return 0;
			value = (value << 6) | (continuation & 0x3f);
		}
		if (value < minimum || value > 0x10ffff ||
		    (value >= 0xd800 && value <= 0xdfff))
			return 0;
	}
	return 1;
}

static int websocket_message(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count, const uint8_t *message,
	size_t length)
{
	char text[WS_MESSAGE_MAX + 1];
	char type[32];
	int type_result;

	if (length > WS_MESSAGE_MAX || !utf8_valid(message, length))
		return -1;
	memcpy(text, message, length);
	text[length] = '\0';
	if (!json_validate_object(text, length))
		return -1;
	type[0] = '\0';
	type_result = json_get_string(text, "t", type, sizeof(type));
	if (type_result < 0)
		return -1;
	if (strcmp(type, "hello") == 0) {
		char *status = make_status_json(srv, "connected");

		if (!status)
			return -1;
		/*
		 * The bundled browser opts into the application-level liveness lease
		 * with hello. Generic WebSocket clients remain compatible until they do.
		 */
		c->application_heartbeat_required = 1;
		c->last_input_ms = now_ms();
		(void)websocket_queue(c, 0x1, status, strlen(status), 1);
		free(status);
		return 0;
	}
	if (strcmp(type, "input") == 0)
		return apply_input_json(srv, c, clients, client_count, text,
			length);
	if (strcmp(type, "heartbeat") == 0) {
		uint64_t heartbeat_mask = 0;
		uint64_t sequence = 0;

		/* The heartbeat is also a complete controller-state renewal. */
		if (json_get_uint(text, "mask", &heartbeat_mask) != 1 ||
		    heartbeat_mask > 0xffff ||
		    json_get_uint(text, "seq", &sequence) != 1 || sequence == 0 ||
		    sequence > WS_HEARTBEAT_SEQ_MAX ||
		    sequence <= c->last_heartbeat_seq)
			return -1;
		if (apply_input_json(srv, c, clients, client_count, text,
		    length) != 0)
			return -1;
		c->last_heartbeat_seq = sequence;
		if (websocket_queue_heartbeat(c, sequence) < 0)
			return -1;
		return 0;
	}
	return 0;
}

static int close_code_valid(uint16_t code)
{
	if (code < 1000 || code >= 5000 ||
	    code == 1004 || code == 1005 || code == 1006 || code == 1015)
		return 0;
	return !(code >= 1016 && code <= 2999);
}

static int websocket_process(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	while (c->ws_in_len >= 2 && !c->dead) {
		uint8_t first = c->ws_in[0];
		uint8_t second = c->ws_in[1];
		uint8_t opcode = first & 0x0f;
		int fin = !!(first & 0x80);
		uint64_t payload_length = second & 0x7f;
		size_t header_length = 2;
		size_t frame_length;
		uint8_t *mask;
		uint8_t *payload;
		size_t i;

		if ((first & 0x70) || !(second & 0x80)) {
			return websocket_protocol_error(c, 1002);
		}
		if (payload_length == 126) {
			if (c->ws_in_len < 4)
				return 0;
			payload_length = ((uint64_t)c->ws_in[2] << 8) | c->ws_in[3];
			header_length = 4;
			if (payload_length < 126) {
				return websocket_protocol_error(c, 1002);
			}
		} else if (payload_length == 127) {
			if (c->ws_in_len < 10)
				return 0;
			if (c->ws_in[2] & 0x80) {
				return websocket_protocol_error(c, 1002);
			}
			payload_length = 0;
			for (i = 0; i < 8; i++)
				payload_length = (payload_length << 8) | c->ws_in[2 + i];
			header_length = 10;
			if (payload_length <= 0xffff) {
				return websocket_protocol_error(c, 1002);
			}
		}
		if (opcode >= 0x8 && (!fin || payload_length > 125)) {
			return websocket_protocol_error(c, 1002);
		}
		if (payload_length > WS_MESSAGE_MAX ||
		    header_length > SIZE_MAX - 4 ||
		    payload_length > SIZE_MAX - header_length - 4) {
			return websocket_protocol_error(c, 1009);
		}
		header_length += 4;
		frame_length = header_length + (size_t)payload_length;
		if (c->ws_in_len < frame_length)
			return 0;
		mask = c->ws_in + header_length - 4;
		payload = c->ws_in + header_length;
		for (i = 0; i < (size_t)payload_length; i++)
			payload[i] ^= mask[i & 3];

		if (opcode == 0x8) {
			if (payload_length == 1) {
				(void)websocket_protocol_error(c, 1002);
			} else if (payload_length >= 2) {
				uint16_t code = ((uint16_t)payload[0] << 8) | payload[1];

				if (!close_code_valid(code) ||
				    !utf8_valid(payload + 2, (size_t)payload_length - 2))
					(void)websocket_protocol_error(c, 1002);
				else
					websocket_receive_close(c, payload,
						(size_t)payload_length);
			} else {
				websocket_receive_close(c, NULL, 0);
			}
		} else if (c->closing) {
			/* No application or control traffic follows a sent Close. */
		} else if (opcode == 0x9) {
			(void)websocket_queue(c, 0xA, payload, (size_t)payload_length, 1);
		} else if (opcode == 0xA) {
			if (c->ping_outstanding &&
			    payload_length == sizeof(c->ping_payload) &&
			    memcmp(payload, c->ping_payload,
				    sizeof(c->ping_payload)) == 0) {
				c->ping_outstanding = 0;
				c->ping_sent_ms = 0;
			}
		} else if (opcode == 0x2) {
			websocket_close(c, 1003);
		} else if (opcode == 0x1) {
			if (c->ws_fragment_opcode) {
				websocket_close(c, 1002);
			} else if (fin) {
				if (websocket_message(srv, c, clients, client_count,
					payload, (size_t)payload_length) != 0)
					websocket_close(c, 1007);
			} else {
				memcpy(c->ws_message, payload, (size_t)payload_length);
				c->ws_message_len = (size_t)payload_length;
				c->ws_fragment_opcode = 0x1;
			}
		} else if (opcode == 0x0) {
			if (!c->ws_fragment_opcode ||
			    payload_length > WS_MESSAGE_MAX - c->ws_message_len) {
				websocket_close(c, c->ws_fragment_opcode ? 1009 : 1002);
			} else {
				memcpy(c->ws_message + c->ws_message_len, payload,
					(size_t)payload_length);
				c->ws_message_len += (size_t)payload_length;
				if (fin) {
					if (websocket_message(srv, c, clients, client_count,
						c->ws_message, c->ws_message_len) != 0)
						websocket_close(c, 1007);
					c->ws_message_len = 0;
					c->ws_fragment_opcode = 0;
				}
			}
		} else {
			websocket_close(c, 1002);
		}

		if (frame_length < c->ws_in_len)
			memmove(c->ws_in, c->ws_in + frame_length,
				c->ws_in_len - frame_length);
		c->ws_in_len -= frame_length;
	}
	return 0;
}

static void broadcast_status(struct nes_http *srv, struct client *clients,
	int client_count, const char *event)
{
	char *status = make_status_json(srv, event);
	int i;

	if (!status)
		return;
	for (i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0 && !clients[i].dead && !clients[i].closing &&
		    clients[i].phase == CLIENT_WEBSOCKET)
			(void)websocket_queue(&clients[i], 0x1, status,
				strlen(status), 1);
	}
	free(status);
}

static void websocket_broadcast_binary(struct client *clients, int client_count,
	const void *packet, size_t length)
{
	int i;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0 && !clients[i].dead && !clients[i].closing &&
		    clients[i].phase == CLIENT_WEBSOCKET)
			(void)websocket_queue_video(&clients[i], packet, length);
	}
}

static void websocket_broadcast_audio(struct client *clients, int client_count,
	const void *packet, size_t length, uint64_t duration_us)
{
	int i;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0 && !clients[i].dead && !clients[i].closing &&
		    clients[i].phase == CLIENT_WEBSOCKET)
			(void)websocket_queue_audio(&clients[i], packet, length,
				duration_us);
	}
}

static int websocket_client_count(struct client *clients, int client_count)
{
	int i;
	int count = 0;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0 && !clients[i].dead && !clients[i].closing &&
		    clients[i].phase == CLIENT_WEBSOCKET)
			count++;
	}
	return count;
}

static uint64_t websocket_viewer_session(struct client *clients,
	int client_count)
{
	int i;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd >= 0 && !clients[i].dead && !clients[i].closing &&
		    clients[i].phase == CLIENT_WEBSOCKET)
			return clients[i].stream_session_id;
	}
	return 0;
}

static int websocket_video_wanted(struct client *clients, int client_count,
	uint64_t now)
{
	int i;

	for (i = 0; i < client_count; i++) {
		if (clients[i].fd < 0 || clients[i].dead || clients[i].closing ||
		    clients[i].phase != CLIENT_WEBSOCKET)
			continue;
		websocket_expire_stale_media(&clients[i], now);
		if ((!clients[i].out || clients[i].out_kind != WS_OUT_VIDEO) &&
		    !clients[i].video_out)
			return 1;
	}
	return 0;
}

/* Drain a scheduler/network backlog and retain only the newest PCM chunk. */
static size_t pull_latest_audio(struct nes_http *srv, unsigned *rate,
	unsigned *channels)
{
	size_t retained = 0;
	unsigned retained_channels = 0;
	unsigned retained_rate = 0;

	for (;;) {
		size_t copied = host_copy_audio(srv->host, audio_drain_scratch,
			AUDIO_PULL_FRAMES, rate, channels);
		size_t samples;

		if (!copied)
			break;
		if (*channels == 0 || *channels > 2)
			return 0;
		samples = copied * *channels;
		if (copied == AUDIO_PULL_FRAMES || !retained ||
		    retained_channels != *channels || retained_rate != *rate) {
			memcpy(audio_scratch, audio_drain_scratch,
				samples * sizeof(audio_scratch[0]));
			retained = copied;
			retained_channels = *channels;
			retained_rate = *rate;
		} else {
			size_t keep = AUDIO_PULL_FRAMES - copied;

			if (keep > retained)
				keep = retained;
			memmove(audio_scratch,
				audio_scratch + (retained - keep) * *channels,
				keep * *channels * sizeof(audio_scratch[0]));
			memcpy(audio_scratch + keep * *channels,
				audio_drain_scratch,
				samples * sizeof(audio_scratch[0]));
			retained = keep + copied;
		}
		if (copied < AUDIO_PULL_FRAMES)
			break;
	}
	/*
	 * A final empty probe is allowed to report the host's new format after the
	 * retained packet was copied.  Keep the metadata paired with the PCM that
	 * is actually returned instead of labelling old samples with that new rate.
	 */
	if (retained) {
		*rate = retained_rate;
		*channels = retained_channels;
	}
	return retained;
}

static int audio_pacer_due(struct video_pacer *pacer, int viewers)
{
	uint64_t now;
	uint64_t periods;

	if (!viewers) {
		pacer->next_audio_deadline_ns = 0;
		return 0;
	}
	now = now_ns();
	if (!pacer->next_audio_deadline_ns) {
		if (!now)
			return 1;
		pacer->next_audio_deadline_ns = now >
			UINT64_MAX - AUDIO_PACKET_PERIOD_NS ? UINT64_MAX :
			now + AUDIO_PACKET_PERIOD_NS;
		return 1;
	}
	if (!now || now < pacer->next_audio_deadline_ns)
		return 0;
	periods = (now - pacer->next_audio_deadline_ns) /
		AUDIO_PACKET_PERIOD_NS + 1ull;
	if (periods > (UINT64_MAX - pacer->next_audio_deadline_ns) /
	    AUDIO_PACKET_PERIOD_NS)
		pacer->next_audio_deadline_ns = now >
			UINT64_MAX - AUDIO_PACKET_PERIOD_NS ? UINT64_MAX :
			now + AUDIO_PACKET_PERIOD_NS;
	else
		pacer->next_audio_deadline_ns += periods * AUDIO_PACKET_PERIOD_NS;
	return 1;
}

static uint64_t video_deadline_after(uint64_t now, uint64_t period)
{
	if (now > UINT64_MAX - period)
		return UINT64_MAX;
	return now + period;
}

static int effective_stream_fps(struct nes_http *srv)
{
	struct nes_host_status status;
	int configured = srv->stream.stream_fps;
	int native_fps;

	if (configured < NES_STREAM_FPS_MIN || configured > NES_STREAM_FPS_MAX)
		configured = NES_STREAM_FPS_DEFAULT;
	host_get_status(srv->host, &status);
	if (!status.game_loaded || status.fps < 1.0 || status.fps > 1000.0)
		return configured;
	native_fps = (int)(status.fps + 0.5);
	if (native_fps < NES_STREAM_FPS_MIN)
		native_fps = NES_STREAM_FPS_MIN;
	if (native_fps < configured)
		return native_fps;
	return configured;
}

static int video_pacer_due(struct video_pacer *pacer, int stream_fps,
	int viewers)
{
	uint64_t now;
	uint64_t period;
	uint64_t periods;

	if (!viewers) {
		pacer->next_deadline_ns = 0;
		pacer->last_video_id = UINT64_MAX;
		pacer->retry_backoff_ms = 0;
		pacer->stream_fps = 0;
		pacer->active = 0;
		return 0;
	}
	if (stream_fps < NES_STREAM_FPS_MIN ||
	    stream_fps > NES_STREAM_FPS_MAX)
		stream_fps = NES_STREAM_FPS_DEFAULT;

	now = now_ns();
	period = (1000000000ull + (uint64_t)stream_fps - 1ull) /
		(uint64_t)stream_fps;
	if (!pacer->active || pacer->stream_fps != stream_fps) {
		/*
		 * A new viewer and an FPS change both get the current frame
		 * immediately. A failed monotonic-clock read still permits that
		 * first frame, then safely suppresses further frames until the
		 * clock becomes available again.
		 */
		pacer->active = 1;
		pacer->stream_fps = stream_fps;
		pacer->last_video_id = UINT64_MAX;
		pacer->retry_backoff_ms = 0;
		pacer->next_deadline_ns = now ?
			video_deadline_after(now, period) : 0;
		return 1;
	}
	if (!pacer->next_deadline_ns) {
		if (!now)
			return 0;
		pacer->last_video_id = UINT64_MAX;
		pacer->next_deadline_ns = video_deadline_after(now, period);
		return 1;
	}
	if (!now || now < pacer->next_deadline_ns)
		return 0;

	/*
	 * Preserve the original phase after a late poll wake-up, but skip every
	 * missed slot instead of emitting a catch-up burst. This avoids the drift
	 * of "last = now" and preserves the long-term cap; poll jitter may still
	 * place two adjacent deliveries less than one nominal period apart.
	 */
	periods = (now - pacer->next_deadline_ns) / period + 1ull;
	if (periods > (UINT64_MAX - pacer->next_deadline_ns) / period)
		pacer->next_deadline_ns = video_deadline_after(now, period);
	else
		pacer->next_deadline_ns += periods * period;
	return 1;
}

/*
 * The emulation and network threads have independent phases. If the stream
 * deadline fires a fraction before the core publishes its next native frame,
 * consuming the whole stream period here would turn a steady 50 Hz PAL core
 * into a visibly uneven ~40 Hz stream. Retry soon, then exponentially back
 * off if the core is genuinely stalled so the network loop never busy-spins.
 */
static void video_pacer_retry_no_frame(struct video_pacer *pacer,
	int stream_fps)
{
	uint64_t now = now_ns();
	unsigned period_ms;
	unsigned delay_ms;

	if (!now)
		return;
	if (stream_fps < NES_STREAM_FPS_MIN ||
	    stream_fps > NES_STREAM_FPS_MAX)
		stream_fps = NES_STREAM_FPS_DEFAULT;
	period_ms = (1000u + (unsigned)stream_fps - 1u) /
		(unsigned)stream_fps;
	delay_ms = 1u << (pacer->retry_backoff_ms < 10u ?
		pacer->retry_backoff_ms : 10u);
	if (delay_ms > period_ms)
		delay_ms = period_ms;
	if (pacer->retry_backoff_ms < 11u)
		pacer->retry_backoff_ms++;
	pacer->next_deadline_ns = video_deadline_after(now,
		(uint64_t)delay_ms * 1000000ull);
}

static int video_pacer_poll_timeout_ms(const struct video_pacer *pacer,
	int stream_fps, int viewers, int fallback_ms)
{
	uint64_t now;
	uint64_t remaining_ns;
	uint64_t wait_ms;

	if (fallback_ms < 0)
		fallback_ms = 0;
	if (!viewers)
		return fallback_ms;
	if (stream_fps < NES_STREAM_FPS_MIN ||
	    stream_fps > NES_STREAM_FPS_MAX)
		stream_fps = NES_STREAM_FPS_DEFAULT;
	/*
	 * A new viewer or an FPS change should be serviced immediately. Once the
	 * pacer is active, use a ceiling conversion so poll never wakes early and
	 * turns a 16.7 ms cadence into alternating 10/20 ms delivery.
	 */
	if (!pacer || !pacer->active || pacer->stream_fps != stream_fps)
		return 0;
	if (!pacer->next_deadline_ns)
		return fallback_ms;
	now = now_ns();
	if (!now)
		return fallback_ms;
	if (now >= pacer->next_deadline_ns)
		return 0;
	remaining_ns = pacer->next_deadline_ns - now;
	wait_ms = remaining_ns / 1000000ull;
	if (remaining_ns % 1000000ull)
		wait_ms++;
	if (wait_ms < (uint64_t)fallback_ms)
		return (int)wait_ms;
	return fallback_ms;
}

static void broadcast_av(struct nes_http *srv, struct client *clients,
	int client_count, struct video_pacer *pacer, int stream_fps)
{
	unsigned width = 0;
	unsigned height = 0;
	uint64_t frame_id = 0;
	uint64_t viewer_session;
	unsigned rate = 48000;
	unsigned channels = 2;
	size_t audio_frames;
	int video_due;
	int viewers = websocket_client_count(clients, client_count);

	host_set_viewers(srv->host, viewers);
	viewer_session = websocket_viewer_session(clients, client_count);
	jpeg_worker_set_session(srv->jpeg_worker, viewer_session);
	jpeg_worker_expire(srv->jpeg_worker, now_ms());
	if (!viewers) {
		(void)video_pacer_due(pacer, stream_fps, 0);
		(void)audio_pacer_due(pacer, 0);
		(void)pull_latest_audio(srv, &rate, &channels);
		return;
	}

	/* Audio is queued first so a video frame can never jump ahead of fresh PCM. */
	audio_frames = audio_pacer_due(pacer, viewers) ?
		pull_latest_audio(srv, &rate, &channels) : 0;
	if (audio_frames && rate && channels > 0 && channels <= 2) {
		uint8_t packet[12 + AUDIO_PULL_FRAMES * 2 * 2];
		size_t samples = audio_frames * channels;
		uint64_t duration_us =
			((uint64_t)audio_frames * 1000000ull + rate - 1u) / rate;
		size_t i;

		packet[0] = NES_PKT_AUDIO_PCM;
		packet[1] = (uint8_t)channels;
		packet[2] = 0;
		packet[3] = 0;
		packet[4] = (uint8_t)rate;
		packet[5] = (uint8_t)(rate >> 8);
		packet[6] = (uint8_t)(rate >> 16);
		packet[7] = (uint8_t)(rate >> 24);
		packet[8] = (uint8_t)audio_frames;
		packet[9] = (uint8_t)(audio_frames >> 8);
		packet[10] = (uint8_t)(audio_frames >> 16);
		packet[11] = (uint8_t)(audio_frames >> 24);
		for (i = 0; i < samples; i++) {
			uint16_t value = (uint16_t)audio_scratch[i];

			packet[12 + i * 2] = (uint8_t)value;
			packet[13 + i * 2] = (uint8_t)(value >> 8);
		}
		websocket_broadcast_audio(clients, client_count, packet,
			12 + samples * 2, duration_us);
	}
	if (srv->stream.use_jpeg && srv->jpeg_worker &&
	    websocket_video_wanted(clients, client_count, now_ms())) {
		size_t completed_length = 0;

		if (jpeg_worker_collect(srv->jpeg_worker, raw_packet,
		    sizeof(raw_packet), &completed_length) == 1)
			websocket_broadcast_binary(clients, client_count,
				raw_packet, completed_length);
	}

	video_due = video_pacer_due(pacer, stream_fps, viewers);
	if (video_due && websocket_video_wanted(clients, client_count, now_ms())) {
		int fresh_frame = host_copy_frame(srv->host, frame_scratch,
			NES_MAX_W * NES_MAX_H, &width, &height, &frame_id) &&
			width > 0 && height > 0 && width <= NES_MAX_W &&
			height <= NES_MAX_H && frame_id != pacer->last_video_id;
		size_t packet_length;

		if (!fresh_frame) {
			video_pacer_retry_no_frame(pacer, stream_fps);
			return;
		}
		pacer->retry_backoff_ms = 0;
		pacer->last_video_id = frame_id;
		if (srv->stream.use_jpeg && srv->jpeg_worker &&
		    jpeg_worker_submit(srv->jpeg_worker, frame_scratch,
			width, height, srv->stream.jpeg_quality, frame_id) == 1)
			return;
		/*
		 * A worker initialization/submission failure falls back immediately.
		 * Per-frame JPEG failures are converted to RGB565 by the worker itself,
		 * so the expensive encoder never returns to this event-loop thread.
		 */
		packet_length = jpeg_worker_make_raw_packet(raw_packet,
			frame_scratch, width, height, frame_id);
		if (packet_length)
			websocket_broadcast_binary(clients, client_count, raw_packet,
				packet_length);
	}
}

static void abort_upload(struct client *c)
{
	if (c->upload_fd >= 0) {
		close(c->upload_fd);
		c->upload_fd = -1;
	}
	if (c->upload_dir_fd >= 0 && c->upload_temp[0])
		(void)unlinkat(c->upload_dir_fd, c->upload_temp, 0);
	if (c->upload_dir_fd >= 0) {
		close(c->upload_dir_fd);
		c->upload_dir_fd = -1;
	}
	c->upload_temp[0] = '\0';
	c->upload_name[0] = '\0';
	c->upload_path[0] = '\0';
	c->upload_got = 0;
}

static int begin_upload(struct nes_http *srv, struct client *c,
	const struct request_info *request, const uint8_t *initial,
	size_t initial_length)
{
	char requested[256];
	char final_path[PATH_MAX];
	unsigned attempt;
	uint64_t seed = now_ms();
	int query_result;
	int n;

	requested[0] = '\0';
	query_result = query_value(request->target, "name", requested,
		sizeof(requested));
	if (query_result < 0) {
		errno = EINVAL;
		return -1;
	}
	if (request->x_filename[0]) {
		if (requested[0] && strcmp(requested, request->x_filename) != 0) {
			errno = EINVAL;
			return -1;
		}
		(void)snprintf(requested, sizeof(requested), "%s",
			request->x_filename);
	}
	if (!requested[0] ||
	    sanitize_filename(requested, c->upload_name,
		sizeof(c->upload_name)) != 0) {
		errno = EINVAL;
		return -1;
	}
	n = snprintf(final_path, sizeof(final_path), "%s/%s",
		srv->rom_dir, c->upload_name);
	if (n < 0 || (size_t)n >= sizeof(final_path) ||
	    storage_available(srv, final_path, request->content_length) != 0)
		return -1;
	c->upload_dir_fd = open(srv->rom_dir,
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (c->upload_dir_fd < 0)
		return -1;
	for (attempt = 0; attempt < 128; attempt++) {
		n = snprintf(c->upload_temp, sizeof(c->upload_temp),
			".nes-upload-%ld-%llx-%u.tmp", (long)getpid(),
			(unsigned long long)seed, attempt);
		if (n < 0 || (size_t)n >= sizeof(c->upload_temp))
			break;
		c->upload_fd = openat(c->upload_dir_fd, c->upload_temp,
			O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0640);
		if (c->upload_fd >= 0)
			break;
		if (errno != EEXIST)
			break;
	}
	if (c->upload_fd < 0) {
		abort_upload(c);
		return -1;
	}
	strcpy(c->upload_path, final_path);
	c->phase = CLIENT_HTTP_UPLOAD;
	c->content_length = request->content_length;
	c->upload_got = 0;
	if (initial_length) {
		if (initial_length > c->content_length ||
		    write_all_fd(c->upload_fd, initial, initial_length) != 0) {
			abort_upload(c);
			return -1;
		}
		c->upload_got = initial_length;
	}
	{
		uint64_t extra = ((uint64_t)c->content_length * 1000ull) /
			UPLOAD_MIN_PROGRESS_BYTES_PER_SEC;
		uint64_t timeout = UPLOAD_BASE_TIMEOUT_MS + extra;
		uint64_t started_at = now_ms();
		uint64_t inactivity_deadline;

		if (timeout > UPLOAD_MAX_TIMEOUT_MS)
			timeout = UPLOAD_MAX_TIMEOUT_MS;
		c->upload_absolute_deadline_ms = started_at + timeout;
		inactivity_deadline = started_at + UPLOAD_INACTIVITY_TIMEOUT_MS;
		c->deadline_ms = inactivity_deadline <
			c->upload_absolute_deadline_ms ? inactivity_deadline :
			c->upload_absolute_deadline_ms;
	}
	return 0;
}

static unsigned active_upload_count(const struct client *clients,
	int client_count)
{
	unsigned active = 0;
	int i;

	for (i = 0; i < client_count; i++) {
		if (!clients[i].dead && clients[i].phase == CLIENT_HTTP_UPLOAD)
			active++;
	}
	return active;
}

static int finish_upload(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	static const char durability_warning[] =
		"ROM stored, but directory durability could not be confirmed";
	struct stat st;
	struct strbuf json;
	int sync_warning = 0;
	int saved_errno;

	if (c->upload_got != c->content_length) {
		errno = EINVAL;
		abort_upload(c);
		return -1;
	}
	if (fsync(c->upload_fd) != 0 || fstat(c->upload_fd, &st) != 0) {
		saved_errno = errno;
		abort_upload(c);
		errno = saved_errno;
		return -1;
	}
	if (!rom_fd_valid(c->upload_fd, c->upload_name,
		srv->max_upload_bytes, c->content_length)) {
		abort_upload(c);
		errno = EINVAL;
		return -1;
	}
	if (storage_commit_allowed(srv, c->upload_path) != 0) {
		saved_errno = errno;
		abort_upload(c);
		errno = saved_errno;
		return -1;
	}
	if (close(c->upload_fd) != 0) {
		c->upload_fd = -1;
		abort_upload(c);
		return -1;
	}
	c->upload_fd = -1;
	if (renameat(c->upload_dir_fd, c->upload_temp,
		c->upload_dir_fd, c->upload_name) != 0) {
		abort_upload(c);
		return -1;
	}
	c->upload_temp[0] = '\0';
	if (fsync(c->upload_dir_fd) != 0 &&
	    errno != EINVAL && errno != EOPNOTSUPP) {
		/*
		 * renameat already committed atomically. Keep ok=true so clients do
		 * not retry and overwrite it, but surface the durability warning.
		 */
		sync_warning = 1;
		fprintf(stderr, "warning: ROM directory fsync failed: %s\n",
			strerror(errno));
	}
	close(c->upload_dir_fd);
	c->upload_dir_fd = -1;

	if (sb_init(&json, 256, 2048) != 0) {
		http_json(srv, c, 200, sync_warning
			? "{\"ok\":true,\"warning\":\"ROM stored, but directory "
			  "durability could not be confirmed\"}"
			: "{\"ok\":true}");
		broadcast_status(srv, clients, client_count, "rom-uploaded");
		c->dead = 1;
		return 0;
	}
	(void)sb_append(&json, "{\"ok\":true,\"name\":");
	(void)sb_append_json_string(&json, c->upload_name);
	(void)sb_append(&json, ",\"path\":");
	(void)sb_append_json_string(&json, c->upload_path);
	if (sync_warning) {
		(void)sb_append(&json, ",\"warning\":");
		(void)sb_append_json_string(&json, durability_warning);
	}
	(void)sb_append(&json, "}");
	if (json.failed) {
		sb_free(&json);
		http_json(srv, c, 200, sync_warning
			? "{\"ok\":true,\"warning\":\"ROM stored, but directory "
			  "durability could not be confirmed\"}"
			: "{\"ok\":true}");
		broadcast_status(srv, clients, client_count, "rom-uploaded");
		c->dead = 1;
		return 0;
	}
	http_json(srv, c, 200, json.data);
	sb_free(&json);
	broadcast_status(srv, clients, client_count, "rom-uploaded");
	c->dead = 1;
	return 0;
}

static void upload_finish_error(struct nes_http *srv, struct client *c)
{
	if (errno == EINVAL)
		http_error(srv, c, 415, "ROM content does not match its format");
	else if (errno == ENOSPC || errno == EDQUOT)
		http_error(srv, c, 507,
			"ROM quota or free-space reserve exceeded");
	else
		http_error(srv, c, 500, "cannot commit uploaded ROM");
	c->dead = 1;
}

static int storage_error_status(int error_number)
{
	return error_number == ENOSPC || error_number == EDQUOT ||
		error_number == EROFS ? 507 : 500;
}

static void sram_action_error(struct nes_http *srv, struct client *c,
	const char *unchanged_action)
{
	char message[160];
	int error_number = errno ? errno : EIO;

	(void)snprintf(message, sizeof(message),
		"SRAM save failed; emulation was not %s", unchanged_action);
	http_error(srv, c, storage_error_status(error_number), message);
}

static int state_action_error_status(int error_number)
{
	if (error_number == EINVAL || error_number == ENAMETOOLONG)
		return 400;
	if (error_number == ENOENT)
		return 404;
	if (error_number == ENODATA || error_number == ENOTSUP)
		return 409;
	if (error_number == EBADMSG || error_number == EPROTO ||
	    error_number == EFBIG || error_number == EPERM ||
	    error_number == EACCES)
		return 422;
	return storage_error_status(error_number);
}

static const char *state_action_error_message(int error_number,
	const char *action)
{
	if (error_number == ENOENT)
		return "save-state slot is empty";
	if (error_number == ENODATA)
		return "load a ROM before using save states";
	if (error_number == ENOTSUP)
		return "the active core does not support save states";
	if (error_number == ENOTRECOVERABLE)
		return "save-state loading and rollback both failed; emulation was stopped";
	if (error_number == EBADMSG)
		return "save state is corrupt and was not loaded";
	if (error_number == EPROTO)
		return "save state is incompatible with the active core";
	if (error_number == EFBIG)
		return "save state exceeds the safe size limit";
	if (error_number == EPERM || error_number == EACCES)
		return "save state has unsafe file metadata";
	if (error_number == ENOSPC || error_number == EDQUOT ||
	    error_number == EROFS)
		return "save-state quota or free-space reserve exceeded";
	if (strcmp(action, "save") == 0)
		return "could not save the emulator state";
	if (strcmp(action, "load") == 0)
		return "could not load the emulator state; the previous state was restored";
	return "could not delete the save state";
}

static const char *state_slot_error_message(int error_number)
{
	if (error_number == EPROTO)
		return "incompatible core version";
	if (error_number == EPERM || error_number == EACCES)
		return "unsafe file metadata";
	if (error_number == EFBIG)
		return "invalid file size";
	return "corrupt save state";
}

static void handle_states(struct nes_http *srv, struct client *c)
{
	struct nes_state_info states[NES_STATE_SLOT_COUNT];
	struct nes_host_status status;
	struct strbuf json;
	char rom_name[256];
	unsigned i;

	host_get_status(srv->host, &status);
	if (host_list_states(srv->host, states, NES_STATE_SLOT_COUNT) != 0) {
		int error_number = errno ? errno : EIO;

		http_error(srv, c, state_action_error_status(error_number),
			"could not inspect save states");
		return;
	}
	if (sb_init(&json, 2048, 32u * 1024u) != 0) {
		http_error(srv, c, 500, "out of memory");
		return;
	}
	basename_safe(status.rom_path, rom_name, sizeof(rom_name));
	if (sb_appendf(&json, "{\"ok\":true,\"game_loaded\":%s,\"rom\":",
		status.game_loaded ? "true" : "false") != 0 ||
	    sb_append_json_string(&json, rom_name) != 0 ||
	    sb_append(&json, ",\"rom_path\":") != 0 ||
	    sb_append_json_string(&json, status.rom_path) != 0 ||
	    sb_append(&json, ",\"slots\":[") != 0)
		goto oom;
	for (i = 0; i < NES_STATE_SLOT_COUNT; i++) {
		if (i && sb_append(&json, ",") != 0)
			goto oom;
		if (sb_appendf(&json,
			"{\"slot\":%u,\"exists\":%s,\"loadable\":%s,"
			"\"modified\":%llu,\"size\":%llu,\"label\":",
			states[i].slot,
			states[i].exists ? "true" : "false",
			states[i].loadable ? "true" : "false",
			(unsigned long long)states[i].modified,
			(unsigned long long)states[i].size) != 0 ||
		    sb_append_json_string(&json, states[i].label) != 0)
			goto oom;
		if (states[i].exists && !states[i].loadable) {
			if (sb_append(&json, ",\"error\":") != 0 ||
			    sb_append_json_string(&json,
				state_slot_error_message(states[i].error)) != 0)
				goto oom;
		}
		if (sb_append(&json, "}") != 0)
			goto oom;
	}
	if (sb_append(&json, "]}") != 0)
		goto oom;
	http_json(srv, c, 200, json.data);
	sb_free(&json);
	return;

oom:
	sb_free(&json);
	http_error(srv, c, 500, "out of memory");
}

static void websocket_discard_state_transition(struct nes_http *srv,
	struct client *clients, int client_count)
{
	int i;

	jpeg_worker_reset(srv ? srv->jpeg_worker : NULL);
	for (i = 0; i < client_count; i++) {
		if (clients[i].fd < 0 || clients[i].dead || clients[i].closing ||
		    clients[i].phase != CLIENT_WEBSOCKET)
			continue;
		clients[i].joy[0] = 0;
		clients[i].joy[1] = 0;
		/* An unsent media frame is safe to cancel at a state boundary. */
		if (clients[i].out && clients[i].out_off == 0 &&
		    (clients[i].out_kind == WS_OUT_AUDIO ||
		     clients[i].out_kind == WS_OUT_VIDEO))
			websocket_clear_active(&clients[i]);
		websocket_audio_clear(&clients[i]);
		free(clients[i].video_out);
		clients[i].video_out = NULL;
		clients[i].video_len = 0;
		clients[i].video_queued_ms = 0;
		free(clients[i].status_out);
		clients[i].status_out = NULL;
		clients[i].status_len = 0;
		websocket_promote_next(&clients[i], now_ms());
	}
}

static void handle_state_action(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count, const char *body,
	size_t body_length, const char *action)
{
	struct nes_state_info state;
	struct strbuf json;
	uint64_t slot_value = 0;
	char label[NES_STATE_LABEL_BYTES];
	int slot_result;
	int label_result;
	int result;
	bool durable = true;

	if (!json_validate_object(body, body_length)) {
		http_error(srv, c, 400, "invalid JSON object");
		return;
	}
	slot_result = json_get_uint(body, "slot", &slot_value);
	if (slot_result != 1 || slot_value < 1 ||
	    slot_value > NES_STATE_SLOT_COUNT) {
		http_error(srv, c, 400, "slot must be an integer from 1 to 10");
		return;
	}
	label[0] = '\0';
	label_result = json_get_string(body, "label", label, sizeof(label));
	if (label_result < 0 ||
	    (strcmp(action, "save") != 0 && label_result != 0)) {
		http_error(srv, c, 400,
			"label must be a valid short string and is only accepted when saving");
		return;
	}
	if (strcmp(action, "save") == 0)
		result = host_save_state(srv->host, (unsigned)slot_value,
			label, &state);
	else if (strcmp(action, "load") == 0)
		result = host_load_state(srv->host, (unsigned)slot_value);
	else
		result = host_delete_state(srv->host, (unsigned)slot_value,
			&durable);
	if (result != 0) {
		int error_number = errno ? errno : EIO;

		if (strcmp(action, "load") == 0 &&
		    error_number == ENOTRECOVERABLE) {
			websocket_discard_state_transition(srv, clients, client_count);
			recompute_inputs(srv, clients, client_count);
			broadcast_status(srv, clients, client_count,
				"state-load-fatal");
		}
		http_error(srv, c, state_action_error_status(error_number),
			state_action_error_message(error_number, action));
		return;
	}
	if (strcmp(action, "load") == 0) {
		websocket_discard_state_transition(srv, clients, client_count);
		recompute_inputs(srv, clients, client_count);
		http_json(srv, c, 200, "{\"ok\":true}");
		broadcast_status(srv, clients, client_count, "state-loaded");
		return;
	}
	if (strcmp(action, "save") == 0)
		durable = state.durable;
	if (sb_init(&json, 256, 4096) != 0) {
		http_error(srv, c, 500, "out of memory");
		return;
	}
	if (sb_appendf(&json,
		"{\"ok\":true,\"slot\":%llu,\"durable\":%s",
		(unsigned long long)slot_value, durable ? "true" : "false") != 0 ||
	    (!durable && sb_append(&json,
		",\"warning\":\"operation completed, but directory durability could not be confirmed\"") != 0) ||
	    sb_append(&json, "}") != 0) {
		sb_free(&json);
		http_error(srv, c, 500, "out of memory");
		return;
	}
	http_json(srv, c, 200, json.data);
	sb_free(&json);
	broadcast_status(srv, clients, client_count,
		strcmp(action, "save") == 0 ? "state-saved" : "state-deleted");
}

static void handle_load(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count, const char *body,
	size_t body_length)
{
	char rom[NES_PATH_MAX];
	char path[NES_PATH_MAX];
	char full[PATH_MAX];
	int path_result;
	int rom_result;

	if (!json_validate_object(body, body_length)) {
		http_error(srv, c, 400, "invalid JSON object");
		return;
	}
	path_result = json_get_string(body, "path", path, sizeof(path));
	rom_result = json_get_string(body, "rom", rom, sizeof(rom));
	if (path_result < 0 || rom_result < 0) {
		http_error(srv, c, 400, "invalid JSON string");
		return;
	}
	if (!path[0] && rom[0])
		(void)snprintf(path, sizeof(path), "%s", rom);
	if (!path[0]) {
		http_error(srv, c, 400, "rom or path required");
		return;
	}
	path_result = resolve_rom_path(srv, path, full, sizeof(full));
	if (path_result != 0) {
		if (path_result == -2) {
			http_error(srv, c, 403,
				"ROM is not readable by nesd; upload it through LuCI "
				"or set group nesd and mode 0640");
			return;
		}
		http_error(srv, c, 400, "ROM not found, invalid, or not allowed");
		return;
	}
	if (host_load_game(srv->host, full) != 0) {
		int error_number = errno ? errno : EIO;

		if (error_number == ENAMETOOLONG)
			http_error(srv, c, 400,
				"ROM path is too long for the emulation core");
		else
			http_error(srv, c, storage_error_status(error_number),
				"load failed; SRAM was not overwritten");
		return;
	}
	websocket_discard_state_transition(srv, clients, client_count);
	recompute_inputs(srv, clients, client_count);
	if (host_set_paused(srv->host, false) != 0) {
		http_error(srv, c, 500, "cannot resume loaded ROM");
		return;
	}
	if (!host_is_running(srv->host) && host_start(srv->host) != 0) {
		http_error(srv, c, 500, "start failed");
		return;
	}
	http_json(srv, c, 200, "{\"ok\":true}");
	broadcast_status(srv, clients, client_count, "loaded");
}

static int path_is_api(const char *path)
{
	return strncmp(path, "/api/", 5) == 0;
}

static void dispatch_http(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	const char *body = c->body ? (const char *)c->body : "";
	int get = strcmp(c->method, "GET") == 0;
	int post = strcmp(c->method, "POST") == 0;
	int options = strcmp(c->method, "OPTIONS") == 0;

	if (options) {
		const char *headers = c->preflight_private_network
			?
			"Access-Control-Allow-Headers: Authorization, Content-Type, X-Filename\r\n"
			"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
			"Access-Control-Max-Age: 600\r\n"
			"Access-Control-Allow-Private-Network: true\r\n"
			:
			"Access-Control-Allow-Headers: Authorization, Content-Type, X-Filename\r\n"
			"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
			"Access-Control-Max-Age: 600\r\n";

		if (!path_is_api(c->path) || !srv->allowed_origin[0] ||
		    !c->cors_allowed) {
			http_error(srv, c, 403, "cross-origin preflight denied");
		} else {
			(void)http_response(srv, c, 200, "text/plain; charset=utf-8",
				"", 0, headers);
		}
		c->dead = 1;
		return;
	}
	if (get && (strcmp(c->path, "/") == 0 ||
	    strcmp(c->path, "/play") == 0 ||
	    strcmp(c->path, "/play.html") == 0)) {
		(void)http_response(srv, c, 200, "text/html; charset=utf-8",
			PLAY_HTML, strlen(PLAY_HTML),
			"Content-Security-Policy: default-src 'self'; "
			"script-src 'self' 'unsafe-inline'; "
			"style-src 'self' 'unsafe-inline'; "
			"connect-src 'self' ws: wss:; img-src 'self' blob:; "
			"object-src 'none'; base-uri 'none'; form-action 'none'; "
			"frame-ancestors 'none'; worker-src 'none'\r\n");
	} else if (get && strcmp(c->path, "/api/status") == 0) {
		handle_status(srv, c);
	} else if (get && strcmp(c->path, "/api/roms") == 0) {
		handle_roms(srv, c);
	} else if (get && strcmp(c->path, "/api/states") == 0) {
		handle_states(srv, c);
	} else if (post && strcmp(c->path, "/api/load") == 0) {
		handle_load(srv, c, clients, client_count, body,
			c->content_length);
	} else if (post && strcmp(c->path, "/api/state/save") == 0) {
		handle_state_action(srv, c, clients, client_count, body,
			c->content_length, "save");
	} else if (post && strcmp(c->path, "/api/state/load") == 0) {
		handle_state_action(srv, c, clients, client_count, body,
			c->content_length, "load");
	} else if (post && strcmp(c->path, "/api/state/delete") == 0) {
		handle_state_action(srv, c, clients, client_count, body,
			c->content_length, "delete");
	} else if (post && strcmp(c->path, "/api/start") == 0) {
		struct nes_host_status status;

		host_get_status(srv->host, &status);
		if (!status.game_loaded)
			http_error(srv, c, 409, "load a ROM before starting");
		else if (host_set_paused(srv->host, false) != 0)
			http_error(srv, c, 500, "resume failed");
		else if (host_start(srv->host) != 0)
			http_error(srv, c, 500, "start failed");
		else {
			http_json(srv, c, 200, "{\"ok\":true}");
			broadcast_status(srv, clients, client_count, "started");
		}
	} else if (post && strcmp(c->path, "/api/stop") == 0) {
		if (host_stop(srv->host) != 0)
			sram_action_error(srv, c, "stopped");
		else {
			http_json(srv, c, 200, "{\"ok\":true}");
			broadcast_status(srv, clients, client_count, "stopped");
		}
	} else if (post && strcmp(c->path, "/api/pause") == 0) {
		bool pause = !host_is_paused(srv->host);

		if (host_set_paused(srv->host, pause) != 0) {
			if (pause)
				sram_action_error(srv, c, "paused");
			else
				http_error(srv, c, 500, "resume failed");
		} else {
			http_json(srv, c, 200, "{\"ok\":true}");
			broadcast_status(srv, clients, client_count,
				"pause-changed");
		}
	} else if (post && strcmp(c->path, "/api/reset") == 0) {
		if (host_reset(srv->host) != 0)
			sram_action_error(srv, c, "reset");
		else {
			websocket_discard_state_transition(srv, clients,
				client_count);
			recompute_inputs(srv, clients, client_count);
			http_json(srv, c, 200, "{\"ok\":true}");
			broadcast_status(srv, clients, client_count, "reset");
		}
	} else if (path_is_api(c->path)) {
		http_error(srv, c, 405, "method or API endpoint not allowed");
	} else {
		http_error(srv, c, 404, "not found");
	}
	c->dead = 1;
}

static int start_http_request(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count, size_t header_length)
{
	struct request_info request;
	size_t initial_length = c->in_len - header_length;
	const uint8_t *initial = c->in + header_length;
	int parse_result = parse_request(c->in, header_length, &request);
	int sensitive;
	int websocket;
	int unauthenticated_preflight;

	if (parse_result == -2) {
		http_error(srv, c, 405, "method not allowed");
		c->dead = 1;
		return -1;
	}
	if (parse_result != 0) {
		http_error(srv, c, 400, "malformed HTTP request");
		c->dead = 1;
		return -1;
	}
	websocket = strcmp(request.path, "/ws") == 0;
	sensitive = websocket || path_is_api(request.path);
	unauthenticated_preflight =
		valid_unauthenticated_preflight(srv, &request);
	(void)snprintf(c->request_origin, sizeof(c->request_origin), "%s",
		request.origin);
	c->cors_allowed = srv->allowed_origin[0] && request.origin[0] &&
		constant_time_equal(request.origin, srv->allowed_origin);
	c->preflight_private_network = request.private_network;
	if (sensitive && !request_origin_allowed(srv, &request)) {
		reject_http_request(srv, c, &request, initial_length,
			403, "origin denied");
		return 0;
	}
	if (sensitive && !unauthenticated_preflight &&
	    !request_authorized(srv, &request, websocket)) {
		reject_http_request(srv, c, &request, initial_length,
			401, "authentication required");
		return 0;
	}
	if (strcmp(request.method, "OPTIONS") == 0 &&
	    !unauthenticated_preflight) {
		reject_http_request(srv, c, &request, initial_length,
			403, "invalid cross-origin preflight");
		return 0;
	}
	if (websocket) {
		static const char upgrade_error[] =
			"{\"error\":\"valid WebSocket v13 upgrade required\"}";

		if (strcmp(request.method, "GET") != 0 ||
		    !request.upgrade_websocket || !request.connection_upgrade ||
		    strcmp(request.ws_version, "13") != 0 ||
		    !websocket_key_valid(request.ws_key) ||
		    (request.has_content_length && request.content_length != 0)) {
			(void)http_response(srv, c, 426, "application/json; charset=utf-8",
				upgrade_error, sizeof(upgrade_error) - 1,
				"Sec-WebSocket-Version: 13\r\n");
			begin_http_discard(c, &request, initial_length);
			return 0;
		}
		if (websocket_client_count(clients, client_count) >=
		    MAX_STREAM_CLIENTS) {
			reject_http_request(srv, c, &request, initial_length,
				503, "another game stream is already active");
			return 0;
		}
		if (websocket_handshake(c, &request) != 0) {
			c->dead = 1;
			return -1;
		}
		websocket_tune_socket(c->fd);
		c->phase = CLIENT_WEBSOCKET;
		c->stream_session_id = jpeg_worker_new_session(srv->jpeg_worker);
		c->in_len = 0;
		c->last_rx_ms = now_ms();
		c->last_ping_ms = c->last_rx_ms;
		c->last_input_ms = c->last_rx_ms;
		if (initial_length > sizeof(c->ws_in)) {
			websocket_close(c, 1009);
		} else if (initial_length) {
			memcpy(c->ws_in, initial, initial_length);
			c->ws_in_len = initial_length;
			(void)websocket_process(srv, c, clients, client_count);
		}
		return 0;
	}

	if (request.upgrade_websocket || request.connection_upgrade) {
		reject_http_request(srv, c, &request, initial_length,
			400, "unexpected protocol upgrade");
		return 0;
	}
	if (strcmp(request.method, "POST") == 0 && !request.has_content_length) {
		http_error(srv, c, 411, "Content-Length required");
		c->dead = 1;
		return -1;
	}
	if (!request.has_content_length)
		request.content_length = 0;
	if (strcmp(request.path, "/api/upload") == 0) {
		if (strcmp(request.method, "POST") != 0 ||
		    request.content_length == 0) {
			reject_http_request(srv, c, &request, initial_length,
				400, "non-empty raw upload required");
			return 0;
		}
		if (request.content_length > srv->max_upload_bytes) {
			reject_http_request(srv, c, &request, initial_length,
				413, "ROM exceeds upload limit");
			return 0;
		}
		if (active_upload_count(clients, client_count) >=
		    MAX_ACTIVE_UPLOADS) {
			reject_http_request(srv, c, &request, initial_length,
				409, "another ROM upload is in progress");
			return 0;
		}
		if (initial_length > request.content_length)
			initial_length = request.content_length;
		if (begin_upload(srv, c, &request, initial, initial_length) != 0) {
			int code = errno == ENOSPC || errno == EDQUOT ? 507 :
				errno == EINVAL ? 400 : 500;

			reject_http_request(srv, c, &request, initial_length,
				code, code == 507
				? "ROM quota or free-space reserve exceeded"
				: code == 400 ? "invalid upload filename or target"
				: "cannot create temporary upload");
			return 0;
		}
		(void)snprintf(c->method, sizeof(c->method), "%s", request.method);
		(void)snprintf(c->path, sizeof(c->path), "%s", request.path);
		if (c->upload_got == c->content_length &&
		    finish_upload(srv, c, clients, client_count) != 0)
			upload_finish_error(srv, c);
		return 0;
	}
	if (request.content_length > HTTP_BODY_MAX) {
		reject_http_request(srv, c, &request, initial_length,
			413, "request body too large");
		return 0;
	}
	(void)snprintf(c->method, sizeof(c->method), "%s", request.method);
	(void)snprintf(c->target, sizeof(c->target), "%s", request.target);
	(void)snprintf(c->path, sizeof(c->path), "%s", request.path);
	c->content_length = request.content_length;
	c->body_got = initial_length > request.content_length
		? request.content_length : initial_length;
	c->body = malloc(request.content_length + 1);
	if (!c->body) {
		reject_http_request(srv, c, &request, initial_length,
			500, "out of memory");
		return 0;
	}
	if (c->body_got)
		memcpy(c->body, initial, c->body_got);
	c->body[c->body_got] = '\0';
	c->in_len = 0;
	if (c->body_got == c->content_length) {
		dispatch_http(srv, c, clients, client_count);
	} else {
		c->phase = CLIENT_HTTP_BODY;
		c->deadline_ms = now_ms() + BODY_TIMEOUT_MS;
	}
	return 0;
}

static void client_init(struct client *c, int fd)
{
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	c->upload_fd = -1;
	c->upload_dir_fd = -1;
	c->phase = CLIENT_HTTP_HEADERS;
	c->accepted_ms = now_ms();
	c->last_rx_ms = c->accepted_ms;
	c->deadline_ms = c->accepted_ms + HEADER_TIMEOUT_MS;
}

static void client_destroy(struct client *c)
{
	abort_upload(c);
	free(c->body);
	free(c->out);
	free(c->control_out);
	free(c->status_out);
	free(c->heartbeat_out);
	websocket_audio_clear(c);
	free(c->video_out);
	if (c->fd >= 0)
		close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	c->upload_fd = -1;
	c->upload_dir_fd = -1;
}

static void read_http_headers(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	ssize_t n;
	ssize_t header_end;

	if (c->in_len >= HTTP_HEADER_MAX) {
		http_error(srv, c, 431, "request headers too large");
		c->dead = 1;
		return;
	}
	n = recv(c->fd, c->in + c->in_len, HTTP_HEADER_MAX - c->in_len, 0);
	if (n == 0) {
		c->dead = 1;
		return;
	}
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			c->dead = 1;
		return;
	}
	c->in_len += (size_t)n;
	c->last_rx_ms = now_ms();
	header_end = find_header_end(c->in, c->in_len);
	if (header_end >= 0)
		(void)start_http_request(srv, c, clients, client_count,
			(size_t)header_end);
	else if (c->in_len >= HTTP_HEADER_MAX) {
		http_error(srv, c, 431, "request headers too large");
		c->dead = 1;
	}
}

static void read_http_body(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	size_t remaining = c->content_length - c->body_got;
	ssize_t n = recv(c->fd, c->body + c->body_got, remaining, 0);

	if (n == 0) {
		c->dead = 1;
		return;
	}
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			c->dead = 1;
		return;
	}
	c->body_got += (size_t)n;
	c->body[c->body_got] = '\0';
	c->last_rx_ms = now_ms();
	if (c->body_got == c->content_length)
		dispatch_http(srv, c, clients, client_count);
}

static void read_upload(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	uint8_t chunk[8192];
	size_t remaining = c->content_length - c->upload_got;
	size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
	ssize_t n = recv(c->fd, chunk, wanted, 0);

	if (n == 0) {
		c->dead = 1;
		return;
	}
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			c->dead = 1;
		return;
	}
	if (write_all_fd(c->upload_fd, chunk, (size_t)n) != 0) {
		size_t received = c->upload_got + (size_t)n;

		abort_upload(c);
		http_error(srv, c, 507, "upload write failed");
		begin_http_discard_lengths(c, c->content_length, received,
			REJECT_DRAIN_MAX, ERROR_DRAIN_TIMEOUT_MS);
		return;
	}
	c->upload_got += (size_t)n;
	c->last_rx_ms = now_ms();
	{
		uint64_t inactivity_deadline = c->last_rx_ms +
			UPLOAD_INACTIVITY_TIMEOUT_MS;

		c->deadline_ms = inactivity_deadline <
			c->upload_absolute_deadline_ms ? inactivity_deadline :
			c->upload_absolute_deadline_ms;
	}
	if (c->upload_got == c->content_length &&
	    finish_upload(srv, c, clients, client_count) != 0)
		upload_finish_error(srv, c);
}

static void read_http_discard(struct client *c)
{
	uint8_t chunk[8192];
	size_t remaining = c->content_length - c->body_got;
	size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
	ssize_t n;

	if (!wanted) {
		c->dead = 1;
		return;
	}
	n = recv(c->fd, chunk, wanted, 0);
	if (n == 0) {
		c->dead = 1;
		return;
	}
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			c->dead = 1;
		return;
	}
	c->body_got += (size_t)n;
	c->last_rx_ms = now_ms();
	if (c->body_got == c->content_length)
		c->dead = 1;
}

static void read_websocket(struct nes_http *srv, struct client *c,
	struct client *clients, int client_count)
{
	ssize_t n;

	if (c->ws_in_len >= sizeof(c->ws_in)) {
		websocket_close(c, 1009);
		return;
	}
	n = recv(c->fd, c->ws_in + c->ws_in_len,
		sizeof(c->ws_in) - c->ws_in_len, 0);
	if (n == 0) {
		c->dead = 1;
		return;
	}
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			c->dead = 1;
		return;
	}
	c->ws_in_len += (size_t)n;
	c->last_rx_ms = now_ms();
	(void)websocket_process(srv, c, clients, client_count);
}

static int listener_error_priority(int error)
{
	if (error == EADDRINUSE)
		return 3;
	if (error == EACCES || error == EPERM)
		return 2;
	return error ? 1 : 0;
}

static void remember_listener_error(int *saved_error, int error)
{
	if (!saved_error || !error)
		return;
	if (listener_error_priority(error) >
	    listener_error_priority(*saved_error))
		*saved_error = error;
}

static int create_listeners(struct nes_http *srv)
{
	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	struct addrinfo *address;
	char service[16];
	int error;
	int saved_error = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;
	(void)snprintf(service, sizeof(service), "%d", srv->port);
	error = getaddrinfo(srv->bind_addr, service, &hints, &addresses);
	if (error != 0) {
		fprintf(stderr, "invalid bind address '%s': %s\n",
			srv->bind_addr, gai_strerror(error));
		if (error == EAI_SYSTEM && errno != 0)
			return -1;
		errno = error == EAI_MEMORY ? ENOMEM : EADDRNOTAVAIL;
		return -1;
	}
	for (address = addresses;
	     address && srv->listen_count < NES_HTTP_MAX_LISTENERS;
	     address = address->ai_next) {
		int fd;
		int yes = 1;

		fd = socket(address->ai_family, address->ai_socktype,
			address->ai_protocol);
		if (fd < 0) {
			remember_listener_error(&saved_error, errno);
			continue;
		}
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef IPV6_V6ONLY
		if (address->ai_family == AF_INET6) {
			int v6_only = 1;
			(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
				&v6_only, sizeof(v6_only));
		}
#endif
		if (bind(fd, address->ai_addr, address->ai_addrlen) != 0) {
			remember_listener_error(&saved_error, errno);
			(void)close(fd);
			continue;
		}
		if (listen(fd, 16) != 0) {
			remember_listener_error(&saved_error, errno);
			(void)close(fd);
			continue;
		}
		if (set_nonblock(fd) != 0) {
			remember_listener_error(&saved_error, errno);
			(void)close(fd);
			continue;
		}
		srv->listen_fds[srv->listen_count++] = fd;
	}
	freeaddrinfo(addresses);
	if (!srv->listen_count) {
		errno = saved_error ? saved_error : EADDRNOTAVAIL;
		return -1;
	}
	srv->listen_fd = srv->listen_fds[0];
	return 0;
}

static int upload_temp_name(const char *name, pid_t *owner_pid)
{
	static const char prefix[] = ".nes-upload-";
	const unsigned char *cursor;
	char *end;
	long parsed_pid;
	int digits = 0;

	if (!name || !owner_pid ||
	    strncmp(name, prefix, sizeof(prefix) - 1) != 0)
		return 0;
	cursor = (const unsigned char *)name + sizeof(prefix) - 1;
	errno = 0;
	parsed_pid = strtol((const char *)cursor, &end, 10);
	if (errno != 0 || end == (const char *)cursor ||
	    parsed_pid <= 0 || parsed_pid > INT_MAX || *end != '-')
		return 0;
	cursor = (const unsigned char *)end + 1;
	digits = 0;
	while (isxdigit(*cursor)) {
		digits = 1;
		cursor++;
	}
	if (!digits || *cursor++ != '-')
		return 0;
	digits = 0;
	while (isdigit(*cursor)) {
		digits = 1;
		cursor++;
	}
	if (!digits || strcmp((const char *)cursor, ".tmp") != 0)
		return 0;
	*owner_pid = (pid_t)parsed_pid;
	return 1;
}

static void cleanup_abandoned_uploads(const char *directory_path)
{
	DIR *directory;
	struct dirent *entry;
	int directory_fd;
	unsigned removed = 0;

	directory_fd = open(directory_path,
		O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
	if (directory_fd < 0)
		return;
	directory = fdopendir(directory_fd);
	if (!directory) {
		close(directory_fd);
		return;
	}
	while ((entry = readdir(directory)) != NULL) {
		struct stat st;
		pid_t owner_pid;

		if (!upload_temp_name(entry->d_name, &owner_pid) ||
		    fstatat(dirfd(directory), entry->d_name, &st,
			    AT_SYMLINK_NOFOLLOW) != 0 ||
		    !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
		    st.st_uid != geteuid())
			continue;
		/*
		 * Cleanup runs before this process can accept an upload, so a temp
		 * carrying our current PID necessarily belongs to an older process
		 * instance (for example after a reboot reused the same PID).
		 */
		if (owner_pid != getpid()) {
			if (kill(owner_pid, 0) == 0 || errno == EPERM)
				continue;
			if (errno != ESRCH)
				continue;
		}
		if (unlinkat(dirfd(directory), entry->d_name, 0) == 0)
			removed++;
		else
			fprintf(stderr,
				"warning: cannot remove abandoned upload %s: %s\n",
				entry->d_name, strerror(errno));
	}
	closedir(directory);
	if (removed)
		fprintf(stderr, "nesd: removed %u abandoned ROM upload(s)\n",
			removed);
}

int http_start(struct nes_http *srv, struct nes_host *host,
	const char *bind_addr, int port, const char *rom_dir,
	const char *extra_rom_dirs, const struct nes_stream_opts *opts)
{
	const char *requested_bind;
	const char *requested_dir;
	char configured_dir[PATH_MAX];
	char resolved_dir[PATH_MAX];
	char extras[2048];
	char *save = NULL;
	char *token;
	int i;

	if (!srv || !host) {
		errno = EINVAL;
		return -1;
	}
	if (port < 0 || port > 65535) {
		errno = EINVAL;
		return -1;
	}
	memset(srv, 0, sizeof(*srv));
	srv->host = host;
	srv->port = port > 0 ? port : 29876;
	srv->listen_fd = -1;
	for (i = 0; i < NES_HTTP_MAX_LISTENERS; i++)
		srv->listen_fds[i] = -1;
	srv->stream.jpeg_quality = 92;
	srv->stream.stream_fps = NES_STREAM_FPS_DEFAULT;
	srv->stream.use_jpeg = 0;
	srv->stream.show_fps = 1;
	srv->stream.show_touch_controls = 1;
	if (opts) {
		if (opts->jpeg_quality > 0)
			srv->stream.jpeg_quality = opts->jpeg_quality;
		if (opts->stream_fps > 0)
			srv->stream.stream_fps = opts->stream_fps;
		srv->stream.use_jpeg = !!opts->use_jpeg;
		srv->stream.show_fps = !!opts->show_fps;
		srv->stream.show_touch_controls = !!opts->show_touch_controls;
	}
	if (srv->stream.jpeg_quality < 1)
		srv->stream.jpeg_quality = 1;
	if (srv->stream.jpeg_quality > 100)
		srv->stream.jpeg_quality = 100;
	if (srv->stream.stream_fps < NES_STREAM_FPS_MIN ||
	    srv->stream.stream_fps > NES_STREAM_FPS_MAX)
		srv->stream.stream_fps = NES_STREAM_FPS_DEFAULT;
	srv->max_upload_bytes = MAX_UPLOAD_DEFAULT;
	srv->rom_quota_bytes = ROM_QUOTA_DEFAULT;
	srv->min_free_bytes = MIN_FREE_DEFAULT;

	requested_bind = bind_addr && bind_addr[0] ?
		bind_addr : "127.0.0.1";
	if (strlen(requested_bind) >= sizeof(srv->bind_addr)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(srv->bind_addr, requested_bind, strlen(requested_bind) + 1);

	requested_dir = rom_dir && rom_dir[0] ?
		rom_dir : "/etc/nes-emulator/roms";
	if (strlen(requested_dir) >= sizeof(configured_dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(configured_dir, requested_dir, strlen(requested_dir) + 1);
	if (mkdir_tree(configured_dir) != 0)
		return -1;
	if (!realpath(configured_dir, resolved_dir))
		return -1;
	if (strlen(resolved_dir) >= sizeof(srv->rom_dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (broad_directory(resolved_dir)) {
		errno = EINVAL;
		return -1;
	}
	strcpy(srv->rom_dir, resolved_dir);
	cleanup_abandoned_uploads(srv->rom_dir);
	if (add_root(srv, srv->rom_dir) != 0)
		return -1;
	if (extra_rom_dirs && extra_rom_dirs[0]) {
		if (strlen(extra_rom_dirs) >= sizeof(extras)) {
			errno = EINVAL;
			return -1;
		}
		strcpy(extras, extra_rom_dirs);
		for (token = strtok_r(extras, ":;,", &save); token;
		     token = strtok_r(NULL, ":;,", &save)) {
			while (*token == ' ' || *token == '\t')
				token++;
			if (*token && add_root(srv, token) != 0)
				fprintf(stderr, "ignoring invalid ROM root: %s\n", token);
		}
	}
	if (create_listeners(srv) != 0)
		return -1;
	fprintf(stderr,
		"nesd listening on %s:%d (rom_dir=%s, roots=%d, stream=%s q=%d fps=%d)\n",
		srv->bind_addr, srv->port, srv->rom_dir, srv->rom_root_count,
		srv->stream.use_jpeg ? "jpeg" : "raw",
		srv->stream.jpeg_quality, srv->stream.stream_fps);
	return 0;
}

int http_set_security(struct nes_http *srv, const char *auth_token,
	const char *allowed_origin)
{
	const char *token = auth_token ? auth_token : "";
	const char *origin = allowed_origin ? allowed_origin : "";
	const char *authority_end;
	const unsigned char *cursor;

	if (!srv || strlen(token) >= sizeof(srv->auth_token) ||
	    strlen(origin) >= sizeof(srv->allowed_origin) ||
	    contains_ctl_or_crlf(token) || contains_ctl_or_crlf(origin))
		return -1;
	if (origin[0]) {
		const char *authority;

		if (strncmp(origin, "http://", 7) == 0)
			authority = origin + 7;
		else if (strncmp(origin, "https://", 8) == 0)
			authority = origin + 8;
		else
			return -1;
		authority_end = strpbrk(authority, "/?#");
		if (!*authority || authority_end ||
		    strpbrk(authority, " \t\\@"))
			return -1;
		for (cursor = (const unsigned char *)authority; *cursor; cursor++) {
			if (*cursor <= 0x20 || *cursor >= 0x7f)
				return -1;
		}
	}
	strcpy(srv->auth_token, token);
	strcpy(srv->allowed_origin, origin);
	return 0;
}

void http_set_storage_limits(struct nes_http *srv, size_t max_upload_bytes,
	uint64_t rom_quota_bytes, uint64_t min_free_bytes)
{
	if (!srv)
		return;
	if (max_upload_bytes)
		srv->max_upload_bytes = max_upload_bytes > MAX_UPLOAD_DEFAULT
			? MAX_UPLOAD_DEFAULT : max_upload_bytes;
	if (rom_quota_bytes)
		srv->rom_quota_bytes = rom_quota_bytes;
	srv->min_free_bytes = min_free_bytes;
}

int http_set_idle_exit(struct nes_http *srv, unsigned seconds)
{
	if (!srv || seconds > 86400u) {
		errno = EINVAL;
		return -1;
	}
	srv->idle_exit_seconds = seconds;
	return 0;
}

void http_stop(struct nes_http *srv)
{
	if (!srv)
		return;
	/* Async-signal-safe: a signal wakes poll; other callers wait at most 250 ms. */
	srv->stop = 1;
}

static void close_listeners(struct nes_http *srv)
{
	int i;

	for (i = 0; i < srv->listen_count; i++) {
		if (srv->listen_fds[i] >= 0) {
			close(srv->listen_fds[i]);
			srv->listen_fds[i] = -1;
		}
	}
	srv->listen_count = 0;
	srv->listen_fd = -1;
}

static void accept_clients(struct nes_http *srv, int listener,
	struct client *clients)
{
	unsigned accepted = 0;

	while (accepted++ < ACCEPT_BUDGET) {
		int fd = accept(listener, NULL, NULL);
		int slot = -1;
		int i;

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		srv->last_activity_ms = now_ms();
		if (set_nonblock(fd) != 0) {
			close(fd);
			continue;
		}
		{
			int send_buffer = HTTP_SOCKET_SNDBUF;

			(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer,
				sizeof(send_buffer));
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			static const char busy[] =
				"HTTP/1.1 503 Service Unavailable\r\n"
				"Connection: close\r\nContent-Length: 0\r\n\r\n";
			/* Best effort only: a busy peer must not stall active gameplay. */
			(void)send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
			close(fd);
		} else {
			client_init(&clients[slot], fd);
		}
	}
	(void)srv;
}

int http_serve(struct nes_http *srv)
{
	struct client *clients;
	struct nes_jpeg_worker jpeg_worker;
	struct video_pacer video_pacer = {
		.last_video_id = UINT64_MAX,
	};
	int jpeg_worker_ready = 0;
	int idle_exit = 0;
	int i;

	if (!srv || !srv->host || srv->listen_count <= 0) {
		errno = EINVAL;
		return -1;
	}
	clients = calloc(MAX_CLIENTS, sizeof(*clients));
	if (!clients)
		return -1;
	for (i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
		clients[i].upload_fd = -1;
		clients[i].upload_dir_fd = -1;
	}
	srv->jpeg_worker = NULL;
	jpeg_worker_ready = jpeg_worker_enable_optional(srv, &jpeg_worker,
		jpeg_worker_init);
	srv->last_activity_ms = now_ms();
	while (!srv->stop) {
		struct pollfd pollfds[NES_HTTP_MAX_LISTENERS + MAX_CLIENTS + 1];
		int client_poll[MAX_CLIENTS];
		int jpeg_poll = -1;
		int poll_count = 0;
		int stream_viewers;
		int stream_fps;
		int media_active;
		int poll_timeout_ms;
		int clients_destroyed = 0;
		int result;
		uint64_t now = now_ms();

		if (expire_input_leases(clients, MAX_CLIENTS, now))
			recompute_inputs(srv, clients, MAX_CLIENTS);

		for (i = 0; i < srv->listen_count; i++) {
			if (srv->listen_fds[i] >= 0) {
				pollfds[poll_count].fd = srv->listen_fds[i];
				pollfds[poll_count].events = POLLIN;
				pollfds[poll_count].revents = 0;
				poll_count++;
			}
		}
		if (jpeg_worker_ready) {
			jpeg_poll = poll_count;
			pollfds[poll_count].fd = jpeg_worker.wake_read_fd;
			pollfds[poll_count].events = POLLIN;
			pollfds[poll_count].revents = 0;
			poll_count++;
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			client_poll[i] = -1;
			if (clients[i].fd < 0)
				continue;
			if (clients[i].out && clients[i].out_kind == WS_OUT_HTTP &&
			    (now - clients[i].out_progress_ms >=
				    HTTP_OUTPUT_STALL_TIMEOUT_MS ||
			     now - clients[i].out_since_ms >= HTTP_OUTPUT_MAX_AGE_MS)) {
				websocket_clear_active(&clients[i]);
				clients[i].dead = 1;
			}
			if (clients[i].dead) {
				if (clients[i].out &&
				    clients[i].out_kind == WS_OUT_HTTP) {
					client_poll[i] = poll_count;
					pollfds[poll_count].fd = clients[i].fd;
					pollfds[poll_count].events = POLLOUT;
					pollfds[poll_count].revents = 0;
					poll_count++;
				}
				continue;
			}
			if (clients[i].phase != CLIENT_WEBSOCKET &&
			    now >= clients[i].deadline_ms) {
				size_t declared_length = clients[i].content_length;
				size_t received_length = clients[i].body_got;
				size_t drain_limit = REJECT_DRAIN_MAX;

				if (clients[i].phase == CLIENT_HTTP_DISCARD) {
					clients[i].dead = 1;
					continue;
				}
				if (clients[i].phase == CLIENT_HTTP_HEADERS) {
					declared_length = HTTP_HEADER_MAX;
					received_length = clients[i].in_len;
					drain_limit = HTTP_HEADER_MAX;
				} else if (clients[i].phase == CLIENT_HTTP_UPLOAD) {
					received_length = clients[i].upload_got;
					abort_upload(&clients[i]);
				} else if (clients[i].phase == CLIENT_HTTP_BODY) {
					free(clients[i].body);
					clients[i].body = NULL;
				}
				if (!clients[i].dead) {
					http_error(srv, &clients[i], 408,
						"request deadline exceeded");
					begin_http_discard_lengths(&clients[i],
						declared_length, received_length,
						drain_limit, ERROR_DRAIN_TIMEOUT_MS);
				}
				continue;
			}
			if (clients[i].phase == CLIENT_WEBSOCKET) {
				websocket_expire_stale_media(&clients[i], now);
				if (!clients[i].closing &&
				    clients[i].application_heartbeat_required &&
				    now && clients[i].last_input_ms &&
				    now >= clients[i].last_input_ms &&
				    now - clients[i].last_input_ms >=
					    WS_APPLICATION_HEARTBEAT_TIMEOUT_MS)
					websocket_close(&clients[i], 1001);
				if (!clients[i].closing &&
				    now - clients[i].last_rx_ms >= WS_IDLE_TIMEOUT_MS)
					websocket_close(&clients[i], 1001);
				if (!clients[i].closing &&
				    clients[i].ping_outstanding &&
				    now - clients[i].ping_sent_ms >= WS_PONG_TIMEOUT_MS)
					websocket_close(&clients[i], 1001);
				if (!clients[i].closing &&
				    !clients[i].ping_queued &&
				    !clients[i].ping_outstanding &&
				    now - clients[i].last_ping_ms >= WS_PING_INTERVAL_MS) {
					unsigned j;
					int queued;

					for (j = 0; j < sizeof(clients[i].ping_payload); j++)
						clients[i].ping_payload[j] =
							(uint8_t)(now >> (j * 8));
					queued = websocket_queue(&clients[i], 0x9,
						clients[i].ping_payload,
						sizeof(clients[i].ping_payload), 1);
					/* Avoid allocation churn if another control frame won. */
					clients[i].last_ping_ms = now;
					if (queued != 0)
						clients[i].ping_queued = 0;
				}
				if (websocket_output_expired(&clients[i], now))
					clients[i].dead = 1;
				/*
				 * Keep reading after our Close has flushed: RFC 6455's
				 * closing handshake ends when the peer Close arrives, not
				 * merely when the local send queue becomes empty. A fixed
				 * post-flush deadline still bounds silent or broken peers.
				 */
				if (clients[i].closing && clients[i].close_frame_sent &&
				    (clients[i].close_frame_received ||
				     (clients[i].close_deadline_ms &&
				      now >= clients[i].close_deadline_ms)))
					clients[i].dead = 1;
			}
			if (clients[i].dead)
				continue;
			client_poll[i] = poll_count;
			pollfds[poll_count].fd = clients[i].fd;
			pollfds[poll_count].events = POLLIN |
				(clients[i].out ? POLLOUT : 0);
			pollfds[poll_count].revents = 0;
			poll_count++;
		}

		/*
		 * Socket readiness wakes poll immediately for input and output.
		 * Otherwise wake at the next video deadline, with a bounded fallback
		 * that also drains PCM before AUDIO_PULL_FRAMES can accumulate.
		 */
		stream_viewers = websocket_client_count(clients, MAX_CLIENTS);
		stream_fps = effective_stream_fps(srv);
		media_active = stream_viewers && host_is_running(srv->host) &&
			!host_is_paused(srv->host);
		poll_timeout_ms = video_pacer_poll_timeout_ms(&video_pacer,
			stream_fps, stream_viewers,
			media_active ? WS_MEDIA_POLL_MAX_MS : IDLE_POLL_MAX_MS);
		result = poll(pollfds, (nfds_t)poll_count, poll_timeout_ms);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 0; i < srv->listen_count; i++) {
			if (i < poll_count && (pollfds[i].revents & POLLIN))
				accept_clients(srv, pollfds[i].fd, clients);
		}
		if (jpeg_poll >= 0 && (pollfds[jpeg_poll].revents & POLLIN))
			jpeg_worker_drain_wake(&jpeg_worker);
		for (i = 0; i < MAX_CLIENTS; i++) {
			int index = client_poll[i];
			short events;

			if (index < 0 || clients[i].fd < 0 ||
			    (clients[i].dead &&
			     !(clients[i].out && clients[i].out_kind == WS_OUT_HTTP)))
				continue;
			events = pollfds[index].revents;
			if (!clients[i].dead && (events & POLLIN)) {
				switch (clients[i].phase) {
				case CLIENT_HTTP_HEADERS:
					read_http_headers(srv, &clients[i], clients, MAX_CLIENTS);
					break;
				case CLIENT_HTTP_BODY:
					read_http_body(srv, &clients[i], clients, MAX_CLIENTS);
					break;
				case CLIENT_HTTP_UPLOAD:
					read_upload(srv, &clients[i], clients, MAX_CLIENTS);
					break;
				case CLIENT_HTTP_DISCARD:
					read_http_discard(&clients[i]);
					break;
				case CLIENT_WEBSOCKET:
					read_websocket(srv, &clients[i], clients, MAX_CLIENTS);
					break;
				}
			}
			if (clients[i].dead &&
			    !(clients[i].out && clients[i].out_kind == WS_OUT_HTTP))
				continue;
			if (events & POLLOUT) {
				if (websocket_flush(&clients[i]) != 0) {
					websocket_clear_active(&clients[i]);
					clients[i].dead = 1;
				}
			}
			if (!clients[i].out && clients[i].http_shutdown_after_flush) {
				(void)shutdown(clients[i].fd, SHUT_WR);
				clients[i].http_shutdown_after_flush = 0;
			}
			if (events & (POLLERR | POLLNVAL)) {
				websocket_clear_active(&clients[i]);
				clients[i].dead = 1;
			}
			if ((events & POLLHUP) && !(events & POLLIN)) {
				websocket_clear_active(&clients[i]);
				clients[i].dead = 1;
			}
		}
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0 && clients[i].dead &&
			    !(clients[i].out && clients[i].out_kind == WS_OUT_HTTP)) {
				client_destroy(&clients[i]);
				clients_destroyed = 1;
			}
		}
		/*
		 * Closing WebSockets stop contributing immediately, even while their
		 * close frame is still being flushed. This prevents a held button
		 * from surviving a peer close or protocol error.
		 */
		recompute_inputs(srv, clients, MAX_CLIENTS);
		broadcast_av(srv, clients, MAX_CLIENTS, &video_pacer, stream_fps);
		if (srv->idle_exit_seconds > 0) {
			int connected = 0;
			int game_loaded = atomic_load_explicit(
				&srv->host->game_loaded, memory_order_acquire);
			uint64_t checked_at = now_ms();
			uint64_t idle_limit =
				(uint64_t)srv->idle_exit_seconds * 1000ull;

			for (i = 0; i < MAX_CLIENTS; i++) {
				if (clients[i].fd >= 0 && !clients[i].dead) {
					connected = 1;
					break;
				}
			}
			/*
			 * A long-lived HTTP/WebSocket client gets a full quiet period
			 * after disconnecting; time spent connected is never counted
			 * towards automatic shutdown. A loaded game is persistent
			 * state and likewise keeps the idle deadline refreshed.
			 */
			if ((connected || game_loaded || clients_destroyed) &&
			    checked_at != 0) {
				srv->last_activity_ms = checked_at;
			} else if (checked_at != 0 &&
			    srv->last_activity_ms != 0 &&
			    checked_at >= srv->last_activity_ms &&
			    checked_at - srv->last_activity_ms >= idle_limit) {
				fprintf(stderr,
					"nesd: idle timeout reached; exiting\n");
				idle_exit = 1;
				break;
			}
		}
	}
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0)
			client_destroy(&clients[i]);
	}
	host_set_joy_mask(srv->host, 0, 0);
	host_set_joy_mask(srv->host, 1, 0);
	host_set_viewers(srv->host, 0);
	close_listeners(srv);
	if (jpeg_worker_ready) {
		jpeg_worker_set_session(&jpeg_worker, 0);
		jpeg_worker_destroy(&jpeg_worker);
		srv->jpeg_worker = NULL;
	}
	free(clients);
	return (srv->stop || idle_exit) ? 0 : -1;
}
