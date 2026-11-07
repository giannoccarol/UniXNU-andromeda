/*
 * Host test of andromeda_ctl.h (runtime control, patch 0140):
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined xnu/test_andromeda_ctl.c -o /tmp/ctl-test
 *   /tmp/ctl-test
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "andromeda_ctl.h"

static enum andr_perr
parse(const char *s, struct andr_cmd *cmd)
{
	return andr_ctl_parse(s, strlen(s), cmd);
}

static void
test_word(void)
{
	uint32_t s = andr_st(ANDR_PH_RUN, 4, 2) | ANDR_ST_GO;
	assert(andr_st_phase(s) == ANDR_PH_RUN);
	assert(andr_st_level(s) == 4u && andr_st_from(s) == 2u);
	assert(!andr_st_failed(s));
	assert(andr_st_failed(andr_done_word(2, true)));
	assert(strcmp(andr_st_name(0), "-") == 0);
	assert(strcmp(andr_st_name(andr_st(ANDR_PH_WAIT, 1, 0)), "wait") == 0);
	assert(strcmp(andr_st_name(s), "run") == 0);
	assert(strcmp(andr_st_name(andr_done_word(3, false)), "done") == 0);
	assert(strcmp(andr_st_name(andr_done_word(3, true)), "fail") == 0);
	/* Fields never spill into each other. */
	assert(andr_st(ANDR_PH_DONE, 0x1ff, 0x1ff) == andr_st(ANDR_PH_DONE, 0xff, 0xff));
}

static void
test_decide(void)
{
	uint32_t next = 0xdeadu;
	/* Range first, whatever the state. */
	assert(andr_decide(0, 0, 4, true, &next) == ANDR_ACT_RANGE);
	assert(andr_decide(0, 5, 4, true, &next) == ANDR_ACT_RANGE);
	assert(next == 0xdeadu);
	/* Nothing asked yet: run 1..want now. */
	assert(andr_decide(0, 2, 4, true, &next) == ANDR_ACT_START);
	assert(next == andr_st(ANDR_PH_RUN, 2, 1));
	/* Boot thread waiting at 1: starts now, at the higher of the two. */
	uint32_t wait1 = andr_st(ANDR_PH_WAIT, 1, 0);
	assert(andr_decide(wait1, 3, 4, true, &next) == ANDR_ACT_NOW);
	assert(next == (andr_st(ANDR_PH_WAIT, 3, 0) | ANDR_ST_GO));
	uint32_t wait4 = andr_st(ANDR_PH_WAIT, 4, 0);
	assert(andr_decide(wait4, 2, 4, false, &next) == ANDR_ACT_NOW);
	assert(andr_st_level(next) == 4u && (next & ANDR_ST_GO));
	/* Running: refused. */
	assert(andr_decide(andr_st(ANDR_PH_RUN, 2, 1), 3, 4, true, &next) == ANDR_ACT_BUSY);
	/* Passed up to 2: 1 and 2 never again, 3 continues from 3. */
	uint32_t done2 = andr_done_word(2, false);
	assert(andr_decide(done2, 1, 4, true, &next) == ANDR_ACT_DONE);
	assert(andr_decide(done2, 2, 4, true, &next) == ANDR_ACT_DONE);
	assert(andr_decide(done2, 4, 4, true, &next) == ANDR_ACT_MORE);
	assert(next == andr_st(ANDR_PH_RUN, 4, 3));
	/* Without steps: once per boot. */
	assert(andr_decide(andr_done_word(1, false), 2, 2, false, &next) == ANDR_ACT_ONCE);
	assert(andr_decide(andr_done_word(1, false), 1, 2, false, &next) == ANDR_ACT_DONE);
	/* Failed: never retried, at any level. */
	uint32_t fail2 = andr_done_word(2, true);
	assert(andr_decide(fail2, 3, 4, true, &next) == ANDR_ACT_FAILED);
	assert(andr_decide(fail2, 1, 4, true, &next) == ANDR_ACT_FAILED);
	assert(andr_decide(andr_done_word(0, true), 1, 1, false, &next) == ANDR_ACT_FAILED);
	assert(andr_act_ok(ANDR_ACT_START) && andr_act_ok(ANDR_ACT_NOW) && andr_act_ok(ANDR_ACT_MORE));
	assert(!andr_act_ok(ANDR_ACT_BUSY) && !andr_act_ok(ANDR_ACT_FAILED) && !andr_act_ok(ANDR_ACT_RANGE));
}

