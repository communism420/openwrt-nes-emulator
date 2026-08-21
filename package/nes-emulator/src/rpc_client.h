#ifndef NES_RPC_CLIENT_H
#define NES_RPC_CLIENT_H

/*
 * A complete HTTP response with a non-2xx status is still written to stdout.
 * The distinct exit status lets callers reject it for readiness checks without
 * losing the daemon's JSON error body.
 */
#define NES_RPC_CLIENT_HTTP_ERROR 22

int rpc_client_request(const char *host, int port, const char *path,
	const char *method, const char *body, const char *auth_token,
	int timeout_ms);

#endif
