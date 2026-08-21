#define _POSIX_C_SOURCE 200809L

#include "rpc_client.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define RPC_HEADER_MAX (16u * 1024u)
#define RPC_LINE_MAX (8u * 1024u)
#define RPC_REQUEST_BODY_MAX (64u * 1024u)
#define RPC_RESPONSE_BODY_MAX (512u * 1024u)
#define RPC_PATH_MAX 2048u
#define RPC_IO_BUFFER_SIZE 4096u
#define RPC_MIN_TIMEOUT_MS 100
#define RPC_MAX_TIMEOUT_MS 10000
#define RPC_MAX_INTERIM_RESPONSES 4

struct rpc_reader {
	int fd;
	int64_t deadline_ms;
	unsigned char buffer[RPC_IO_BUFFER_SIZE];
	size_t offset;
	size_t length;
};

struct rpc_body {
	unsigned char *data;
	size_t length;
	size_t capacity;
};

struct rpc_response {
	int status;
	bool has_content_length;
	bool chunked;
	uint64_t content_length;
};

static int64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int remaining_ms(int64_t deadline_ms)
{
	int64_t now = monotonic_ms();
	int64_t remaining;

	if (now < 0)
		return -1;
	remaining = deadline_ms - now;
	if (remaining <= 0) {
		errno = ETIMEDOUT;
		return -1;
	}
	return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static int wait_socket(int fd, short events, int64_t deadline_ms)
{
	struct pollfd descriptor;

	descriptor.fd = fd;
	descriptor.events = events;
	descriptor.revents = 0;
	for (;;) {
		int timeout = remaining_ms(deadline_ms);
		int result;

		if (timeout < 0)
			return -1;
		result = poll(&descriptor, 1, timeout);
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0)
			return -1;
		if (result == 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (descriptor.revents & POLLNVAL) {
			errno = EBADF;
			return -1;
		}
		if (descriptor.revents & (events | POLLHUP))
			return 0;
		if (descriptor.revents & POLLERR) {
			errno = EIO;
			return -1;
		}
	}
}

static int set_nonblocking_cloexec(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
		return -1;
	flags = fcntl(fd, F_GETFD, 0);
	if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
		return -1;
	return 0;
}

static bool valid_local_host(const char *host)
{
	struct in_addr ipv4;
	struct in6_addr ipv6;
	struct ifaddrs *interfaces = NULL;
	struct ifaddrs *interface;
	const unsigned char *bytes;
	bool valid = false;

	if (!host || !host[0])
		return false;
	if (inet_pton(AF_INET, host, &ipv4) == 1) {
		bytes = (const unsigned char *)&ipv4;
		if (bytes[0] == 127)
			return true;
	} else if (inet_pton(AF_INET6, host, &ipv6) == 1) {
		if (IN6_IS_ADDR_LOOPBACK(&ipv6))
			return true;
	} else {
		return false;
	}

	/*
	 * Exact LAN binds are supported without turning this into a generic HTTP
	 * client: a numeric destination must be assigned to this router at the
	 * time of the request. Thus the bearer token cannot be sent to a remote
	 * address, and proxy/DNS configuration is never consulted.
	 */
	if (getifaddrs(&interfaces) != 0)
		return false;
	for (interface = interfaces; interface; interface = interface->ifa_next) {
		if (!interface->ifa_addr)
			continue;
		if (interface->ifa_addr->sa_family == AF_INET &&
		    inet_pton(AF_INET, host, &ipv4) == 1) {
			const struct sockaddr_in *assigned =
				(const struct sockaddr_in *)interface->ifa_addr;

			if (memcmp(&assigned->sin_addr, &ipv4,
				   sizeof(ipv4)) == 0) {
				valid = true;
				break;
			}
		}
		if (interface->ifa_addr->sa_family == AF_INET6 &&
		    inet_pton(AF_INET6, host, &ipv6) == 1) {
			const struct sockaddr_in6 *assigned =
				(const struct sockaddr_in6 *)interface->ifa_addr;

			if (memcmp(&assigned->sin6_addr, &ipv6,
				   sizeof(ipv6)) == 0) {
				valid = true;
				break;
			}
		}
	}
	freeifaddrs(interfaces);
	return valid;
}

static int connect_loopback(const char *host, int port, int64_t deadline_ms)
{
	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	struct addrinfo *address;
	char service[16];
	int error;
	int saved_errno = ECONNREFUSED;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	if (snprintf(service, sizeof(service), "%d", port) < 0) {
		errno = EINVAL;
		return -1;
	}
	error = getaddrinfo(host, service, &hints, &addresses);
	if (error != 0) {
		errno = EINVAL;
		return -1;
	}
	for (address = addresses; address; address = address->ai_next) {
		int result;
		int socket_error = 0;
		socklen_t socket_error_size = sizeof(socket_error);

		fd = socket(address->ai_family, address->ai_socktype,
			    address->ai_protocol);
		if (fd < 0) {
			saved_errno = errno;
			continue;
		}
		if (set_nonblocking_cloexec(fd) != 0) {
			saved_errno = errno;
			close(fd);
			fd = -1;
			continue;
		}
		result = connect(fd, address->ai_addr, address->ai_addrlen);
		if (result == 0)
			break;
		if (errno != EINPROGRESS ||
		    wait_socket(fd, POLLOUT, deadline_ms) != 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
			       &socket_error_size) != 0 ||
		    socket_error != 0) {
			if (socket_error)
				saved_errno = socket_error;
			else if (errno)
				saved_errno = errno;
			close(fd);
			fd = -1;
			if (saved_errno == ETIMEDOUT)
				break;
			continue;
		}
		break;
	}
	freeaddrinfo(addresses);
	if (fd < 0)
		errno = saved_errno;
	return fd;
}