static void
test_begin(void)
{
	/* The boot thread takes WAIT (+GO) as RUN 1..level. */
	assert(andr_begin_word(andr_st(ANDR_PH_WAIT, 3, 0) | ANDR_ST_GO) == andr_st(ANDR_PH_RUN, 3, 1));
	assert(andr_begin_word(andr_st(ANDR_PH_WAIT, 2, 0)) == andr_st(ANDR_PH_RUN, 2, 1));
	/* A continuation stored by the control keeps its first level. */
	assert(andr_begin_word(andr_st(ANDR_PH_RUN, 4, 3)) == andr_st(ANDR_PH_RUN, 4, 3));
	/* A full sequence: boot wait at 1, control asks 2, run, then 4 continues from 3. */
	uint32_t st = andr_st(ANDR_PH_WAIT, 1, 0), next = 0;
	assert(andr_decide(st, 2, 4, true, &next) == ANDR_ACT_NOW);
	st = andr_begin_word(next);
	assert(andr_st_from(st) == 1u && andr_st_level(st) == 2u);
	st = andr_done_word(2, false);
	assert(andr_decide(st, 4, 4, true, &next) == ANDR_ACT_MORE);
	st = andr_begin_word(next);
	assert(andr_st_from(st) == 3u && andr_st_level(st) == 4u);
}

static void
test_claim(void)
{
	unsigned owner = 0;
	assert(andr_claim(&owner, 1));
	assert(andr_claim(&owner, 1));          /* again by the same owner */
	assert(!andr_claim(&owner, 2));         /* the other one: never */
	assert(owner == 1u);
}

static void
test_peer(void)
{
	assert(andr_ctl_peer_ok(0xc0a80701u, 40000, "ue0", 3));
	assert(!andr_ctl_peer_ok(0xc0a80702u, 40000, "ue0", 3));   /* the phone itself */
	assert(!andr_ctl_peer_ok(0xc0a80701u, 0, "ue0", 3));
	assert(!andr_ctl_peer_ok(0xc0a80701u, 40000, "en0", 3));
	assert(!andr_ctl_peer_ok(0xc0a80701u, 40000, "ue0x", 4));
	assert(!andr_ctl_peer_ok(0xc0a80701u, 40000, "ue", 2));
	assert(!andr_ctl_peer_ok(0xc0a80701u, 40000, NULL, 0));
	assert(!andr_ctl_peer_ok(0x7f000001u, 40000, "lo0", 3));
}

static void
test_rate(void)
{
	struct andr_rate r;
	memset(&r, 0, sizeof(r));
	for (unsigned i = 0; i < ANDR_CTL_BURST; ++i) {
		assert(andr_rate_take(&r, 1000));
	}
	assert(!andr_rate_take(&r, 1000));
	assert(!andr_rate_take(&r, 1000 + ANDR_CTL_REFILL_MS - 1));
	assert(andr_rate_take(&r, 1000 + ANDR_CTL_REFILL_MS));
	assert(!andr_rate_take(&r, 1000 + ANDR_CTL_REFILL_MS));
	/* Two refills at once; the remainder is kept. */
	assert(andr_rate_take(&r, 1000 + 3 * ANDR_CTL_REFILL_MS + 10));
	assert(andr_rate_take(&r, 1000 + 3 * ANDR_CTL_REFILL_MS + 10));
	assert(!andr_rate_take(&r, 1000 + 3 * ANDR_CTL_REFILL_MS + 10));
	/* A long pause fills the bucket, never above the burst. */
	unsigned n = 0;
	while (andr_rate_take(&r, 1000000)) {
		++n;
	}
	assert(n == ANDR_CTL_BURST);
	/* A clock that goes back does not refill. */
	assert(!andr_rate_take(&r, 5));
}

