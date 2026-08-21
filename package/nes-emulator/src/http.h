#ifndef NES_HTTP_H
#define NES_HTTP_H

#include <signal.h>

#include "host.h"

#define NES_MAX_ROM_ROOTS 16
#define NES_HTTP_MAX_LISTENERS 2
#define NES_HTTP_AUTH_TOKEN_MAX 128
#define NES_HTTP_ORIGIN_MAX 256
#define NES_STREAM_FPS_MIN 1
#define NES_STREAM_FPS_MAX 60
#define NES_STREAM_FPS_DEFAULT 2

/* WS binary frame types (all rendering/encoding on the router CPU) */
#define NES_PKT_VIDEO_RAW  1 /* RGB565 — debug / low-latency LAN */
#define NES_PKT_AUDIO_PCM  2 /* int16 LE interleaved stereo */
#define NES_PKT_VIDEO_JPEG 3 /* software JPEG, browser only blits */

struct nes_jpeg_worker;

struct nes_stream_opts {
	int jpeg_quality; /* 1..100, default 92 */
	int stream_fps;   /* browser stream only; range 1..60, default 2 */
	int use_jpeg;     /* 1=JPEG, 0=raw RGB565 (default) */
	int show_fps;     /* 1=show, 0=hide; NULL opts default to show */
	int show_touch_controls; /* 1=show, 0=hide; NULL opts default to show */
};

struct nes_http {
	struct nes_host *host;
	char bind_addr[64];
	int port;
	int listen_fd;
	int listen_fds[NES_HTTP_MAX_LISTENERS];
	int listen_count;
	char rom_dir[NES_PATH_MAX];
	char rom_roots[NES_MAX_ROM_ROOTS][NES_PATH_MAX];
	int rom_root_count;
	struct nes_stream_opts stream;
	/*
	 * Empty auth_token explicitly disables HTTP authentication. Production
	 * callers should set it after http_start() and before http_serve().
	 * allowed_origin is exact (scheme + authority) and never interpreted as
	 * a wildcard. Empty means same-origin requests only and emits no CORS
	 * response headers.
	 */
	char auth_token[NES_HTTP_AUTH_TOKEN_MAX];
	char allowed_origin[NES_HTTP_ORIGIN_MAX];
	size_t max_upload_bytes;
	uint64_t rom_quota_bytes;
	uint64_t min_free_bytes;
	unsigned idle_exit_seconds;
	uint64_t last_activity_ms;
	volatile sig_atomic_t stop;
	/* Private to http.c while http_serve() owns the encoder thread. */
	struct nes_jpeg_worker *jpeg_worker;
};

int http_start(struct nes_http *srv, struct nes_host *host,
	const char *bind_addr, int port, const char *rom_dir,
	const char *extra_rom_dirs, const struct nes_stream_opts *opts);
/* Call after http_start() and before http_serve(). */
int http_set_security(struct nes_http *srv, const char *auth_token,
	const char *allowed_origin);
void http_set_storage_limits(struct nes_http *srv, size_t max_upload_bytes,
	uint64_t rom_quota_bytes, uint64_t min_free_bytes);
/* Zero disables idle exit; non-zero values are limited to 1..86400 seconds. */
int http_set_idle_exit(struct nes_http *srv, unsigned seconds);
/* Async-signal-safe stop request; http_serve() closes listeners on exit. */
void http_stop(struct nes_http *srv);
int http_serve(struct nes_http *srv);

#endif