static int send_all(int fd, const void *data, size_t length,
		    int64_t deadline_ms)
{
	const unsigned char *cursor = data;

	while (length) {
		ssize_t sent = send(fd, cursor, length, MSG_NOSIGNAL);

		if (sent > 0) {
			cursor += (size_t)sent;
			length -= (size_t)sent;
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (wait_socket(fd, POLLOUT, deadline_ms) != 0)
				return -1;
			continue;
		}
		if (sent == 0)
			errno = EPIPE;
		return -1;
	}
	return 0;
}

static int reader_fill(struct rpc_reader *reader)
{
	ssize_t received;

	reader->offset = 0;
	reader->length = 0;
	for (;;) {
		if (wait_socket(reader->fd, POLLIN, reader->deadline_ms) != 0)
			return -1;
		received = recv(reader->fd, reader->buffer,
				sizeof(reader->buffer), 0);
		if (received > 0) {
			reader->length = (size_t)received;
			return 1;
		}
		if (received == 0)
			return 0;
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		return -1;
	}
}

static int reader_byte(struct rpc_reader *reader, unsigned char *out)
{
	int result;

	if (reader->offset == reader->length) {
		result = reader_fill(reader);
		if (result <= 0) {
			if (result == 0)
				errno = ECONNRESET;
			return -1;
		}
	}
	*out = reader->buffer[reader->offset++];
	return 0;
}

static int reader_line(struct rpc_reader *reader, char *line, size_t line_size,
		       size_t *wire_total)
{
	size_t length = 0;

	if (!line || line_size < 2) {
		errno = EINVAL;
		return -1;
	}
	for (;;) {
		unsigned char byte;

		if (reader_byte(reader, &byte) != 0)
			return -1;
		if (wire_total) {
			(*wire_total)++;
			if (*wire_total > RPC_HEADER_MAX) {
				errno = EOVERFLOW;
				return -1;
			}
		}
		if (byte == '\0') {
			errno = EPROTO;
			return -1;
		}
		if (byte == '\n') {
			if (length == 0 || line[length - 1] != '\r') {
				errno = EPROTO;
				return -1;
			}
			line[--length] = '\0';
			return 0;
		}
		if (length + 1 >= line_size) {
			errno = EOVERFLOW;
			return -1;
		}
		line[length++] = (char)byte;
	}
}

static bool header_name_valid(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;

	if (!*cursor)
		return false;
	for (; *cursor; cursor++) {
		if (isalnum(*cursor) || strchr("!#$%&'*+-.^_`|~", *cursor))
			continue;
		return false;
	}
	return true;
}

static int parse_uint64_decimal(const char *text, uint64_t *out)
{
	uint64_t value = 0;
	const unsigned char *cursor = (const unsigned char *)text;

	if (!*cursor)
		return -1;
	for (; *cursor; cursor++) {
		unsigned digit;

		if (!isdigit(*cursor))
			return -1;
		digit = (unsigned)(*cursor - '0');
		if (value > (UINT64_MAX - digit) / 10)
			return -1;
		value = value * 10 + digit;
	}
	*out = value;
	return 0;
}

