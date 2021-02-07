// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Backend for the LRNG providing the cryptographic primitives using
 * ChaCha20 cipher implementations.
 *
 * Copyright (C) 2016 - 2021, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/chacha.h>
#include <crypto/sha.h>
#include <linux/lrng.h>
#include <linux/random.h>
#include <linux/slab.h>

#include "lrng_chacha20.h"
#include "lrng_internal.h"

/******************************* ChaCha20 DRNG *******************************/

#define CHACHA_BLOCK_WORDS	(CHACHA_BLOCK_SIZE / sizeof(u32))

struct chacha20_state {
	struct chacha20_block block;
};

/*
 * Have a static memory blocks for the ChaCha20 DRNG instance to avoid calling
 * kmalloc too early in the boot cycle. For subsequent allocation requests,
 * such as per-NUMA-node DRNG instances, kmalloc will be used.
 */
struct chacha20_state chacha20 __latent_entropy;

/**
 * Update of the ChaCha20 state by either using an unused buffer part or by
 * generating one ChaCha20 block which is half of the state of the ChaCha20.
 * The block is XORed into the key part of the state. This shall ensure
 * backtracking resistance as well as a proper mix of the ChaCha20 state once
 * the key is injected.
 */
static void lrng_chacha20_update(struct chacha20_state *chacha20_state,
				 __le32 *buf, u32 used_words)
{
	struct chacha20_block *chacha20 = &chacha20_state->block;
	u32 i;
	__le32 tmp[CHACHA_BLOCK_WORDS];

	BUILD_BUG_ON(sizeof(struct chacha20_block) != CHACHA_BLOCK_SIZE);
	BUILD_BUG_ON(CHACHA_BLOCK_SIZE != 2 * CHACHA_KEY_SIZE);

	if (used_words > CHACHA_KEY_SIZE_WORDS) {
		chacha20_block(&chacha20->constants[0], (u8 *)tmp);
		for (i = 0; i < CHACHA_KEY_SIZE_WORDS; i++)
			chacha20->key.u[i] ^= le32_to_cpu(tmp[i]);
		memzero_explicit(tmp, sizeof(tmp));
	} else {
		for (i = 0; i < CHACHA_KEY_SIZE_WORDS; i++)
			chacha20->key.u[i] ^= le32_to_cpu(buf[i + used_words]);
	}

	/* Deterministic increment of nonce as required in RFC 7539 chapter 4 */
	chacha20->nonce[0]++;
	if (chacha20->nonce[0] == 0)
		chacha20->nonce[1]++;
	if (chacha20->nonce[1] == 0)
		chacha20->nonce[2]++;

	/* Leave counter untouched as it is start value is undefined in RFC */
}

/*
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

/*
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
	__le32 aligned_buf[CHACHA_BLOCK_WORDS];
	u32 ret = outbuflen, used = CHACHA_BLOCK_WORDS;
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

static inline void
_lrng_cc20_init_state_common(u32 *val, bool (*seed)(unsigned long *v),
			     bool (*rand)(unsigned long *v))
{
	unsigned long v;

	*val ^= jiffies;
	*val ^= random_get_entropy();
	if (seed(&v) || rand(&v))
		*val ^= v;
}

static void lrng_cc20_init_state_common(struct chacha20_state *state,
					bool (*seed)(unsigned long *v),
					bool (*rand)(unsigned long *v))
{
	struct chacha20_block *chacha20 = &state->block;
	u32 i;

	lrng_cc20_init_rfc7539(chacha20);

	for (i = 0; i < CHACHA_KEY_SIZE_WORDS; i++)
		_lrng_cc20_init_state_common(&chacha20->key.u[i], seed, rand);

	for (i = 0; i < 3; i++)
		_lrng_cc20_init_state_common(&chacha20->nonce[i], seed, rand);

	lrng_chacha20_update(state, NULL, CHACHA_BLOCK_WORDS);
}

void lrng_cc20_init_state(struct chacha20_state *state)
{
	lrng_cc20_init_state_common(state, arch_get_random_seed_long,
				    arch_get_random_long);
	pr_info("ChaCha20 core initialized\n");
}

void __init lrng_cc20_init_state_boot(struct chacha20_state *state)
{
	lrng_cc20_init_state_common(state, arch_get_random_seed_long_early,
				    arch_get_random_long_early);
}

/*
 * Allocation of the DRNG state
 */
