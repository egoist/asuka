#define _POSIX_C_SOURCE 200809L
#include "t.h"
#include "abi/dev.h"
#include "rt/sched.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

static uint8_t got[64];
static size_t got_len;
static bool got_fresh;

static void reload(const uint8_t *p, size_t n, bool fresh) {
	memcpy(got, p, n < sizeof got ? n : sizeof got);
	got_len = n;
	got_fresh = fresh;
}

static bool read_frame(int fd, uint8_t *kind, uint8_t *buf, size_t cap, size_t *n) {
	uint8_t h[5];
	size_t off = 0;
	while (off < 5) {
		ssize_t r = recv(fd, h + off, 5 - off, 0);
		if (r <= 0) return false;
		off += (size_t)r;
	}
	uint32_t len = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
	*kind = h[4];
	*n = len - 1;
	off = 0;
	while (off < *n && off < cap) {
		ssize_t r = recv(fd, buf + off, *n - off, 0);
		if (r <= 0) return false;
		off += (size_t)r;
	}
	return true;
}

// accept_within accepts a connection on ln, waiting at most 10 s (the
// player dials again half a second after a session ends).
static int accept_within(int ln) {
	struct pollfd pf = {.fd = ln, .events = POLLIN};
	if (poll(&pf, 1, 10000) != 1) return -1;
	return accept(ln, NULL, NULL);
}

static void expect_hello(int c) {
	uint8_t kind, buf[64];
	size_t n;
	REQUIRE(read_frame(c, &kind, buf, sizeof buf, &n));
	CHECK_EQ_INT(kind, AK_DEV_HELLO);
	CHECK(n == 19 && memcmp(buf, "{\"platform\":\"test\"}", 19) == 0);
}

static void send_frame(int c, uint8_t kind, const char *five) {
	uint8_t frame[10] = {6, 0, 0, 0, kind};
	memcpy(frame + 5, five, 5);
	send(c, frame, sizeof frame, 0);
	got_len = 0;
	// Wait for it (a condition, not a time).
	for (int i = 0; i < 5000 && got_len == 0; i++) {
		ak_sched_run_posted();
		struct timespec ts = {0, 1000000};
		nanosleep(&ts, NULL);
	}
	CHECK_EQ_INT(got_len, 5);
	CHECK(memcmp(got, five, 5) == 0);
	CHECK(got_fresh == (kind == AK_DEV_RESTART));
}

static void send_module(int c, const char *five) { send_frame(c, AK_DEV_MODULE, five); }

static int free_port(void) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001)};
	bind(s, (struct sockaddr *)&a, sizeof a);
	socklen_t al = sizeof a;
	getsockname(s, (struct sockaddr *)&a, &al);
	close(s);
	return ntohs(a.sin_port);
}

// The player dials the CLI (the first of its hosts that answers), gets
// modules on the main thread, sends its frames, and dials again when the
// CLI goes; a connection the CLI makes to its tunnel port takes over.
TEST(the_session) {
	int ln = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001)};
	REQUIRE(bind(ln, (struct sockaddr *)&a, sizeof a) == 0);
	socklen_t al = sizeof a;
	getsockname(ln, (struct sockaddr *)&a, &al);
	listen(ln, 4);
	ak_dev_set_reload(reload);
	char err[128];
	REQUIRE(ak_dev_connect("::1,127.0.0.1", ntohs(a.sin_port), "{\"platform\":\"test\"}", false, err, sizeof err));
	int c = accept_within(ln);
	REQUIRE(c >= 0);
	expect_hello(c);
	send_module(c, "AKBC!");
	ak_dev_send(AK_DEV_RELOADED, "\x03\0\0\0\x10\0\0\0", 8);
	uint8_t kind, buf[64];
	size_t n;
	REQUIRE(read_frame(c, &kind, buf, sizeof buf, &n));
	CHECK_EQ_INT(kind, AK_DEV_RELOADED);
	// A restart: the module, loaded keeping no state.
	send_frame(c, AK_DEV_RESTART, "AKBC!");

	// The CLI restarts: the player comes back.
	close(c);
	c = accept_within(ln);
	REQUIRE(c >= 0);
	expect_hello(c);
	send_module(c, "AGAIN");

	// Through the cable: the CLI connects to the player, which leaves its
	// own connection for it.
	int port = free_port();
	REQUIRE(ak_dev_listen(port, "{\"platform\":\"test\"}", false, err, sizeof err));
	int t = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in ta = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(0x7f000001)};
	REQUIRE(connect(t, (struct sockaddr *)&ta, sizeof ta) == 0);
	expect_hello(t);
	CHECK(!read_frame(c, &kind, buf, sizeof buf, &n));  // the old one closed
	close(c);
	send_module(t, "CABLE");
	ak_dev_send(AK_DEV_LOG, "\x01hi", 3);
	REQUIRE(read_frame(t, &kind, buf, sizeof buf, &n));
	CHECK_EQ_INT(kind, AK_DEV_LOG);

	// The cable's goes: the player dials again.
	close(t);
	c = accept_within(ln);
	REQUIRE(c >= 0);
	expect_hello(c);
	close(c);
	close(ln);
}

T_MAIN()
