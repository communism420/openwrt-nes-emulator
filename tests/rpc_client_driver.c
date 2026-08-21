#include "rpc_client.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define TEST_TOKEN "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static int parse_number(const char *text, int minimum, int maximum, int *out)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || !end || *end || value < minimum || value > maximum)
		return -1;
	*out = (int)value;
	return 0;
}

int main(int argc, char **argv)
{
	int port;
	int timeout_ms;

	if (argc != 7 ||
	    parse_number(argv[2], 1, 65535, &port) != 0 ||
	    parse_number(argv[6], 100, 10000, &timeout_ms) != 0)
		return 2;
	return rpc_client_request(argv[1], port, argv[3], argv[4], argv[5],
				  TEST_TOKEN, timeout_ms);
}
