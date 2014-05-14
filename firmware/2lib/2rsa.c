/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Implementation of RSA signature verification which uses a pre-processed key
 * for computation. The code extends Android's RSA verification code to support
 * multiple RSA key lengths and hash digest algorithms.
 */

#include "2sysincludes.h"
#include "2common.h"
#include "2rsa.h"
#include "2sha.h"

/**
 * a[] -= mod
 */
static void subM(const struct vb2_public_key *key, uint32_t *a)
{
	int64_t A = 0;
	uint32_t i;
	for (i = 0; i < key->arrsize; ++i) {
		A += (uint64_t)a[i] - key->n[i];
		a[i] = (uint32_t)A;
		A >>= 32;
	}
}

/**
 * Return a[] >= mod
 */
int vb2_mont_ge(const struct vb2_public_key *key, uint32_t *a)
{
	uint32_t i;
	for (i = key->arrsize; i;) {
		--i;
		if (a[i] < key->n[i])
			return 0;
		if (a[i] > key->n[i])
			return 1;
	}
	return 1;  /* equal */
}

/**
 * Montgomery c[] += a * b[] / R % mod
 */
static void montMulAdd(const struct vb2_public_key *key,
                       uint32_t *c,
                       const uint32_t a,
                       const uint32_t *b)
{
	uint64_t A = (uint64_t)a * b[0] + c[0];
	uint32_t d0 = (uint32_t)A * key->n0inv;
	uint64_t B = (uint64_t)d0 * key->n[0] + (uint32_t)A;
	uint32_t i;

	for (i = 1; i < key->arrsize; ++i) {
		A = (A >> 32) + (uint64_t)a * b[i] + c[i];
		B = (B >> 32) + (uint64_t)d0 * key->n[i] + (uint32_t)A;
		c[i - 1] = (uint32_t)B;
	}

	A = (A >> 32) + (B >> 32);

	c[i - 1] = (uint32_t)A;

	if (A >> 32) {
		subM(key, c);
	}
}

/**
 * Montgomery c[] = a[] * b[] / R % mod
 */
static void montMul(const struct vb2_public_key *key,
                    uint32_t *c,
                    const uint32_t *a,
                    const uint32_t *b)
{
	uint32_t i;
	for (i = 0; i < key->arrsize; ++i) {
		c[i] = 0;
	}
	for (i = 0; i < key->arrsize; ++i) {
		montMulAdd(key, c, a[i], b);
	}
}

/**
 * In-place public exponentiation. (65537}
 *
 * @param key		Key to use in signing
 * @param inout		Input and output big-endian byte array
 * @param workbuf32	Work buffer; caller must verify this is
 *			(3 * key->arrsize) elements long.
 */
static void modpowF4(const struct vb2_public_key *key, uint8_t *inout,
		    uint32_t *workbuf32)
{
	uint32_t *a = workbuf32;
	uint32_t *aR = a + key->arrsize;
	uint32_t *aaR = aR + key->arrsize;
	uint32_t *aaa = aaR;  /* Re-use location. */
	int i;

	/* Convert from big endian byte array to little endian word array. */
	for (i = 0; i < (int)key->arrsize; ++i) {
		uint32_t tmp =
			(inout[((key->arrsize - 1 - i) * 4) + 0] << 24) |
			(inout[((key->arrsize - 1 - i) * 4) + 1] << 16) |
			(inout[((key->arrsize - 1 - i) * 4) + 2] << 8) |
			(inout[((key->arrsize - 1 - i) * 4) + 3] << 0);
		a[i] = tmp;
	}

	montMul(key, aR, a, key->rr);  /* aR = a * RR / R mod M   */
	for (i = 0; i < 16; i+=2) {
		montMul(key, aaR, aR, aR);  /* aaR = aR * aR / R mod M */
		montMul(key, aR, aaR, aaR);  /* aR = aaR * aaR / R mod M */
	}
	montMul(key, aaa, aR, a);  /* aaa = aR * a / R mod M */


	/* Make sure aaa < mod; aaa is at most 1x mod too large. */
	if (vb2_mont_ge(key, aaa)) {
		subM(key, aaa);
	}

	/* Convert to bigendian byte array */
	for (i = (int)key->arrsize - 1; i >= 0; --i) {
		uint32_t tmp = aaa[i];
		*inout++ = (uint8_t)(tmp >> 24);
		*inout++ = (uint8_t)(tmp >> 16);
		*inout++ = (uint8_t)(tmp >>  8);
		*inout++ = (uint8_t)(tmp >>  0);
	}
}

