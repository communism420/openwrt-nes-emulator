#define _GNU_SOURCE
#include "host.h"
#include "http.h"
#include "rpc_client.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define NESD_VERSION "1.0.0"

static struct nes_http g_http;

enum nesd_exit_code {
	NESD_EXIT_TOKEN = 64,
	NESD_EXIT_DATA = 65,
	NESD_EXIT_RESOURCE = 66,
	NESD_EXIT_ADDRESS_IN_USE = 67,
	NESD_EXIT_BIND_PERMISSION = 68,
	NESD_EXIT_HTTP = 69,
	NESD_EXIT_SECURITY = 70,
	NESD_EXIT_SIGNAL = 71
};

enum {
	OPT_RPC_CLIENT = 1000,
	OPT_RPC_HOST,
	OPT_RPC_PORT,
	OPT_RPC_PATH,
	OPT_RPC_METHOD,
	OPT_RPC_BODY,
	OPT_RPC_TOKEN_FILE,
	OPT_RPC_TIMEOUT_MS,
	OPT_IDLE_EXIT_SECONDS,
	OPT_SHOW_FPS,
	OPT_HIDE_FPS,
	OPT_SHOW_TOUCH_CONTROLS,
	OPT_HIDE_TOUCH_CONTROLS
};

static void on_signal(int signal_number)
{
	(void)signal_number;
	/*
	 * The signal interrupts poll, and this sig_atomic_t-compatible flag is
	 * observed at the loop boundary.
	 * Listener cleanup is deliberately left to normal process context.
	 */
	g_http.stop = 1;
}

static int install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) != 0 ||
	    sigaction(SIGTERM, &action, NULL) != 0)
		return -1;

	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_IGN;
	sigemptyset(&action.sa_mask);
	return sigaction(SIGPIPE, &action, NULL);
}

static int ignore_sigpipe(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_IGN;
	sigemptyset(&action.sa_mask);
	return sigaction(SIGPIPE, &action, NULL);
}

static int parse_int(const char *text, int minimum, int maximum, int *out)
{
	char *end = NULL;
	long value;

	if (!text || !*text || !out) {
		errno = EINVAL;
		return -1;
	}
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || !end || *end || value < minimum || value > maximum) {
		errno = EINVAL;
		return -1;
	}
	*out = (int)value;
	return 0;
}

static int http_start_exit_code(int error)
{
	if (error == EADDRINUSE)
		return NESD_EXIT_ADDRESS_IN_USE;
	if (error == EACCES || error == EPERM)
		return NESD_EXIT_BIND_PERMISSION;
	if (error == ENOMEM)
		return NESD_EXIT_RESOURCE;
	return NESD_EXIT_HTTP;
}

static int ensure_directory(const char *path)
{
	char copy[PATH_MAX];
	struct stat status;
	size_t length;
	char *cursor;

	if (!path || path[0] != '/' || strlen(path) >= sizeof(copy)) {
		errno = EINVAL;
		return -1;
	}
	strcpy(copy, path);
	length = strlen(copy);
	while (length > 1 && copy[length - 1] == '/')
		copy[--length] = '\0';
	if (strcmp(copy, "/") == 0 || strcmp(copy, "/bin") == 0 ||
	    strcmp(copy, "/boot") == 0 || strcmp(copy, "/dev") == 0 ||
	    strcmp(copy, "/etc") == 0 || strcmp(copy, "/home") == 0 ||
	    strcmp(copy, "/lib") == 0 || strcmp(copy, "/lib64") == 0 ||
	    strcmp(copy, "/mnt") == 0 || strcmp(copy, "/opt") == 0 ||
	    strcmp(copy, "/overlay") == 0 || strcmp(copy, "/proc") == 0 ||
	    strcmp(copy, "/root") == 0 || strcmp(copy, "/run") == 0 ||
	    strcmp(copy, "/sbin") == 0 || strcmp(copy, "/sys") == 0 ||
	    strcmp(copy, "/tmp") == 0 || strcmp(copy, "/usr") == 0 ||
	    strcmp(copy, "/var") == 0) {
		errno = EINVAL;
		return -1;
	}
	if (strstr(copy, "/../") || strstr(copy, "/./") ||
	    strcmp(copy + (length >= 3 ? length - 3 : 0), "/..") == 0 ||
	    strcmp(copy + (length >= 2 ? length - 2 : 0), "/.") == 0 ||
	    strstr(copy, "//")) {
		errno = EINVAL;
		return -1;
	}

	for (cursor = copy + 1; ; cursor++) {
		if (*cursor != '/' && *cursor != '\0')
			continue;
		{
			char saved = *cursor;

			*cursor = '\0';
			if (lstat(copy, &status) != 0) {
				if (errno != ENOENT || mkdir(copy, 0750) != 0)
					return -1;
			} else if (!S_ISDIR(status.st_mode) ||
				   S_ISLNK(status.st_mode)) {
				errno = ENOTDIR;
				return -1;
			}
			*cursor = saved;
			if (saved == '\0')
				break;
		}
	}
	return 0;
}