static int parse_status_line(const char *line, int *status)
{
	const char *cursor;

	if (strncmp(line, "HTTP/1.1 ", 9) == 0 ||
	    strncmp(line, "HTTP/1.0 ", 9) == 0)
		cursor = line + 9;
	else
		return -1;
	if (!isdigit((unsigned char)cursor[0]) ||
	    !isdigit((unsigned char)cursor[1]) ||
	    !isdigit((unsigned char)cursor[2]) ||
	    (cursor[3] != '\0' && cursor[3] != ' '))
		return -1;
	*status = (cursor[0] - '0') * 100 +
		  (cursor[1] - '0') * 10 + cursor[2] - '0';
	return *status >= 100 && *status <= 599 ? 0 : -1;
}

static int parse_response_headers(struct rpc_reader *reader,
				  struct rpc_response *response)
{
	char line[RPC_LINE_MAX];
	size_t wire_total = 0;

	memset(response, 0, sizeof(*response));
	if (reader_line(reader, line, sizeof(line), &wire_total) != 0 ||
	    parse_status_line(line, &response->status) != 0) {
		errno = EPROTO;
		return -1;
	}
	for (;;) {
		char *colon;
		char *name;
		char *value;
		char *end;
		uint64_t content_length;

		if (reader_line(reader, line, sizeof(line), &wire_total) != 0)
			return -1;
		if (!line[0])
			return 0;
		if (line[0] == ' ' || line[0] == '\t') {
			errno = EPROTO;
			return -1;
		}
		colon = strchr(line, ':');
		if (!colon) {
			errno = EPROTO;
			return -1;
		}
		*colon = '\0';
		name = line;
		if (!header_name_valid(name)) {
			errno = EPROTO;
			return -1;
		}
		value = colon + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		end = value + strlen(value);
		while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';

		if (strcasecmp(name, "Content-Length") == 0) {
			if (parse_uint64_decimal(value, &content_length) != 0 ||
			    (response->has_content_length &&
			     response->content_length != content_length)) {
				errno = EPROTO;
				return -1;
			}
			response->has_content_length = true;
			response->content_length = content_length;
		} else if (strcasecmp(name, "Transfer-Encoding") == 0) {
			if (response->chunked || strcasecmp(value, "chunked") != 0) {
				errno = EPROTO;
				return -1;
			}
			response->chunked = true;
		}
	}
}

static int body_reserve(struct rpc_body *body, size_t additional)
{
	size_t required;
	size_t capacity;
	unsigned char *replacement;

	if (additional > RPC_RESPONSE_BODY_MAX - body->length) {
		errno = EFBIG;
		return -1;
	}
	required = body->length + additional;
	if (required <= body->capacity)
		return 0;
	capacity = body->capacity ? body->capacity : 4096;
	while (capacity < required) {
		if (capacity >= RPC_RESPONSE_BODY_MAX / 2) {
			capacity = RPC_RESPONSE_BODY_MAX;
			break;
		}
		capacity *= 2;
	}
	replacement = realloc(body->data, capacity);
	if (!replacement)
		return -1;
	body->data = replacement;
	body->capacity = capacity;
	return 0;
}

static int reader_append_exact(struct rpc_reader *reader,
			       struct rpc_body *body, size_t length)
{
	if (body_reserve(body, length) != 0)
		return -1;
	while (length) {
		size_t available;
		size_t take;
		int result;

		if (reader->offset == reader->length) {
			result = reader_fill(reader);
			if (result <= 0) {
				if (result == 0)
					errno = ECONNRESET;
				return -1;
			}
		}
		available = reader->length - reader->offset;
		take = available < length ? available : length;
		memcpy(body->data + body->length,
		       reader->buffer + reader->offset, take);
		reader->offset += take;
		body->length += take;
		length -= take;
	}
	return 0;
}