static void *lrng_cc20_drng_alloc(u32 sec_strength)
{
	struct chacha20_state *state = NULL;

	if (sec_strength > CHACHA_KEY_SIZE) {
		pr_err("Security strength of ChaCha20 DRNG (%u bits) lower than requested by LRNG (%u bits)\n",
			CHACHA_KEY_SIZE * 8, sec_strength * 8);
		return ERR_PTR(-EINVAL);
	}
	if (sec_strength < CHACHA_KEY_SIZE)
		pr_warn("Security strength of ChaCha20 DRNG (%u bits) higher than requested by LRNG (%u bits)\n",
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

	if (drng == &chacha20) {
		memzero_explicit(chacha20_state, sizeof(*chacha20_state));
		pr_debug("static ChaCha20 core zeroized\n");
		return;
	}

	pr_debug("ChaCha20 core zeroized and freed\n");
	kfree_sensitive(chacha20_state);
}

/******************************* Hash Operation *******************************/

#ifdef CONFIG_CRYPTO_LIB_SHA256

static u32 lrng_cc20_hash_digestsize(void *hash)
{
	return SHA256_DIGEST_SIZE;
}

static int lrng_cc20_hash_init(struct shash_desc *shash, void *hash)
{
	/*
	 * We do not need a TFM - we only need sufficient space for
	 * struct sha1_state on the stack.
	 */
	sha256_init(shash_desc_ctx(shash));
	return 0;
}

static int lrng_cc20_hash_update(struct shash_desc *shash,
				 const u8 *inbuf, u32 inbuflen)
{
	sha256_update(shash_desc_ctx(shash), inbuf, inbuflen);
	return 0;
}

static int lrng_cc20_hash_final(struct shash_desc *shash, u8 *digest)
{
	sha256_final(shash_desc_ctx(shash), digest);
	return 0;
}

static const char *lrng_cc20_hash_name(void)
{
	return "SHA-256";
}

#else /* CONFIG_CRYPTO_LIB_SHA256 */

#include <crypto/sha1_base.h>

/*
 * If the SHA-256 support is not compiled, we fall back to SHA-1 that is always
 * compiled and present in the kernel.
 */
static u32 lrng_cc20_hash_digestsize(void *hash)
{
	return SHA1_DIGEST_SIZE;
}

static void lrng_sha1_block_fn(struct sha1_state *sctx, const u8 *src,
			       int blocks)
{
	u32 temp[SHA1_WORKSPACE_WORDS];

	while (blocks--) {
		sha1_transform(sctx->state, src, temp);
		src += SHA1_BLOCK_SIZE;
	}
	memzero_explicit(temp, sizeof(temp));
}

static int lrng_cc20_hash_init(struct shash_desc *shash, void *hash)
{
	/*
	 * We do not need a TFM - we only need sufficient space for
	 * struct sha1_state on the stack.
	 */
	sha1_base_init(shash);
	return 0;
}

static int lrng_cc20_hash_update(struct shash_desc *shash,
				 const u8 *inbuf, u32 inbuflen)
{
	return sha1_base_do_update(shash, inbuf, inbuflen, lrng_sha1_block_fn);
}

static int lrng_cc20_hash_final(struct shash_desc *shash, u8 *digest)
{
	return sha1_base_do_finalize(shash, lrng_sha1_block_fn) ?:
	       sha1_base_finish(shash, digest);
}

static const char *lrng_cc20_hash_name(void)
{
	return "SHA-1";
}

#endif /* CONFIG_CRYPTO_LIB_SHA256 */

static void *lrng_cc20_hash_alloc(void)
{
	pr_info("Hash %s allocated\n", lrng_cc20_hash_name());
	return NULL;
}

static void lrng_cc20_hash_dealloc(void *hash)
{
}

static const char *lrng_cc20_drng_name(void)
{
	return "ChaCha20 DRNG";
}

const struct lrng_crypto_cb lrng_cc20_crypto_cb = {
	.lrng_drng_name			= lrng_cc20_drng_name,
	.lrng_hash_name			= lrng_cc20_hash_name,
	.lrng_drng_alloc		= lrng_cc20_drng_alloc,
	.lrng_drng_dealloc		= lrng_cc20_drng_dealloc,
	.lrng_drng_seed_helper		= lrng_cc20_drng_seed_helper,
	.lrng_drng_generate_helper	= lrng_cc20_drng_generate_helper,
	.lrng_hash_alloc		= lrng_cc20_hash_alloc,
	.lrng_hash_dealloc		= lrng_cc20_hash_dealloc,
	.lrng_hash_digestsize		= lrng_cc20_hash_digestsize,
	.lrng_hash_init			= lrng_cc20_hash_init,
	.lrng_hash_update		= lrng_cc20_hash_update,
	.lrng_hash_final		= lrng_cc20_hash_final,
};