/**
 * Safer memcmp() for use in crypto.
 *
 * Compares the buffers to see if they are equal.  Time taken to perform
 * the comparison is dependent only on the size, not the relationship of
 * the match between the buffers.  Note that unlike memcmp(), this only
 * indicates inequality, not which buffer is lesser.
 *
 * @param s1		First buffer
 * @param s2		Second buffer
 * @param size		Number of bytes to compare
 * @return 0 if match or size=0, non-zero if at least one byte mismatched.
 */
int vb2_safe_memcmp(const void *s1, const void *s2, size_t size)
{
	const unsigned char *us1 = s1;
	const unsigned char *us2 = s2;
	int result = 0;

	if (0 == size)
		return 0;

	/*
	 * Code snippet without data-dependent branch due to Nate Lawson
	 * (nate@root.org) of Root Labs.
	 */
	while (size--)
		result |= *us1++ ^ *us2++;

	return result != 0;
}

uint32_t vb2_rsa_sig_size(uint32_t algorithm)
{
	switch (algorithm) {
	case VB2_ALG_RSA1024_SHA1:
	case VB2_ALG_RSA1024_SHA256:
	case VB2_ALG_RSA1024_SHA512:
		return 1024 / 8;
	case VB2_ALG_RSA2048_SHA1:
	case VB2_ALG_RSA2048_SHA256:
	case VB2_ALG_RSA2048_SHA512:
		return 2048 / 8;
	case VB2_ALG_RSA4096_SHA1:
	case VB2_ALG_RSA4096_SHA256:
	case VB2_ALG_RSA4096_SHA512:
		return 4096 / 8;
	case VB2_ALG_RSA8192_SHA1:
	case VB2_ALG_RSA8192_SHA256:
	case VB2_ALG_RSA8192_SHA512:
		return 8192 / 8;
	default:
		return 0;
	}
}

uint32_t vb2_packed_key_size(uint32_t algorithm)
{
	if (algorithm >= VB2_ALG_COUNT)
		return 0;

	/*
	 * Total size needed by a RSAPublicKey buffer is =
	 *  2 * key_len bytes for the n and rr arrays
	 *  + sizeof len + sizeof n0inv.
	 */
	return 2 * vb2_rsa_sig_size(algorithm) + 2 * sizeof(uint32_t);
}

/*
 * PKCS 1.5 padding (from the RSA PKCS#1 v2.1 standard)
 *
 * Depending on the RSA key size and hash function, the padding is calculated
 * as follows:
 *
 * 0x00 || 0x01 || PS || 0x00 || T
 *
 * T: DER Encoded DigestInfo value which depends on the hash function used.
 *
 * SHA-1:   (0x)30 21 30 09 06 05 2b 0e 03 02 1a 05 00 04 14 || H.
 * SHA-256: (0x)30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20 || H.
 * SHA-512: (0x)30 51 30 0d 06 09 60 86 48 01 65 03 04 02 03 05 00 04 40 || H.
 *
 * Length(T) = 35 octets for SHA-1
 * Length(T) = 51 octets for SHA-256
 * Length(T) = 83 octets for SHA-512
 *
 * PS: octet string consisting of {Length(RSA Key) - Length(T) - 3} 0xFF
 */
static const uint8_t sha1_tail[] = {
	0x00,0x30,0x21,0x30,0x09,0x06,0x05,0x2b,
	0x0e,0x03,0x02,0x1a,0x05,0x00,0x04,0x14
};

static const uint8_t sha256_tail[] = {
	0x00,0x30,0x31,0x30,0x0d,0x06,0x09,0x60,
	0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
	0x05,0x00,0x04,0x20
};

static const uint8_t sha512_tail[] = {
	0x00,0x30,0x51,0x30,0x0d,0x06,0x09,0x60,
	0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,
	0x05,0x00,0x04,0x40
};