static void
test_parse_ok(void)
{
	struct andr_cmd c;
	assert(parse("status", &c) == ANDR_P_OK && c.verb == ANDR_V_STATUS && !c.has_value);
	assert(parse("status\n", &c) == ANDR_P_OK && c.verb == ANDR_V_STATUS);
	assert(parse("status\r\n", &c) == ANDR_P_OK && c.verb == ANDR_V_STATUS);
	assert(parse("  help  ", &c) == ANDR_P_OK && c.verb == ANDR_V_HELP);
	assert(parse("gpu 1", &c) == ANDR_P_OK && c.verb == ANDR_V_GPU && c.value == 1u && c.has_value);
	assert(parse("gpu  4", &c) == ANDR_P_OK && c.value == 4u);
	assert(parse("gpu 02", &c) == ANDR_P_OK && c.value == 2u);
	assert(parse("bt 2", &c) == ANDR_P_OK && c.verb == ANDR_V_BT && c.value == 2u);
	assert(parse("hap 1", &c) == ANDR_P_OK && c.verb == ANDR_V_HAP);
	assert(parse("cam 1", &c) == ANDR_P_OK && c.verb == ANDR_V_CAM);
	assert(parse("wlan 2", &c) == ANDR_P_OK && c.verb == ANDR_V_WLAN && c.value == 2u);
	assert(parse("bl 2", &c) == ANDR_P_OK && c.verb == ANDR_V_BL);
	assert(parse("pwr 0", &c) == ANDR_P_OK && c.verb == ANDR_V_PWR && c.value == 0u && c.has_value);
	assert(parse("pwr 1", &c) == ANDR_P_OK && c.value == 1u);
	assert(parse("slider 1", &c) == ANDR_P_OK && c.verb == ANDR_V_SLIDER);
	assert(parse("slidercam 0", &c) == ANDR_P_OK && c.verb == ANDR_V_SLIDERCAM && c.value == 0u);
	assert(parse("slidercam 2", &c) == ANDR_P_OK && c.value == 2u);
	assert(parse("fastboot", &c) == ANDR_P_OK && c.verb == ANDR_V_FASTBOOT);
	assert(parse("idle", &c) == ANDR_P_OK && c.verb == ANDR_V_IDLE && c.idle == ANDR_IDLE_COUNT);
	assert(parse("idle cpu 1", &c) == ANDR_P_OK && c.idle == ANDR_IDLE_CPU && c.value == 1u);
	assert(parse("idle fb 0", &c) == ANDR_P_OK && c.idle == ANDR_IDLE_FB && c.value == 0u);
	assert(parse("idle usb 20", &c) == ANDR_P_OK && c.idle == ANDR_IDLE_USB && c.value == 20u);
	assert(parse("idle usb 0", &c) == ANDR_P_OK && c.value == 0u);
	assert(parse("idle keys 50", &c) == ANDR_P_OK && c.idle == ANDR_IDLE_KEYS);
	assert(parse("idle touch 1000", &c) == ANDR_P_OK && c.idle == ANDR_IDLE_TOUCH && c.value == 1000u);
}

