// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Backend for the LRNG providing the cryptographic primitives using
 * ChaCha20 cipher implementations.
 *
 * Copyright (C) 2016 - 2019, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/chacha.h>
#include <linux/cryptohash.h>
#include <linux/lrng.h>
#include <linux/random.h>

#include "lrng_internal.h"

/******************************* ChaCha20 DRNG *******************************/

/* State according to RFC 7539 section 2.3 */
struct chacha20_block {
	u32 constants[4];
#define CHACHA_KEY_SIZE_WORDS (CHACHA_KEY_SIZE / sizeof(u32))
	union {
		u32 u[CHACHA_KEY_SIZE_WORDS];
		u8  b[CHACHA_KEY_SIZE];
	} key;
	u32 counter;
	u32 nonce[3];
};

#define CHACHA_BLOCK_WORDS	(CHACHA_BLOCK_SIZE / sizeof(u32))

struct chacha20_state {
	struct chacha20_block block;
};

/*
 * Have two static memory blocks for two ChaCha20 DRNG instances (the primary
 * and the secondary DRNG) to avoid calling kmalloc too early in the boot cycle.
 * for subsequent allocation requests, such as per-NUMA-node DRNG instances,
 * kmalloc will be used.
 */
struct chacha20_state primary_chacha20;
struct chacha20_state secondary_chacha20;

/**
 * Update of the ChaCha20 state by either using an unused buffer part or by
 * generating one ChaCha20 block which is half of the state of the ChaCha20.
 * The block is XORed into the key part of the state. This shall ensure
 * backtracking resistance as well as a proper mix of the ChaCha20 state once
 * the key is injected.
 */
static void lrng_chacha20_update(struct chacha20_state *chacha20_state,
				 u32 *buf, u32 used_words)
{
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 i, tmp[CHACHA_BLOCK_WORDS];

	BUILD_BUG_ON(sizeof(struct chacha20_block) != CHACHA_BLOCK_SIZE);
	BUILD_BUG_ON(CHACHA_BLOCK_SIZE != 2 * CHACHA_KEY_SIZE);

	if (used_words > CHACHA_KEY_SIZE_WORDS) {
		chacha20_block(&chacha20->constants[0], (u8 *)tmp);
		for (i = 0; i < CHACHA_KEY_SIZE_WORDS; i++)
			chacha20->key.u[i] ^= tmp[i];
		memzero_explicit(tmp, sizeof(tmp));
	} else {
		for (i = 0; i < CHACHA_KEY_SIZE_WORDS; i++)
			chacha20->key.u[i] ^= buf[i + used_words];
	}

	/* Deterministic increment of nonce as required in RFC 7539 chapter 4 */
	chacha20->nonce[0]++;
	if (chacha20->nonce[0] == 0)
		chacha20->nonce[1]++;
	if (chacha20->nonce[1] == 0)
		chacha20->nonce[2]++;

	/* Leave counter untouched as it is start value is undefined in RFC */
}

/**
 * Seed the ChaCha20 DRNG by injecting the input data into the key part of
 * the ChaCha20 state. If the input data is longer than the ChaCha20 key size,
 * perform a ChaCha20 operation after processing of key size input data.
 * This operation shall spread out the entropy into the ChaCha20 state before
 * new entropy is injected into the key part.
 */
static int lrng_cc20_drng_seed_helper(void *drng, const u8 *inbuf, u32 inbuflen)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;
	struct chacha20_block *chacha20 = &chacha20_state->block;

	while (inbuflen) {
		u32 i, todo = min_t(u32, inbuflen, CHACHA_KEY_SIZE);

		for (i = 0; i < todo; i++)
			chacha20->key.b[i] ^= inbuf[i];

		/* Break potential dependencies between the inbuf key blocks */
		lrng_chacha20_update(chacha20_state, NULL,
				     CHACHA_BLOCK_WORDS);
		inbuf += todo;
		inbuflen -= todo;
	}

	return 0;
}

