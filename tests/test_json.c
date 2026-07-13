// SPDX-License-Identifier: BSD-3-Clause
/*
 * Unit tests for the JSON parser in src/json.c. The parser is fully
 * self-contained (no device or ux_* dependencies), so these tests link
 * json.c directly and exercise its public API.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <cmocka.h>

#include "json.h"

static struct json_value *parse_ok(const char *s)
{
	struct json_value *v = json_parse_buf(s, strlen(s));

	assert_non_null(v);
	return v;
}

static void parse_fail(const char *s)
{
	struct json_value *v = json_parse_buf(s, strlen(s));

	assert_null(v);
	/* A recorded error message must accompany every failure. */
	assert_int_not_equal(json_error[0], '\0');
}

static void test_scalar_roots(void **state)
{
	struct json_value *v;
	(void)state;

	v = parse_ok("true");
	assert_int_equal(v->type, JSON_TYPE_TRUE);
	json_free(v);

	v = parse_ok("false");
	assert_int_equal(v->type, JSON_TYPE_FALSE);
	json_free(v);

	v = parse_ok("null");
	assert_int_equal(v->type, JSON_TYPE_NULL);
	json_free(v);

	v = parse_ok("  42  ");
	assert_int_equal(v->type, JSON_TYPE_NUMBER);
	assert_true(v->u.number == 42.0);
	json_free(v);

	v = parse_ok("\"hello\"");
	assert_int_equal(v->type, JSON_TYPE_STRING);
	assert_string_equal(v->u.string, "hello");
	json_free(v);
}

static void test_object_accessors(void **state)
{
	const char *json =
		"{ \"s\": \"hi\", \"n\": 42, \"b\": true, \"nil\": null,"
		"  \"arr\": [10, 20, 30] }";
	struct json_value *root;
	struct json_value *arr;
	double num = 0;
	(void)state;

	root = parse_ok(json);
	assert_int_equal(root->type, JSON_TYPE_OBJECT);

	assert_string_equal(json_get_string(root, "s"), "hi");

	assert_int_equal(json_get_number(root, "n", &num), 0);
	assert_true(num == 42.0);

	assert_non_null(json_get_child(root, "b"));
	assert_int_equal(json_get_child(root, "b")->type, JSON_TYPE_TRUE);
	assert_int_equal(json_get_child(root, "nil")->type, JSON_TYPE_NULL);

	arr = json_get_child(root, "arr");
	assert_non_null(arr);
	assert_int_equal(json_count_children(arr), 3);
	assert_true(json_get_element_object(arr, 0)->u.number == 10.0);
	assert_true(json_get_element_object(arr, 2)->u.number == 30.0);
	assert_null(json_get_element_object(arr, 3));

	/* Type/absence mismatches return the sentinel, not garbage. */
	assert_null(json_get_string(root, "n"));
	assert_null(json_get_string(root, "missing"));
	assert_int_equal(json_get_number(root, "s", &num), -1);
	assert_int_equal(json_count_children(root), -1);

	json_free(root);
}

static void test_nested_and_array_of_objects(void **state)
{
	struct json_value *root;
	struct json_value *a;
	struct json_value *b;
	(void)state;

	root = parse_ok("{\"a\":{\"b\":{\"c\":\"deep\"}}}");
	a = json_get_child(root, "a");
	b = json_get_child(a, "b");
	assert_string_equal(json_get_string(b, "c"), "deep");
	json_free(root);

	root = parse_ok("[{\"k\":\"v0\"},{\"k\":\"v1\"}]");
	assert_int_equal(json_count_children(root), 2);
	assert_string_equal(json_get_string(json_get_element_object(root, 0), "k"), "v0");
	assert_string_equal(json_get_string(json_get_element_object(root, 1), "k"), "v1");
	json_free(root);
}

static void test_string_escapes(void **state)
{
	struct json_value *v;
	(void)state;

	v = parse_ok("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\te\"");
	assert_string_equal(v->u.string, "a\"b\\c/d\b\f\n\r\te");
	json_free(v);
}

static void test_unicode_escapes(void **state)
{
	struct json_value *v;
	(void)state;

	/* BMP: U+0041 'A' and U+00E9 'e-acute' (UTF-8 0xC3 0xA9). */
	v = parse_ok("\"\\u0041\\u00e9\"");
	assert_string_equal(v->u.string, "A\xc3\xa9");
	json_free(v);

	/* Surrogate pair: U+1F600 -> UTF-8 F0 9F 98 80. */
	v = parse_ok("\"\\uD83D\\uDE00\"");
	assert_string_equal(v->u.string, "\xf0\x9f\x98\x80");
	json_free(v);
}