static bool token_valid(const char *token)
{
	size_t length;
	const unsigned char *cursor;

	if (!token)
		return false;
	length = strlen(token);
	if (length < 32 || length >= NES_HTTP_AUTH_TOKEN_MAX)
		return false;
	for (cursor = (const unsigned char *)token; *cursor; cursor++) {
		if (!(('a' <= *cursor && *cursor <= 'z') ||
		      ('A' <= *cursor && *cursor <= 'Z') ||
		      ('0' <= *cursor && *cursor <= '9') ||
		      strchr("._~-", *cursor)))
			return false;
	}
	return true;
}

static bool loopback_address(const char *address)
{
	return address &&
	       (strcmp(address, "127.0.0.1") == 0 ||
		strcmp(address, "::1") == 0 ||
		strcmp(address, "localhost") == 0);
}

static int read_auth_token_file(const char *path, char *out, size_t out_size)
{
	char raw[NES_HTTP_AUTH_TOKEN_MAX + 1];
	struct stat status;
	size_t done = 0;
	ssize_t got;
	int fd;
	int saved_errno;

	if (!path || !path[0] || !out || out_size < 2) {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) != 0) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
	    status.st_uid != 0 ||
	    (status.st_mode & (S_ISUID | S_ISGID | S_ISVTX | S_IXUSR |
			      S_IWGRP | S_IXGRP | S_IRWXO)) != 0 ||
	    (geteuid() != 0 &&
	     (status.st_gid != getegid() ||
	      (status.st_mode & S_IRGRP) == 0)) ||
	    status.st_size < 2 ||
	    (uint64_t)status.st_size > NES_HTTP_AUTH_TOKEN_MAX) {
		close(fd);
		errno = EPERM;
		return -1;
	}
	while (done < (size_t)status.st_size) {
		got = read(fd, raw + done, (size_t)status.st_size - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			saved_errno = got < 0 && errno ? errno : EIO;
			close(fd);
			memset(raw, 0, sizeof(raw));
			errno = saved_errno;
			return -1;
		}
		done += (size_t)got;
	}
	do {
		got = read(fd, raw + done, 1);
	} while (got < 0 && errno == EINTR);
	if (got != 0) {
		saved_errno = got < 0 && errno ? errno : EIO;
		close(fd);
		memset(raw, 0, sizeof(raw));
		errno = saved_errno;
		return -1;
	}
	if (close(fd) != 0) {
		saved_errno = errno;
		memset(raw, 0, sizeof(raw));
		errno = saved_errno;
		return -1;
	}
	if (done < 2 || raw[done - 1] != '\n' ||
	    done - 1 >= out_size) {
		memset(raw, 0, sizeof(raw));
		errno = EINVAL;
		return -1;
	}
	raw[done - 1] = '\0';
	if (!token_valid(raw)) {
		memset(raw, 0, sizeof(raw));
		errno = EINVAL;
		return -1;
	}
	memcpy(out, raw, done);
	memset(raw, 0, sizeof(raw));
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  OpenWrt NES host with statically linked FCEUmm.\n"
		"  Emulation, PPU rendering and JPEG encoding run on the router CPU.\n\n"
		"  -r, --rom PATH          load ROM on startup\n"
		"  -d, --rom-dir DIR       primary ROM directory\n"
		"  -e, --extra-rom-dirs S  extra scan roots separated by ':' or ';'\n"
		"  -b, --bind ADDR         bind address (default 127.0.0.1)\n"
		"  -p, --port N            HTTP/WebSocket port (default 29876)\n"
		"  -s, --system DIR        system directory\n"
		"  -S, --save DIR          save/SRAM directory\n"
		"  -q, --jpeg-quality N    JPEG quality 1-100 (default 92)\n"
		"  -f, --stream-fps N      streamed FPS limit 1-60 (default 2)\n"
		"      --show-fps          show measured browser paint FPS (default)\n"
		"      --hide-fps          hide the browser FPS counter\n"
		"      --show-touch-controls\n"
		"                          show on-screen pointer controls (default)\n"
		"      --hide-touch-controls\n"
		"                          hide on-screen pointer controls\n"
		"      --idle-exit-seconds N\n"
		"                          exit after 1-86400 idle seconds (default off)\n"
		"      --raw-video         stream little-endian RGB565 (default)\n"
		"      --jpeg-video        software-encode JPEG on the router\n"
		"      --auth-token TOKEN  development token; loopback only\n"
		"      --auth-token-file F read token from protected file\n"
		"      --allowed-origin O  optional exact cross-origin client Origin\n"
		"      --insecure-no-auth  development only; requires loopback bind\n"
		"      --allow-demo        keep demo mode if built-in FCEUmm fails\n"
		"\n"
		"  Internal loopback RPC client (used by rpcd):\n"
		"      --rpc-client\n"
		"      --rpc-host ADDR     numeric address assigned to this router\n"
		"      --rpc-port N\n"
		"      --rpc-path PATH     origin-form HTTP path\n"
		"      --rpc-method M      GET or POST\n"
		"      --rpc-body JSON     POST body (maximum 64 KiB)\n"
		"      --rpc-token-file F  read bearer token from protected file\n"
		"      --rpc-timeout-ms N  total deadline 100..10000 ms\n"
		"      --version\n"
		"  -h, --help\n",
		program);
}