/**
 * Chacha20 DRNG generation of random numbers: the stream output of ChaCha20
 * is the random number. After the completion of the generation of the
 * stream, the entire ChaCha20 state is updated.
 *
 * Note, as the ChaCha20 implements a 32 bit counter, we must ensure
 * that this function is only invoked for at most 2^32 - 1 ChaCha20 blocks
 * before a reseed or an update happens. This is ensured by the variable
 * outbuflen which is a 32 bit integer defining the number of bytes to be
 * generated by the ChaCha20 DRNG. At the end of this function, an update
 * operation is invoked which implies that the 32 bit counter will never be
 * overflown in this implementation.
 */
static int lrng_cc20_drng_generate_helper(void *drng, u8 *outbuf, u32 outbuflen)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 aligned_buf[CHACHA_BLOCK_WORDS], ret = outbuflen,
	    used = CHACHA_BLOCK_WORDS;
	int zeroize_buf = 0;

	while (outbuflen >= CHACHA_BLOCK_SIZE) {
		chacha20_block(&chacha20->constants[0], outbuf);
		outbuf += CHACHA_BLOCK_SIZE;
		outbuflen -= CHACHA_BLOCK_SIZE;
	}

	if (outbuflen) {
		chacha20_block(&chacha20->constants[0], (u8 *)aligned_buf);
		memcpy(outbuf, aligned_buf, outbuflen);
		used = ((outbuflen + sizeof(aligned_buf[0]) - 1) /
			sizeof(aligned_buf[0]));
		zeroize_buf = 1;
	}

	lrng_chacha20_update(chacha20_state, aligned_buf, used);

	if (zeroize_buf)
		memzero_explicit(aligned_buf, sizeof(aligned_buf));

	return ret;
}

/**
 * ChaCha20 DRNG that provides full strength, i.e. the output is capable
 * of transporting 1 bit of entropy per data bit, provided the DRNG was
 * seeded with 256 bits of entropy. This is achieved by folding the ChaCha20
 * block output of 512 bits in half using XOR.
 *
 * Other than the output handling, the implementation is conceptually
 * identical to lrng_drng_generate_helper.
 */
static int lrng_cc20_drng_generate_helper_full(void *drng, u8 *outbuf,
					       u32 outbuflen)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 aligned_buf[CHACHA_BLOCK_WORDS];
	u32 ret = outbuflen;

	while (outbuflen >= CHACHA_BLOCK_SIZE) {
		u32 i;

		chacha20_block(&chacha20->constants[0], outbuf);

		/* fold output in half */
		for (i = 0; i < (CHACHA_BLOCK_WORDS / 2); i++)
			outbuf[i] ^= outbuf[i + (CHACHA_BLOCK_WORDS / 2)];

		outbuf += CHACHA_BLOCK_SIZE / 2;
		outbuflen -= CHACHA_BLOCK_SIZE / 2;
	}

	while (outbuflen) {
		u32 i, todo = min_t(u32, CHACHA_BLOCK_SIZE / 2, outbuflen);

		chacha20_block(&chacha20->constants[0], (u8 *)aligned_buf);

		/* fold output in half */
		for (i = 0; i < (CHACHA_BLOCK_WORDS / 2); i++)
			aligned_buf[i] ^=
				aligned_buf[i + (CHACHA_BLOCK_WORDS / 2)];

		memcpy(outbuf, aligned_buf, todo);
		outbuflen -= todo;
		outbuf += todo;
	}
	memzero_explicit(aligned_buf, sizeof(aligned_buf));

	lrng_chacha20_update(chacha20_state, NULL, CHACHA_BLOCK_WORDS);

	return ret;
}

void lrng_cc20_init_state(struct chacha20_state *state)
{
	struct chacha20_block *chacha20 = &state->block;
	unsigned long v;
	u32 i;

	memcpy(&chacha20->constants[0], "expand 32-byte k", 16);

	for (i = 0; i < CHACHA_KEY_SIZE_WORDS; i++) {
		chacha20->key.u[i] ^= jiffies;
		chacha20->key.u[i] ^= random_get_entropy();
		if (arch_get_random_seed_long(&v) || arch_get_random_long(&v))
			chacha20->key.u[i] ^= v;
	}

	for (i = 0; i < 3; i++) {
		chacha20->nonce[i] ^= jiffies;
		chacha20->nonce[i] ^= random_get_entropy();
		if (arch_get_random_seed_long(&v) || arch_get_random_long(&v))
			chacha20->nonce[i] ^= v;
	}

	pr_info("ChaCha20 core initialized\n");
}