/**
 * Check pkcs 1.5 padding bytes
 *
 * @param sig		Signature to verify
 * @param algorithm	Key algorithm
 * @return VB2_SUCCESS, or non-zero if error.
 */
int vb2_check_padding(uint8_t *sig, int algorithm)
{
	/* Determine padding to use depending on the signature type */
	uint32_t pad_size = vb2_rsa_sig_size(algorithm) -
		vb2_digest_size(algorithm);
	const uint8_t *tail;
	uint32_t tail_size;
	int result = 0;

	int i;

	switch (algorithm) {
	case VB2_ALG_RSA1024_SHA1:
	case VB2_ALG_RSA2048_SHA1:
	case VB2_ALG_RSA4096_SHA1:
	case VB2_ALG_RSA8192_SHA1:
		tail = sha1_tail;
		tail_size = sizeof(sha1_tail);
		break;
	case VB2_ALG_RSA1024_SHA256:
	case VB2_ALG_RSA2048_SHA256:
	case VB2_ALG_RSA4096_SHA256:
	case VB2_ALG_RSA8192_SHA256:
		tail = sha256_tail;
		tail_size = sizeof(sha256_tail);
		break;
	case VB2_ALG_RSA1024_SHA512:
	case VB2_ALG_RSA2048_SHA512:
	case VB2_ALG_RSA4096_SHA512:
	case VB2_ALG_RSA8192_SHA512:
		tail = sha512_tail;
		tail_size = sizeof(sha512_tail);
		break;
	default:
		return VB2_ERROR_BAD_ALGORITHM;
	}

	/* First 2 bytes are always 0x00 0x01 */
	result |= *sig++ ^ 0x00;
	result |= *sig++ ^ 0x01;

	/* Then 0xff bytes until the tail */
	for (i = 0; i < pad_size - tail_size - 2; i++)
		result |= *sig++ ^ 0xff;

	/*
	 * Then the tail.  Even though there are probably no timing issues
	 * here, we use vb2_safe_memcmp() just to be on the safe side.
	 */
	result |= vb2_safe_memcmp(sig, tail, tail_size);

	return result ? VB2_ERROR_BAD_SIGNATURE : VB2_SUCCESS;
}

int vb2_verify_digest(const struct vb2_public_key *key,
		      uint8_t *sig,
		      const uint8_t *digest,
		      struct vb2_workbuf *wb)
{
	struct vb2_workbuf wblocal = *wb;
	uint32_t *workbuf32;
	uint32_t key_bytes = key->arrsize * sizeof(uint32_t);
	int pad_size;
	int rv;

	if (!key || !sig || !digest)
		return VB2_ERROR_UNKNOWN;

	if (key->algorithm >= VB2_ALG_COUNT) {
		VB2_DEBUG("Invalid signature type!\n");
		return VB2_ERROR_BAD_ALGORITHM;
	}

	/* Signature length should be same as key length */
	if (key_bytes != vb2_rsa_sig_size(key->algorithm)) {
		VB2_DEBUG("Signature is of incorrect length!\n");
		return VB2_ERROR_BAD_SIGNATURE;
	}

	workbuf32 = vb2_workbuf_alloc(&wblocal, 3 * key_bytes);
	if (!workbuf32)
		return VB2_ERROR_UNKNOWN;

	modpowF4(key, sig, workbuf32);

	vb2_workbuf_free(&wblocal, 3 * key_bytes);

	/* Check padding */
	rv = vb2_check_padding(sig, key->algorithm);
	if (rv)
		return rv;

	/*
	 * Check digest.  Even though there are probably no timing issues here,
	 * use vb2_safe_memcmp() just to be on the safe side.  (That's also why
	 * we don't return before this check if the padding check failed.)
	 */
	pad_size = vb2_rsa_sig_size(key->algorithm) -
		vb2_digest_size(key->algorithm);

	if (vb2_safe_memcmp(sig + pad_size, digest, key_bytes - pad_size)) {
		VB2_DEBUG("Digest check failed!\n");
		rv = VB2_ERROR_BAD_SIGNATURE;
	}

	return rv;
}