static void test_invalid_surrogates(void **state)
{
	(void)state;

	parse_fail("\"\\uD800\"");         /* lone high surrogate */
	parse_fail("\"\\uDC00\"");         /* lone low surrogate */
	parse_fail("\"\\uD83D\\u0041\"");  /* high followed by non-low */
	parse_fail("\"\\uD83Dx\"");        /* high not followed by escape */
}

static void test_numbers(void **state)
{
	struct json_value *root;
	double n = 0;
	(void)state;

	root = parse_ok("{\"i\":-7,\"f\":3.5,\"e\":2e3,\"z\":0}");
	assert_int_equal(json_get_number(root, "i", &n), 0);
	assert_true(n == -7.0);
	assert_int_equal(json_get_number(root, "f", &n), 0);
	assert_true(n == 3.5);
	assert_int_equal(json_get_number(root, "e", &n), 0);
	assert_true(n == 2000.0);
	assert_int_equal(json_get_number(root, "z", &n), 0);
	assert_true(n == 0.0);
	json_free(root);
}

static void test_number_overflow_rejected(void **state)
{
	(void)state;

	/* Overflow to +inf is rejected. */
	parse_fail("1e400");
}

static void test_number_underflow_accepted(void **state)
{
	struct json_value *v;
	(void)state;

	/*
	 * A subnormal / underflow-to-zero result is a valid JSON number and
	 * must be accepted (strtod reports ERANGE for it, which must not be
	 * treated as a parse error).
	 */
	v = parse_ok("1e-310");
	assert_int_equal(v->type, JSON_TYPE_NUMBER);
	json_free(v);
}

static void test_duplicate_keys_first_wins(void **state)
{
	struct json_value *root;
	(void)state;

	root = parse_ok("{\"k\":\"first\",\"k\":\"second\"}");
	assert_string_equal(json_get_string(root, "k"), "first");
	json_free(root);
}

static char *make_nested(unsigned int depth)
{
	char *s = malloc((size_t)depth * 2 + 1);
	unsigned int i;

	assert_non_null(s);
	for (i = 0; i < depth; i++)
		s[i] = '[';
	for (i = 0; i < depth; i++)
		s[depth + i] = ']';
	s[depth * 2] = '\0';
	return s;
}

static void test_nesting_depth_limit(void **state)
{
	struct json_value *v;
	char *s;
	(void)state;

	/* JSON_MAX_DEPTH levels are accepted. */
	s = make_nested(JSON_MAX_DEPTH);
	v = json_parse_buf(s, strlen(s));
	assert_non_null(v);
	json_free(v);
	free(s);

	/* One level deeper is rejected instead of overflowing the stack. */
	s = make_nested(JSON_MAX_DEPTH + 1);
	assert_null(json_parse_buf(s, strlen(s)));
	free(s);
}

static void test_malformed_inputs(void **state)
{
	(void)state;

	parse_fail("");
	parse_fail("{");
	parse_fail("[1,2");
	parse_fail("{\"a\" 1}");     /* missing colon */
	parse_fail("{\"a\":1,}");    /* trailing comma */
	parse_fail("{}trailing");    /* trailing token after root */
	parse_fail("nul");           /* truncated keyword */
	parse_fail("\"unterminated");
	parse_fail("\"bad\\xescape\"");
}

static void test_oversized_length_rejected(void **state)
{
	char buf[4] = "1";
	(void)state;

	/*
	 * A length above INT_MAX must be rejected up front (the internal
	 * position/length counters are int) rather than parsing a truncated
	 * view of the input. The guard runs before the buffer is read.
	 */
	assert_null(json_parse_buf(buf, (size_t)INT_MAX + 1));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_scalar_roots),
		cmocka_unit_test(test_object_accessors),
		cmocka_unit_test(test_nested_and_array_of_objects),
		cmocka_unit_test(test_string_escapes),
		cmocka_unit_test(test_unicode_escapes),
		cmocka_unit_test(test_invalid_surrogates),
		cmocka_unit_test(test_numbers),
		cmocka_unit_test(test_number_overflow_rejected),
		cmocka_unit_test(test_number_underflow_accepted),
		cmocka_unit_test(test_duplicate_keys_first_wins),
		cmocka_unit_test(test_nesting_depth_limit),
		cmocka_unit_test(test_malformed_inputs),
		cmocka_unit_test(test_oversized_length_rejected),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