static void
test_parse_bad(void)
{
	struct andr_cmd c;
	char big[ANDR_CTL_MAX + 2];
	assert(parse("", &c) == ANDR_P_EMPTY);
	assert(parse("   ", &c) == ANDR_P_EMPTY);
	assert(parse("\n", &c) == ANDR_P_EMPTY);
	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	assert(parse(big, &c) == ANDR_P_LONG);
	/* Exactly the maximum, with the newline on top, is still parsed. */
	memset(big, ' ', ANDR_CTL_MAX);
	memcpy(big, "status", 6);
	big[ANDR_CTL_MAX] = '\n';
	big[ANDR_CTL_MAX + 1] = '\0';
	assert(parse(big, &c) == ANDR_P_OK && c.verb == ANDR_V_STATUS);
	assert(parse("gpu\t1", &c) == ANDR_P_CHAR);
	assert(andr_ctl_parse("gpu 1\0", 6, &c) == ANDR_P_CHAR);
	assert(parse("gpu 1\n\n", &c) == ANDR_P_CHAR);
	assert(parse("st\x80tus", &c) == ANDR_P_CHAR);
	assert(parse("GPU 1", &c) == ANDR_P_VERB);
	assert(parse("gp 1", &c) == ANDR_P_VERB);
	assert(parse("gpux 1", &c) == ANDR_P_VERB);
	assert(parse("statusx", &c) == ANDR_P_VERB);
	assert(parse("reboot", &c) == ANDR_P_VERB);
	assert(parse("gpu", &c) == ANDR_P_ARGS);
	assert(parse("gpu 1 2", &c) == ANDR_P_ARGS);
	assert(parse("status 1", &c) == ANDR_P_ARGS);
	assert(parse("fastboot now", &c) == ANDR_P_ARGS);
	assert(parse("gpu -1", &c) == ANDR_P_NUMBER);
	assert(parse("gpu +1", &c) == ANDR_P_NUMBER);
	assert(parse("gpu 0x2", &c) == ANDR_P_NUMBER);
	assert(parse("gpu 123456", &c) == ANDR_P_NUMBER);
	assert(parse("gpu 0", &c) == ANDR_P_RANGE && c.verb == ANDR_V_GPU);
	assert(parse("gpu 5", &c) == ANDR_P_RANGE);
	assert(parse("gpu 99999", &c) == ANDR_P_RANGE);
	assert(parse("bt 3", &c) == ANDR_P_RANGE);
	assert(parse("hap 2", &c) == ANDR_P_RANGE);
	assert(parse("cam 0", &c) == ANDR_P_RANGE);
	assert(parse("pwr 2", &c) == ANDR_P_RANGE);
	assert(parse("slidercam 3", &c) == ANDR_P_RANGE);
	assert(parse("idle disk 1", &c) == ANDR_P_IDLE);
	assert(parse("idle usb", &c) == ANDR_P_ARGS);
	assert(parse("idle usb 2 3", &c) == ANDR_P_ARGS);
	assert(parse("idle usb 1", &c) == ANDR_P_RANGE && c.idle == ANDR_IDLE_USB);
	assert(parse("idle usb 21", &c) == ANDR_P_RANGE);
	assert(parse("idle keys 19", &c) == ANDR_P_RANGE);
	assert(parse("idle keys 201", &c) == ANDR_P_RANGE);
	assert(parse("idle touch 1001", &c) == ANDR_P_RANGE);
	assert(parse("idle cpu 2", &c) == ANDR_P_RANGE);    /* bits above 0 are for phase P2 */
	assert(parse("idle fb 2", &c) == ANDR_P_RANGE);
	assert(parse("idle cpu x", &c) == ANDR_P_NUMBER);
	for (int e = ANDR_P_OK; e <= ANDR_P_IDLE; ++e) {
		assert(andr_perr_name((enum andr_perr)e)[0] != '\0');
	}
}

/* Every byte string up to 4 bytes from a small alphabet: never crashes, verb set on success. */
static void
test_parse_fuzz(void)
{
	static const char alpha[] = "gpu 1\n\r\x01idle";
	char buf[4];
	unsigned n = (unsigned)sizeof(alpha) - 1u, ok = 0;
	for (unsigned len = 0; len <= 4; ++len) {
		unsigned total = 1;
		for (unsigned i = 0; i < len; ++i) {
			total *= n;
		}
		for (unsigned k = 0; k < total; ++k) {
			unsigned x = k;
			for (unsigned i = 0; i < len; ++i) {
				buf[i] = alpha[x % n];
				x /= n;
			}
			struct andr_cmd c;
			if (andr_ctl_parse(buf, len, &c) == ANDR_P_OK) {
				assert(c.verb < ANDR_V_COUNT);
				++ok;
			}
		}
	}
	assert(ok > 0u);
}

int
main(void)
{
	test_word();
	test_decide();
	test_begin();
	test_claim();
	test_peer();
	test_rate();
	test_parse_ok();
	test_parse_bad();
	test_parse_fuzz();
	printf("andromeda_ctl: all tests passed\n");
	return 0;
}
