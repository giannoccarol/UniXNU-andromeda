/* Bounded software I2C transport. Drive-low or release only: never push high.
 * Caller supplies uint8_t and errno definitions, GPIO operations and delay.
 * No allocation, global interrupt masking, DMA or bus-recovery pulses.
 */
#ifndef CEPHEUS_I2C_BITBANG_H
#define CEPHEUS_I2C_BITBANG_H
enum { BB_SDA, BB_SCL };
struct cepheus_bb {
	void *ctx;
	void (*drive)(void *, unsigned, int); /* low=1, released=0 */
	int (*sample)(void *, unsigned);
	void (*wait)(void *, unsigned);
};

static void bb_wait(struct cepheus_bb *b) { b->wait(b->ctx, 10); }
static void bb_drive(struct cepheus_bb *b, unsigned line, int low) { b->drive(b->ctx, line, low); }
static void bb_release(struct cepheus_bb *b)
{
	bb_drive(b, BB_SDA, 0);
	bb_drive(b, BB_SCL, 0);
}
static int bb_scl_high(struct cepheus_bb *b)
{
	bb_drive(b, BB_SCL, 0);
	for (unsigned i = 0; i < 100; ++i) {
		if (b->sample(b->ctx, BB_SCL)) { bb_wait(b); return 0; }
		bb_wait(b);
	}
	return ETIMEDOUT;
}
static int bb_start(struct cepheus_bb *b)
{
	bb_drive(b, BB_SDA, 0);
	bb_wait(b);
	int error = bb_scl_high(b);
	if (error) { return error; }
	if (!b->sample(b->ctx, BB_SDA)) { return EBUSY; }
	bb_drive(b, BB_SDA, 1);
	bb_wait(b);
	bb_drive(b, BB_SCL, 1);
	return 0;
}
static int bb_stop(struct cepheus_bb *b)
{
	bb_drive(b, BB_SCL, 1);
	bb_drive(b, BB_SDA, 1);
	bb_wait(b);
	int error = bb_scl_high(b);
	bb_drive(b, BB_SDA, 0);
	bb_wait(b);
	return error;
}
static int bb_put(struct cepheus_bb *b, uint8_t byte)
{
	for (unsigned bit = 0; bit < 8; ++bit) {
		int one = (byte & (0x80u >> bit)) != 0;
		bb_drive(b, BB_SDA, !one);
		bb_wait(b);
		int error = bb_scl_high(b);
		if (error) { return error; }
		if (one && !b->sample(b->ctx, BB_SDA)) { return EAGAIN; }
		bb_drive(b, BB_SCL, 1);
	}
	bb_drive(b, BB_SDA, 0);
	bb_wait(b);
	int error = bb_scl_high(b);
	if (error) { return error; }
	int nack = b->sample(b->ctx, BB_SDA);
	bb_drive(b, BB_SCL, 1);
	return nack ? ENXIO : 0;
}
static int bb_get(struct cepheus_bb *b, uint8_t *byte, int last)
{
	uint8_t value = 0;
	bb_drive(b, BB_SDA, 0);
	for (unsigned bit = 0; bit < 8; ++bit) {
		bb_wait(b);
		int error = bb_scl_high(b);
		if (error) { return error; }
		value = (uint8_t)((value << 1) | !!b->sample(b->ctx, BB_SDA));
		bb_drive(b, BB_SCL, 1);
	}
	bb_drive(b, BB_SDA, !last); /* ACK all but the final byte. */
	bb_wait(b);
	int error = bb_scl_high(b);
	bb_drive(b, BB_SCL, 1);
	bb_drive(b, BB_SDA, 0);
	if (!error) { *byte = value; }
	return error;
}
static int cepheus_bb_xfer(struct cepheus_bb *b, unsigned address,
    const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr)
{
	if (address > 0x7f || nt > 8 || nr > 256 || (!nt && !nr) ||
	    (nt && !tx) || (nr && !rx)) { return EINVAL; }
	int error = bb_start(b);
	if (error) { bb_release(b); return error; }
	if (nt) {
		error = bb_put(b, (uint8_t)(address << 1));
		for (unsigned i = 0; i < nt && !error; ++i) { error = bb_put(b, tx[i]); }
	}
	if (nr && !error) {
		if (nt) { error = bb_start(b); } /* repeated START, no STOP */
		if (!error) { error = bb_put(b, (uint8_t)((address << 1) | 1)); }
		for (unsigned i = 0; i < nr && !error; ++i) { error = bb_get(b, rx + i, i + 1 == nr); }
	}
	/* Never disturb the bus with STOP after losing arbitration or stretch
	 * timeout. Leave both pins released and let the caller stop this driver.
	 */
	if (error == EAGAIN || error == ETIMEDOUT || error == EBUSY) {
		bb_release(b);
		return error;
	}
	int stop_error = bb_stop(b);
	bb_release(b);
	return error ? error : stop_error;
}
#endif