/**
 * Allocation of the DRNG state
 */
static void *lrng_cc20_drng_alloc(u32 sec_strength)
{
	struct chacha20_state *state = NULL;

	if (sec_strength > CHACHA_KEY_SIZE) {
		pr_err("Security strength of ChaCha20 DRNG (%u bits) lower "
		       "than requested by LRNG (%u bits)\n",
			CHACHA_KEY_SIZE * 8, sec_strength * 8);
		return ERR_PTR(-EINVAL);
	}
	if (sec_strength < CHACHA_KEY_SIZE)
		pr_warn("Security strength of ChaCha20 DRNG (%u bits) higher "
			"than requested by LRNG (%u bits)\n",
			CHACHA_KEY_SIZE * 8, sec_strength * 8);

	state = kmalloc(sizeof(struct chacha20_state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);
	pr_debug("memory for ChaCha20 core allocated\n");

	lrng_cc20_init_state(state);

	return state;
}

static void lrng_cc20_drng_dealloc(void *drng)
{
	struct chacha20_state *chacha20_state = (struct chacha20_state *)drng;

	if (drng == &primary_chacha20 || drng == &secondary_chacha20) {
		memzero_explicit(chacha20_state, sizeof(*chacha20_state));
		pr_debug("static ChaCha20 core zeroized\n");
		return;
	}

	pr_debug("ChaCha20 core zeroized and freed\n");
	kzfree(chacha20_state);
}

/******************************* Hash Operation *******************************/

static void *lrng_cc20_hash_alloc(const u8 *key, u32 keylen)
{
	pr_info("Hash SHA-1 allocated\n");
	return NULL;
}

static void lrng_cc20_hash_dealloc(void *hash)
{
}

static u32 lrng_cc20_hash_digestsize(void *hash)
{
	return (SHA_DIGEST_WORDS * sizeof(u32));
}

static int lrng_cc20_hash_buffer(void *hash, const u8 *inbuf, u32 inbuflen,
				 u8 *digest)
{
	u32 i;
	u32 workspace[SHA_WORKSPACE_WORDS];

	WARN_ON(inbuflen % (SHA_WORKSPACE_WORDS * sizeof(u32)));

	for (i = 0; i < inbuflen; i += (SHA_WORKSPACE_WORDS * sizeof(u32)))
		sha_transform((u32 *)digest, (inbuf + i), workspace);
	memzero_explicit(workspace, sizeof(workspace));

	return 0;
}

static const char *lrng_cc20_drng_name(void)
{
	const char *cc20_drng_name = "ChaCha20 DRNG";
	return cc20_drng_name;
}

static const char *lrng_cc20_hash_name(void)
{
	const char *cc20_hash_name = "SHA-1";
	return cc20_hash_name;
}

const struct lrng_crypto_cb lrng_cc20_crypto_cb = {
	.lrng_drng_name			= lrng_cc20_drng_name,
	.lrng_hash_name			= lrng_cc20_hash_name,
	.lrng_drng_alloc		= lrng_cc20_drng_alloc,
	.lrng_drng_dealloc		= lrng_cc20_drng_dealloc,
	.lrng_drng_seed_helper		= lrng_cc20_drng_seed_helper,
	.lrng_drng_generate_helper	= lrng_cc20_drng_generate_helper,
	.lrng_drng_generate_helper_full	= lrng_cc20_drng_generate_helper_full,
	.lrng_hash_alloc		= lrng_cc20_hash_alloc,
	.lrng_hash_dealloc		= lrng_cc20_hash_dealloc,
	.lrng_hash_digestsize		= lrng_cc20_hash_digestsize,
	.lrng_hash_buffer		= lrng_cc20_hash_buffer,
};