static int reader_expect_crlf(struct rpc_reader *reader)
{
	unsigned char first;
	unsigned char second;

	if (reader_byte(reader, &first) != 0 ||
	    reader_byte(reader, &second) != 0)
		return -1;
	if (first != '\r' || second != '\n') {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

static int parse_chunk_size(const char *line, uint64_t *out)
{
	const unsigned char *cursor = (const unsigned char *)line;
	uint64_t value = 0;
	bool saw_digit = false;

	while (isxdigit(*cursor)) {
		unsigned digit;

		saw_digit = true;
		if (isdigit(*cursor))
			digit = (unsigned)(*cursor - '0');
		else
			digit = (unsigned)(tolower(*cursor) - 'a' + 10);
		if (value > (UINT64_MAX - digit) / 16)
			return -1;
		value = value * 16 + digit;
		cursor++;
	}
	if (!saw_digit || (*cursor != '\0' && *cursor != ';'))
		return -1;
	for (; *cursor; cursor++) {
		if (*cursor < 0x20 || *cursor == 0x7f)
			return -1;
	}
	*out = value;
	return 0;
}

static int read_chunked_body(struct rpc_reader *reader, struct rpc_body *body)
{
	char line[RPC_LINE_MAX];

	for (;;) {
		uint64_t chunk_size;
		size_t trailer_total = 0;

		if (reader_line(reader, line, sizeof(line), NULL) != 0 ||
		    parse_chunk_size(line, &chunk_size) != 0) {
			errno = EPROTO;
			return -1;
		}
		if (chunk_size > RPC_RESPONSE_BODY_MAX - body->length) {
			errno = EFBIG;
			return -1;
		}
		if (chunk_size == 0) {
			for (;;) {
				char *colon;

				if (reader_line(reader, line, sizeof(line),
						&trailer_total) != 0)
					return -1;
				if (!line[0])
					return 0;
				if (line[0] == ' ' || line[0] == '\t') {
					errno = EPROTO;
					return -1;
				}
				colon = strchr(line, ':');
				if (!colon) {
					errno = EPROTO;
					return -1;
				}
				*colon = '\0';
				if (!header_name_valid(line)) {
					errno = EPROTO;
					return -1;
				}
			}
		}
		if (reader_append_exact(reader, body, (size_t)chunk_size) != 0 ||
		    reader_expect_crlf(reader) != 0)
			return -1;
	}
}

static int read_close_delimited_body(struct rpc_reader *reader,
				     struct rpc_body *body)
{
	for (;;) {
		size_t available;
		int result;

		if (reader->offset == reader->length) {
			result = reader_fill(reader);
			if (result < 0)
				return -1;
			if (result == 0)
				return 0;
		}
		available = reader->length - reader->offset;
		if (body_reserve(body, available) != 0)
			return -1;
		memcpy(body->data + body->length,
		       reader->buffer + reader->offset, available);
		reader->offset += available;
		body->length += available;
	}
}

static int receive_response(int fd, int64_t deadline_ms,
			    struct rpc_body *body, int *status)
{
	struct rpc_reader reader;
	struct rpc_response response;
	int interim = 0;

	memset(&reader, 0, sizeof(reader));
	reader.fd = fd;
	reader.deadline_ms = deadline_ms;
	for (;;) {
		if (parse_response_headers(&reader, &response) != 0)
			return -1;
		if (response.status < 100 || response.status >= 200)
			break;
		if (response.status == 101 ||
		    ++interim > RPC_MAX_INTERIM_RESPONSES ||
		    response.chunked ||
		    (response.has_content_length && response.content_length != 0)) {
			errno = EPROTO;
			return -1;
		}
	}
	if (response.chunked && response.has_content_length) {
		errno = EPROTO;
		return -1;
	}
	*status = response.status;
	if (response.status == 204 || response.status == 304) {
		if (response.chunked ||
		    (response.has_content_length && response.content_length != 0)) {
			errno = EPROTO;
			return -1;
		}
		return 0;
	}
	if (response.chunked)
		return read_chunked_body(&reader, body);
	if (response.has_content_length) {
		if (response.content_length > RPC_RESPONSE_BODY_MAX) {
			errno = EFBIG;
			return -1;
		}
		return reader_append_exact(&reader, body,
					   (size_t)response.content_length);
	}
	return read_close_delimited_body(&reader, body);
}

static bool token_valid(const char *token)
{
	const unsigned char *cursor = (const unsigned char *)token;
	size_t length;

	if (!token)
		return false;
	length = strlen(token);
	if (length < 32 || length > 127)
		return false;
	for (; *cursor; cursor++) {
		if (isalnum(*cursor) || strchr("._~-", *cursor))
			continue;
		return false;
	}
	return true;
}

static bool path_valid(const char *path)
{
	const unsigned char *cursor = (const unsigned char *)path;
	size_t length;

	if (!path || path[0] != '/')
		return false;
	length = strlen(path);
	if (length == 0 || length > RPC_PATH_MAX)
		return false;
	for (; *cursor; cursor++) {
		if (*cursor <= 0x20 || *cursor >= 0x7f ||
		    *cursor == '#' || *cursor == '\\')
			return false;
	}
	return true;
}

static int write_stdout(const unsigned char *data, size_t length)
{
	while (length) {
		ssize_t written = write(STDOUT_FILENO, data, length);

		if (written > 0) {
			data += (size_t)written;
			length -= (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written == 0)
			errno = EIO;
		return -1;
	}
	return 0;
}

int rpc_client_request(const char *host, int port, const char *path,
		       const char *method, const char *body,
		       const char *auth_token, int timeout_ms)
{
	struct rpc_body response_body;
	char *request_header = NULL;
	const char *request_body = body ? body : "";
	const char *host_open = "";
	const char *host_close = "";
	size_t request_body_length;
	size_t header_capacity;
	int header_length;
	int64_t started;
	int64_t deadline;
	int response_status = 0;
	int fd = -1;
	int result = -1;
	int saved_errno = 0;

	memset(&response_body, 0, sizeof(response_body));
	if (!valid_local_host(host) || port < 1 || port > 65535 ||
	    !path_valid(path) || !method ||
	    (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) ||
	    !token_valid(auth_token) ||
	    timeout_ms < RPC_MIN_TIMEOUT_MS || timeout_ms > RPC_MAX_TIMEOUT_MS) {
		errno = EINVAL;
		goto done;
	}
	request_body_length = strlen(request_body);
	if (request_body_length > RPC_REQUEST_BODY_MAX ||
	    (strcmp(method, "GET") == 0 && request_body_length != 0)) {
		errno = E2BIG;
		goto done;
	}
	if (strchr(host, ':')) {
		host_open = "[";
		host_close = "]";
	}
	header_capacity = strlen(path) + strlen(host) + strlen(auth_token) + 512;
	request_header = malloc(header_capacity);
	if (!request_header)
		goto done;
	if (strcmp(method, "POST") == 0) {
		header_length = snprintf(request_header, header_capacity,
			"POST %s HTTP/1.1\r\n"
			"Host: %s%s%s:%d\r\n"
			"Authorization: Bearer %s\r\n"
			"Accept: application/json\r\n"
			"Accept-Encoding: identity\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n"
			"User-Agent: nesd-loopback-rpc/1\r\n"
			"\r\n",
			path, host_open, host, host_close, port, auth_token,
			request_body_length);
	} else {
		header_length = snprintf(request_header, header_capacity,
			"GET %s HTTP/1.1\r\n"
			"Host: %s%s%s:%d\r\n"
			"Authorization: Bearer %s\r\n"
			"Accept: application/json\r\n"
			"Accept-Encoding: identity\r\n"
			"Connection: close\r\n"
			"User-Agent: nesd-loopback-rpc/1\r\n"
			"\r\n",
			path, host_open, host, host_close, port, auth_token);
	}
	if (header_length < 0 || (size_t)header_length >= header_capacity) {
		errno = EOVERFLOW;
		goto done;
	}
	started = monotonic_ms();
	if (started < 0)
		goto done;
	deadline = started + timeout_ms;
	fd = connect_loopback(host, port, deadline);
	if (fd < 0)
		goto done;
	if (send_all(fd, request_header, (size_t)header_length, deadline) != 0 ||
	    (request_body_length &&
	     send_all(fd, request_body, request_body_length, deadline) != 0) ||
	    receive_response(fd, deadline, &response_body,
			     &response_status) != 0)
		goto done;
	if (write_stdout(response_body.data, response_body.length) != 0)
		goto done;
	result = response_status >= 200 && response_status < 300 ?
		0 : NES_RPC_CLIENT_HTTP_ERROR;

done:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (request_header) {
		volatile unsigned char *cursor =
			(volatile unsigned char *)request_header;
		size_t index;

		for (index = 0; index < header_capacity; index++)
			cursor[index] = 0;
		free(request_header);
	}
	free(response_body.data);
	errno = saved_errno;
	return result;
}
