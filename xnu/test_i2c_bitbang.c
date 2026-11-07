#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cepheus_i2c_bitbang.h"

struct mock {
	int samples[4096]; unsigned count, pos, waits, starts, stops;
	int low[2], stretch;
};
static void drive(void *ctx, unsigned line, int low)
{
	struct mock *m = ctx;
	assert(line < 2 && (low == 0 || low == 1));
	if (line == BB_SDA && !m->low[BB_SCL] && m->low[line] != low) {
		if (low) { ++m->starts; } else { ++m->stops; }
	}
	m->low[line] = low;
}
static int sample(void *ctx, unsigned line)
{
	struct mock *m = ctx;
	if (line == BB_SCL) { return !m->stretch; }
	assert(m->pos < m->count);
	return m->samples[m->pos++];
}
static void wait_us(void *ctx, unsigned us) { assert(us == 10); ((struct mock *)ctx)->waits++; }
static void push(struct mock *m, int value) { assert(m->count < 4096); m->samples[m->count++] = value; }
static void tx(struct mock *m, unsigned value, int nack)
{
	for (unsigned mask = 0x80; mask; mask >>= 1) { if (value & mask) { push(m, 1); } }
	push(m, nack);
}
static void rx(struct mock *m, unsigned value)
{
	for (unsigned mask = 0x80; mask; mask >>= 1) { push(m, !!(value & mask)); }
}
int main(void)
{
	struct mock m = {0}; struct cepheus_bb b = {&m, drive, sample, wait_us};
	const uint8_t id_cmd[] = {0xfa, 0x20, 0, 0, 0}; uint8_t data[256] = {0};
	push(&m, 1); tx(&m, 0x92, 0);
	for (unsigned i = 0; i < sizeof(id_cmd); ++i) { tx(&m, id_cmd[i], 0); }
	push(&m, 1); tx(&m, 0x93, 0); rx(&m, 0x36); rx(&m, 0x39);
	assert(!cepheus_bb_xfer(&b, 0x49, id_cmd, sizeof(id_cmd), data, 2));
	assert(data[0] == 0x36 && data[1] == 0x39 && m.pos == m.count);
	assert(m.starts == 2 && m.stops == 1 && !m.low[0] && !m.low[1]);
	memset(&m, 0, sizeof(m)); push(&m, 1); tx(&m, 0x92, 1);
	assert(cepheus_bb_xfer(&b, 0x49, id_cmd, 1, NULL, 0) == ENXIO);
	assert(m.stops == 1 && !m.low[0] && !m.low[1]);
	memset(&m, 0, sizeof(m)); m.stretch = 1;
	assert(cepheus_bb_xfer(&b, 0x49, id_cmd, 1, NULL, 0) == ETIMEDOUT);
	assert(m.waits <= 102 && !m.low[0] && !m.low[1]);
	memset(&m, 0, sizeof(m)); push(&m, 0);
	assert(cepheus_bb_xfer(&b, 0x49, id_cmd, 1, NULL, 0) == EBUSY);
	assert(!m.low[0] && !m.low[1] && !m.starts);
	memset(&m, 0, sizeof(m)); push(&m, 1); push(&m, 0); /* address loses arbitration */
	assert(cepheus_bb_xfer(&b, 0x49, id_cmd, 1, NULL, 0) == EAGAIN);
	assert(!m.low[0] && !m.low[1] && !m.stops);
	memset(&m, 0, sizeof(m)); push(&m, 1); tx(&m, 0x93, 0);
	for (unsigned i = 0; i < 256; ++i) { rx(&m, i); }
	assert(!cepheus_bb_xfer(&b, 0x49, NULL, 0, data, 256));
	for (unsigned i = 0; i < 256; ++i) { assert(data[i] == i); }
	/* 6-byte hardware register write (system reset: FA 20000024 80), ACKed. */
	const uint8_t reset_cmd[] = {0xfa, 0x20, 0x00, 0x00, 0x24, 0x80};
	memset(&m, 0, sizeof(m)); push(&m, 1); tx(&m, 0x92, 0);
	for (unsigned i = 0; i < sizeof(reset_cmd); ++i) { tx(&m, reset_cmd[i], 0); }
	assert(!cepheus_bb_xfer(&b, 0x49, reset_cmd, sizeof(reset_cmd), NULL, 0));
	assert(m.pos == m.count && m.starts == 1 && m.stops == 1 && !m.low[0] && !m.low[1]);
	const uint8_t long_cmd[9] = {0};
	assert(cepheus_bb_xfer(&b, 0x49, long_cmd, 9, NULL, 0) == EINVAL);
	puts("GPIO I2C combined read, repeated START, NACK, timeout, busy, arbitration, 6-byte write, bounds: PASS");
}