int main(int argc, char **argv)
{
	const char *rom = NULL;
	const char *rom_dir = "/etc/nes-emulator/roms";
	const char *extra_rom_dirs = "";
	const char *bind_address = "127.0.0.1";
	const char *system_dir = "/etc/nes-emulator/system";
	const char *save_dir = "/etc/nes-emulator/saves";
	const char *auth_token = NULL;
	const char *auth_token_file = NULL;
	char *auth_token_argument = NULL;
	const char *allowed_origin = "";
	const char *rpc_host = NULL;
	const char *rpc_path = NULL;
	const char *rpc_method = NULL;
	const char *rpc_body = "";
	const char *rpc_token_file = NULL;
	char auth_token_buffer[NES_HTTP_AUTH_TOKEN_MAX] = { 0 };
	bool insecure_no_auth = false;
	bool allow_demo = false;
	bool rpc_client_mode = false;
	bool rpc_option_seen = false;
	bool server_option_seen = false;
	int port = 29876;
	int rpc_port = 0;
	int rpc_timeout_ms = 3000;
	int idle_exit_seconds = 0;
	int rpc_result;
	int rpc_errno;
	int serve_result;
	int serve_errno;
	int shutdown_result;
	int shutdown_errno;
	int security_result;
	bool startup_game_loaded = false;
	struct nes_host *host = NULL;
	struct nes_stream_opts stream = {
		.jpeg_quality = 92,
		.stream_fps = NES_STREAM_FPS_DEFAULT,
		.use_jpeg = 0,
		.show_fps = 1,
		.show_touch_controls = 1,
	};

	static const struct option options[] = {
		{"core", required_argument, NULL, 'c'},
		{"rom", required_argument, NULL, 'r'},
		{"rom-dir", required_argument, NULL, 'd'},
		{"extra-rom-dirs", required_argument, NULL, 'e'},
		{"bind", required_argument, NULL, 'b'},
		{"port", required_argument, NULL, 'p'},
		{"system", required_argument, NULL, 's'},
		{"save", required_argument, NULL, 'S'},
		{"jpeg-quality", required_argument, NULL, 'q'},
		{"stream-fps", required_argument, NULL, 'f'},
		{"raw-video", no_argument, NULL, 'R'},
		{"jpeg-video", no_argument, NULL, 'J'},
		{"auth-token", required_argument, NULL, 'A'},
		{"auth-token-file", required_argument, NULL, 'F'},
		{"allowed-origin", required_argument, NULL, 'O'},
		{"insecure-no-auth", no_argument, NULL, 'I'},
		{"allow-demo", no_argument, NULL, 'D'},
		{"rpc-client", no_argument, NULL, OPT_RPC_CLIENT},
		{"rpc-host", required_argument, NULL, OPT_RPC_HOST},
		{"rpc-port", required_argument, NULL, OPT_RPC_PORT},
		{"rpc-path", required_argument, NULL, OPT_RPC_PATH},
		{"rpc-method", required_argument, NULL, OPT_RPC_METHOD},
		{"rpc-body", required_argument, NULL, OPT_RPC_BODY},
		{"rpc-token-file", required_argument, NULL, OPT_RPC_TOKEN_FILE},
		{"rpc-timeout-ms", required_argument, NULL, OPT_RPC_TIMEOUT_MS},
		{"idle-exit-seconds", required_argument, NULL,
		 OPT_IDLE_EXIT_SECONDS},
		{"show-fps", no_argument, NULL, OPT_SHOW_FPS},
		{"hide-fps", no_argument, NULL, OPT_HIDE_FPS},
		{"show-touch-controls", no_argument, NULL,
		 OPT_SHOW_TOUCH_CONTROLS},
		{"hide-touch-controls", no_argument, NULL,
		 OPT_HIDE_TOUCH_CONTROLS},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	for (;;) {
		int option = getopt_long(argc, argv,
			"c:r:d:e:b:p:s:S:q:f:RJA:F:O:IDVh", options, NULL);

		if (option < 0)
			break;
		switch (option) {
		case 'c':
			server_option_seen = true;
			if (strcmp(optarg, "builtin") != 0) {
				fprintf(stderr,
					"nesd: external cores are unsupported; "
					"FCEUmm is statically linked\n");
				return 2;
			}
			break;
		case 'r': server_option_seen = true; rom = optarg; break;
		case 'd': server_option_seen = true; rom_dir = optarg; break;
		case 'e': server_option_seen = true; extra_rom_dirs = optarg; break;
		case 'b': server_option_seen = true; bind_address = optarg; break;
		case 'p':
			server_option_seen = true;
			if (parse_int(optarg, 1, 65535, &port) != 0)
				goto bad_option;
			break;
		case 's': server_option_seen = true; system_dir = optarg; break;
		case 'S': server_option_seen = true; save_dir = optarg; break;
		case 'q':
			server_option_seen = true;
			if (parse_int(optarg, 1, 100, &stream.jpeg_quality) != 0)
				goto bad_option;
			break;
		case 'f':
			server_option_seen = true;
			if (parse_int(optarg, NES_STREAM_FPS_MIN,
			    NES_STREAM_FPS_MAX, &stream.stream_fps) != 0)
				goto bad_option;
			break;
		case 'R': server_option_seen = true; stream.use_jpeg = 0; break;
		case 'J': server_option_seen = true; stream.use_jpeg = 1; break;
		case 'A':
			server_option_seen = true;
			auth_token = optarg;
			auth_token_argument = optarg;
			break;
		case 'F':
			server_option_seen = true;
			auth_token_file = optarg;
			break;
		case 'O':
			server_option_seen = true;
			allowed_origin = optarg;
			break;
		case 'I': server_option_seen = true; insecure_no_auth = true; break;
		case 'D': server_option_seen = true; allow_demo = true; break;
		case OPT_RPC_CLIENT:
			rpc_client_mode = true;
			rpc_option_seen = true;
			break;
		case OPT_RPC_HOST:
			rpc_host = optarg;
			rpc_option_seen = true;
			break;
		case OPT_RPC_PORT:
			rpc_option_seen = true;
			if (parse_int(optarg, 1, 65535, &rpc_port) != 0)
				goto bad_option;
			break;
		case OPT_RPC_PATH:
			rpc_path = optarg;
			rpc_option_seen = true;
			break;
		case OPT_RPC_METHOD:
			rpc_method = optarg;
			rpc_option_seen = true;
			break;
		case OPT_RPC_BODY:
			rpc_body = optarg;
			rpc_option_seen = true;
			break;
		case OPT_RPC_TOKEN_FILE:
			rpc_token_file = optarg;
			rpc_option_seen = true;
			break;
		case OPT_RPC_TIMEOUT_MS:
			rpc_option_seen = true;
			if (parse_int(optarg, 100, 10000,
				      &rpc_timeout_ms) != 0)
				goto bad_option;
			break;
		case OPT_IDLE_EXIT_SECONDS:
			server_option_seen = true;
			if (parse_int(optarg, 1, 86400,
				      &idle_exit_seconds) != 0)
				goto bad_option;
			break;
		case OPT_SHOW_FPS:
			server_option_seen = true;
			stream.show_fps = 1;
			break;
		case OPT_HIDE_FPS:
			server_option_seen = true;
			stream.show_fps = 0;
			break;
		case OPT_SHOW_TOUCH_CONTROLS:
			server_option_seen = true;
			stream.show_touch_controls = 1;
			break;
		case OPT_HIDE_TOUCH_CONTROLS:
			server_option_seen = true;
			stream.show_touch_controls = 0;
			break;
		case 'V':
			printf("nesd %s\n", NESD_VERSION);
			return 0;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (optind != argc) {
		fprintf(stderr, "nesd: unexpected positional argument: %s\n",
			argv[optind]);
		return 2;
	}
	if (rpc_option_seen) {
		if (!rpc_client_mode || server_option_seen || !rpc_host ||
		    rpc_port == 0 || !rpc_path || !rpc_method ||
		    !rpc_token_file) {
			fprintf(stderr,
				"nesd: incomplete or mixed loopback RPC client options\n");
			return 2;
		}
		if (read_auth_token_file(rpc_token_file, auth_token_buffer,
			sizeof(auth_token_buffer)) != 0) {
			fprintf(stderr,
				"nesd: cannot read secure RPC token file: %s\n",
				strerror(errno));
			return 1;
		}
		if (ignore_sigpipe() != 0) {
			rpc_errno = errno;
			memset(auth_token_buffer, 0,
			       sizeof(auth_token_buffer));
			fprintf(stderr,
				"nesd: cannot prepare loopback RPC client: %s\n",
				strerror(rpc_errno));
			return 1;
		}
		rpc_result = rpc_client_request(rpc_host, rpc_port, rpc_path,
			rpc_method, rpc_body, auth_token_buffer,
			rpc_timeout_ms);
		rpc_errno = errno;
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		if (rpc_result < 0) {
			fprintf(stderr, "nesd: loopback RPC failed: %s\n",
				strerror(rpc_errno ? rpc_errno : EIO));
			return 1;
		}
		return rpc_result;
	}
	if (auth_token && auth_token_file) {
		fprintf(stderr,
			"nesd: choose either --auth-token or --auth-token-file\n");
		return NESD_EXIT_TOKEN;
	}
	if ((auth_token || auth_token_file) && insecure_no_auth) {
		fprintf(stderr,
			"nesd: authentication and --insecure-no-auth are exclusive\n");
		return NESD_EXIT_TOKEN;
	}
	if (auth_token_file) {
		if (read_auth_token_file(auth_token_file, auth_token_buffer,
			sizeof(auth_token_buffer)) != 0) {
			fprintf(stderr, "nesd: cannot read secure auth token file: %s\n",
				strerror(errno));
			memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
			return NESD_EXIT_TOKEN;
		}
		auth_token = auth_token_buffer;
	}
	if (!auth_token && !insecure_no_auth) {
		fprintf(stderr,
			"nesd: --auth-token or --auth-token-file is required (or use "
			"--insecure-no-auth on loopback for development)\n");
		return NESD_EXIT_TOKEN;
	}
	if (auth_token && !token_valid(auth_token)) {
		fprintf(stderr,
			"nesd: auth token must contain 32..127 URL-safe characters\n");
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		return NESD_EXIT_TOKEN;
	}
	if (auth_token_argument) {
		size_t token_length = strlen(auth_token_argument);

		if (!loopback_address(bind_address)) {
			fprintf(stderr,
				"nesd: --auth-token is development-only and restricted "
				"to loopback; use --auth-token-file on LAN\n");
			memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
			return NESD_EXIT_SECURITY;
		}
		memcpy(auth_token_buffer, auth_token_argument, token_length + 1);
		memset(auth_token_argument, 0, token_length);
		auth_token = auth_token_buffer;
	}
	if (insecure_no_auth && !loopback_address(bind_address)) {
		fprintf(stderr,
			"nesd: unauthenticated mode is restricted to loopback\n");
		return NESD_EXIT_SECURITY;
	}

	umask(0027);
	if (ensure_directory(rom_dir) != 0 ||
	    ensure_directory(system_dir) != 0 ||
	    ensure_directory(save_dir) != 0) {
		fprintf(stderr, "nesd: cannot prepare data directories: %s\n",
			strerror(errno));
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		return NESD_EXIT_DATA;
	}

	host_apply_router_limits();
	host = calloc(1, sizeof(*host));
	if (!host) {
		fprintf(stderr, "nesd: cannot allocate emulator host: %s\n",
			strerror(errno));
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		return NESD_EXIT_RESOURCE;
	}
	/*
	 * Keep installation/boot cheap: the statically linked core is initialized
	 * lazily by host_load_game(). --allow-demo remains an explicit development
	 * opt-in and is never used by the OpenWrt service.
	 */
	if (host_init(host, allow_demo ? "builtin" : NULL,
		      system_dir, save_dir) != 0) {
		fprintf(stderr, "nesd: host initialization failed: %s\n",
			strerror(errno));
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		free(host);
		return NESD_EXIT_RESOURCE;
	}
	if (rom && rom[0]) {
		if (host_load_game(host, rom) != 0) {
			fprintf(stderr,
				"nesd: failed to load startup ROM %s; "
				"continuing without a loaded game\n",
				rom);
		} else {
			startup_game_loaded = true;
		}
	}
	if ((allow_demo || startup_game_loaded) && host_start(host) != 0) {
		int host_errno = errno ? errno : EIO;

		fprintf(stderr, "nesd: failed to start emulation: %s\n",
			strerror(host_errno));
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		(void)host_shutdown(host);
		free(host);
		return NESD_EXIT_RESOURCE;
	}

	if (http_start(&g_http, host, bind_address, port, rom_dir,
		       extra_rom_dirs, &stream) != 0) {
		int http_errno = errno ? errno : EIO;

		fprintf(stderr, "nesd: HTTP server initialization failed: %s\n",
			strerror(http_errno));
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		(void)host_shutdown(host);
		free(host);
		return http_start_exit_code(http_errno);
	}
	if (http_set_idle_exit(&g_http, (unsigned)idle_exit_seconds) != 0) {
		fprintf(stderr, "nesd: invalid idle-exit setting\n");
		memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
		http_stop(&g_http);
		(void)host_shutdown(host);
		free(host);
		return NESD_EXIT_HTTP;
	}
	security_result = http_set_security(&g_http,
		insecure_no_auth ? "" : auth_token, allowed_origin);
	memset(auth_token_buffer, 0, sizeof(auth_token_buffer));
	if (security_result != 0) {
		fprintf(stderr, "nesd: invalid authentication/origin settings\n");
		http_stop(&g_http);
		(void)host_shutdown(host);
		free(host);
		return NESD_EXIT_SECURITY;
	}
	if (install_signal_handlers() != 0) {
		fprintf(stderr, "nesd: cannot install signal handlers: %s\n",
			strerror(errno));
		http_stop(&g_http);
		(void)host_shutdown(host);
		free(host);
		return NESD_EXIT_SIGNAL;
	}

	serve_result = http_serve(&g_http);
	serve_errno = serve_result == 0 ? 0 : errno;
	http_stop(&g_http);
	shutdown_result = host_shutdown(host);
	shutdown_errno = shutdown_result == 0 ? 0 : errno;
	free(host);
	if (serve_result != 0) {
		fprintf(stderr, "nesd: HTTP server failed: %s\n",
			strerror(serve_errno ? serve_errno : EIO));
		return 1;
	}
	if (shutdown_result != 0) {
		fprintf(stderr, "nesd: shutdown failed while saving SRAM: %s\n",
			strerror(shutdown_errno ? shutdown_errno : EIO));
		return 1;
	}
	return 0;

bad_option:
	fprintf(stderr, "nesd: invalid value for option %s\n",
		argv[optind - 1]);
	return 2;
}
