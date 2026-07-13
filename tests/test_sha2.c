// SPDX-License-Identifier: BSD-3-Clause
/*
 * Known-answer tests for the SHA-256 implementation in src/sha2.c. The
 * skipblock and getsha256digest paths rely on this producing correct
 * digests, so verify it against the standard FIPS 180 test vectors and
 * check that incremental updates match a one-shot digest across the
 * message-block and padding boundaries.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "qdl.h"
#include "sha2.h"

static void digest_hex(const uint8_t *digest, char out[SHA256_DIGEST_STRING_LENGTH])
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		out[i * 2] = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 0xf];
	}
	out[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static void sha256_buf(const void *data, size_t len, char out[SHA256_DIGEST_STRING_LENGTH])
{
	uint8_t digest[SHA256_DIGEST_LENGTH];
	SHA2_CTX ctx;

	SHA256Init(&ctx);
	SHA256Update(&ctx, data, len);
	SHA256Final(digest, &ctx);
	digest_hex(digest, out);
}

static void assert_sha256(const char *msg, const char *expect)
{
	char got[SHA256_DIGEST_STRING_LENGTH];

	sha256_buf(msg, strlen(msg), got);
	assert_string_equal(got, expect);
}

static void test_known_vectors(void **state)
{
	(void)state;

	assert_sha256("",
		      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	assert_sha256("abc",
		      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	/* FIPS 180 two-block-ish 56-byte example. */
	assert_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_million_a(void **state)
{
	char *buf;
	char got[SHA256_DIGEST_STRING_LENGTH];
	(void)state;

	buf = malloc(1000000);
	assert_non_null(buf);
	memset(buf, 'a', 1000000);

	sha256_buf(buf, 1000000, got);
	assert_string_equal(got,
			    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	free(buf);
}

static void oneshot(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LENGTH])
{
	SHA2_CTX ctx;

	SHA256Init(&ctx);
	SHA256Update(&ctx, data, len);
	SHA256Final(out, &ctx);
}

static void chunked(const uint8_t *data, size_t len, size_t chunk,
		    uint8_t out[SHA256_DIGEST_LENGTH])
{
	SHA2_CTX ctx;
	size_t off;

	SHA256Init(&ctx);
	for (off = 0; off < len; off += chunk) {
		size_t n = len - off < chunk ? len - off : chunk;

		SHA256Update(&ctx, data + off, n);
	}
	SHA256Final(out, &ctx);
}

static void test_incremental_matches_oneshot(void **state)
{
	/* Lengths straddling the 64-byte block and 56-byte padding boundary. */
	const size_t lens[] = { 0, 1, 55, 56, 57, 63, 64, 65, 127, 128, 200 };
	const size_t chunks[] = { 1, 3, 7, 13, 32, 64 };
	uint8_t buf[256];
	size_t i;
	size_t j;
	(void)state;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(i * 7 + 1);

	for (i = 0; i < ARRAY_SIZE(lens); i++) {
		uint8_t ref[SHA256_DIGEST_LENGTH];

		oneshot(buf, lens[i], ref);

		for (j = 0; j < ARRAY_SIZE(chunks); j++) {
			uint8_t got[SHA256_DIGEST_LENGTH];

			chunked(buf, lens[i], chunks[j], got);
			assert_memory_equal(got, ref, SHA256_DIGEST_LENGTH);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_known_vectors),
		cmocka_unit_test(test_million_a),
		cmocka_unit_test(test_incremental_matches_oneshot),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
