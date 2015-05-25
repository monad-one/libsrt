/*
 * senc.c
 *
 * Buffer encoding/decoding
 *
 * Copyright (c) 2015 F. Aragon. All rights reserved.
 */

#include "senc.h"
#include <stdlib.h>

#define SLZW_ENABLE_RLE
#if defined(SLZW_ENABLE_RLE) && defined(S_UNALIGNED_MEMORY_ACCESS)
#define SLZW_ENABLE_RLE_ENC
#endif

/*
 * Constants
 */

static const char b64e[64] = {
	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
	'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b',
	'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
	'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3',
	'4', '5', '6', '7', '8', '9', '+', '/'
	};
static const char b64d[128] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0,
	0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	21, 22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33,
	34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
	51, 0, 0, 0, 0, 0
	};
static const char n2h_l[16] = {
	48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 97, 98, 99, 100, 101, 102
	};
static const char n2h_u[16] = {
	48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 65, 66, 67, 68, 69, 70
	};
static const char h2n[64] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0,
	0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 10, 11, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

#define SLZW_MAX_TREE_BITS	12
#define SLZW_CODE_LIMIT		(1 << SLZW_MAX_TREE_BITS)
#define SLZW_MAX_CODE		(SLZW_CODE_LIMIT - 1)
#define SLZW_ROOT_NODE_BITS	8
#define SLZW_RESET		(1 << SLZW_ROOT_NODE_BITS)
#define SLZW_STOP		(SLZW_RESET + 1)
#ifdef SLZW_ENABLE_RLE
	#define SLZW_RLE	(SLZW_STOP + 1)
	#define SLZW_FIRST	(SLZW_RLE + 1)
	#define SLZW_RLE_CSIZE	16
	#if S_BPWORD >= 8
		#define SLRE_CMPS 8
		typedef suint_t srle_cmp_t;
	#else
		#define SLRE_CMPS 4
		typedef suint32_t srle_cmp_t;
	#endif
#else
	#define SLZW_FIRST	(SLZW_STOP + 1)
#endif
#define SLZW_LUT_CHILD_ELEMS	256
#define SLZW_MAX_LUTS		(SLZW_CODE_LIMIT / 8)

/*
 * Macros
 */

#define EB64C1(a)	(a >> 2)
#define EB64C2(a, b)	((a & 3) << 4 | b >> 4)
#define EB64C3(b, c)	((b & 0xf) << 2 | c >> 6)
#define EB64C4(c)	(c & 0x3f)
#define DB64C1(a, b)	(a << 2 | b >> 4)
#define DB64C2(b, c)	(b << 4 | c >> 2)
#define DB64C3(c, d)	(c << 6 | d)

#define SLZW_ENC_WRITE(ob, oi, acc, code, curr_code_len) {	\
		int c = code, code_bits = curr_code_len;	\
		if (acc) {					\
			int xbits = 8 - acc;			\
			ob[oi++] |= (c << acc);			\
			c >>= xbits;				\
			code_bits -= xbits;			\
		}						\
		size_t copy_size = code_bits / 8;		\
		switch (copy_size) {				\
		case 3: ob[oi++] = c; c >>= 8;			\
		case 2: ob[oi++] = c; c >>= 8;			\
		case 1: ob[oi++] = c; c >>= 8;			\
		}						\
		acc = code_bits % 8;				\
		if (acc)					\
			ob[oi] = c;				\
	}

#define SLZW_ENC_RESET(node_codes, node_lutref, lut_stack_in_use,	\
		       node_stack_in_use, next_code, curr_code_len) {	\
		int j;							\
		for (j = 0; j < 256; j += 4) {				\
			node_codes[j] = j;				\
			node_codes[j + 1] = j + 1;			\
			node_codes[j + 2] = j + 2;			\
			node_codes[j + 3] = j + 3;			\
		}							\
		memset(node_lutref, 0, 256 * sizeof(node_lutref[0]));	\
		lut_stack_in_use = 1;					\
		node_stack_in_use = 256;				\
		SLZW_ENC_WRITE(o, oi, acc, SLZW_RESET, curr_code_len);	\
		curr_code_len = SLZW_ROOT_NODE_BITS + 1;		\
		next_code = SLZW_FIRST;					\
	}

#define SLZW_DEC_READ(nc, s, ss, i, acc, accbuf, curr_code_len) {	\
		int cbits = curr_code_len;				\
		nc = 0;							\
		if (acc) {						\
			nc |= accbuf;					\
			cbits -= acc;					\
		}							\
		if (cbits >= 8) {					\
			nc |= (s[i++] << acc);				\
			cbits -= 8;					\
			acc += 8;					\
		}							\
		if (cbits > 0) {					\
			accbuf = s[i++];				\
			nc |= ((accbuf & S_NBITMASK(cbits)) << acc);	\
			accbuf >>= cbits;				\
			acc = 8 - cbits;				\
		} else {						\
			acc = 0;					\
		}							\
	}

#define SLZW_DEC_RESET(j, curr_code_len, last_code, next_inc_code,	\
		       parents) {					\
		curr_code_len = SLZW_ROOT_NODE_BITS + 1;		\
		last_code = SLZW_CODE_LIMIT;				\
		next_inc_code = SLZW_FIRST;				\
		for (j = 0; j < 256; j += 4) 				\
			parents[j] = parents[j + 1] = parents[j + 2] =	\
				     parents[j + 3] = SLZW_CODE_LIMIT;	\
	}

/*
 * Internal functions
 */

static int hex2nibble(const int h)
{
	return h2n[(h - 48) & 0x3f];
}

static size_t senc_hex_aux(const unsigned char *s, const size_t ss, unsigned char *o, const char *t)
{
	RETURN_IF(!s, ss * 2);
	ASSERT_RETURN_IF(!o, 0);
	const size_t out_size = ss * 2;
	size_t i = ss, j = out_size;
	size_t tail = ss - (ss % 2);
	#define ENCHEX_LOOP(ox, ix) {		\
		const int next = s[ix - 1];	\
		o[ox - 2] = t[next >> 4];	\
		o[ox - 1] = t[next & 0x0f];	\
		}
	if (tail)
		for (; i > 0; i--, j -= 2)
			ENCHEX_LOOP(j, i);
	for (; i > 0; i -= 2, j -= 4) {
		ENCHEX_LOOP(j - 2, i - 1);
		ENCHEX_LOOP(j, i);
	}
	#undef ENCHEX_LOOP
	return out_size;
}

static void slzw_setseq256s8(unsigned *p)
{
	union { unsigned a32; char b[4]; } acc;
	acc.b[0] = 0;
	acc.b[1] = 1;
	acc.b[2] = 2;
	acc.b[3] = 3;
	unsigned j;
	for (j = 0; j < 256 / 4; j++) {
		p[j] = acc.a32;
		acc.a32 += 0x04040404;
	}
}

/*
 * Base64 encoding/decoding
 */

size_t senc_b64(const unsigned char *s, const size_t ss, unsigned char *o)
{
	RETURN_IF(!s, (ss / 3 + (ss % 3 ? 1 : 0)) * 4);
	ASSERT_RETURN_IF(!o, 0);
	const size_t ssod4 = (ss / 3) * 4;
	const size_t ssd3 = ss - (ss % 3);
	const size_t tail = ss - ssd3;
	size_t i = ssd3, j = ssod4 + (tail ? 4 : 0);
	const size_t out_size = j;
	unsigned si0, si1, si2;
	switch (tail) {
	case 2: si0 = s[ssd3], si1 = s[ssd3 + 1];
		o[j - 4] = b64e[EB64C1(si0)];
		o[j - 3] = b64e[EB64C2(si0, si1)];
		o[j - 2] = b64e[EB64C3(si1, 0)];
		o[j - 1] = '=';
		j -= 4;
		break;
	case 1: si0 = s[ssd3];
		o[j - 4] = b64e[EB64C1(si0)];
		o[j - 3] = b64e[EB64C2(si0, 0)];
		o[j - 2] = '=';
		o[j - 1] = '=';
		j -= 4;
	}
	for (; i > 0; i -= 3, j -= 4) {
		si0 = s[i - 3], si1 = s[i - 2], si2 = s[i - 1];
		o[j - 4] = b64e[EB64C1(si0)];
		o[j - 3] = b64e[EB64C2(si0, si1)];
		o[j - 2] = b64e[EB64C3(si1, si2)];
		o[j - 1] = b64e[EB64C4(si2)];
	}
	return out_size;
}

size_t sdec_b64(const unsigned char *s, const size_t ss, unsigned char *o)
{
	RETURN_IF(!s, (ss / 4) * 3);
	ASSERT_RETURN_IF(!o, 0);
	size_t i = 0, j = 0;
	const size_t ssd4 = ss - (ss % 4);
	const size_t tail = s[ss - 2] == '=' || s[ss - 1] == '=' ? 4 : 0;
	for (; i  < ssd4 - tail; i += 4, j += 3) {
		const int a = b64d[s[i]], b = b64d[s[i + 1]],
			  c = b64d[s[i + 2]], d = b64d[s[i + 3]];
		o[j] = DB64C1(a, b);
		o[j + 1] = DB64C2(b, c);
		o[j + 2] = DB64C3(c, d);
	}
	if (tail) {
		const int a = b64d[s[i] & 0x7f], b = b64d[s[i + 1] & 0x7f];
		o[j++] = DB64C1(a, b);
		if (s[i + 2] != '=') {
			const int c = b64d[s[i + 2] & 0x7f];
			o[j++] = DB64C2(b, c);
		}
	}
	return j;
}

/*
 * Hexadecimal encoding/decoding
 */

size_t senc_hex(const unsigned char *s, const size_t ss, unsigned char *o)
{
	return senc_hex_aux(s, ss, o, n2h_l);
}

size_t senc_HEX(const unsigned char *s, const size_t ss, unsigned char *o)
{
	return senc_hex_aux(s, ss, o, n2h_u);
}

size_t sdec_hex(const unsigned char *s, const size_t ss, unsigned char *o)
{
	RETURN_IF(!s, ss / 2);
	ASSERT_RETURN_IF(!o, 0);
	const size_t ssd2 = ss - (ss % 2),
		     ssd4 = ss - (ss % 4);
	ASSERT_RETURN_IF(!ssd2, 0);
	size_t i = 0, j = 0;
	#define SDEC_HEX_L(n, m)	\
		o[j + n] = (hex2nibble(s[i + m]) << 4) | \
			    hex2nibble(s[i + m + 1]);
	for (; i < ssd4; i += 4, j += 2) {
		SDEC_HEX_L(0, 0);
		SDEC_HEX_L(1, 2);
	}
	for (; i < ssd2; i += 2, j += 1)
		SDEC_HEX_L(0, 0);
	#undef SDEC_HEX_L
	return j;
}

/*
 * LZW encoding/decoding
 */

typedef short slzw_ndx_t;

size_t senc_lzw(const unsigned char *s, const size_t ss, unsigned char *o)
{
	RETURN_IF(!s || !o || !ss, 0);
	/*
	 * Node structure (separated elements and not a "struct", in order to
	 * save space because of data alignment)
	 *
	 * node_codes[i]: LZW output code for the node
	 * node_lutref[i]: 0: empty, < 0: -next_node, > 0: 256-child LUT ref.
	 * node_child[i]: if node_lutref[0] < 0: next node byte (one-child node)
	 */
        slzw_ndx_t node_codes[SLZW_CODE_LIMIT],
		   node_lutref[SLZW_CODE_LIMIT],
		   lut_stack[SLZW_MAX_LUTS][SLZW_LUT_CHILD_ELEMS];
        unsigned char node_child[SLZW_CODE_LIMIT];
	/*
	 * Stack allocation control
	 */
        size_t node_stack_in_use, lut_stack_in_use;
	/*
	 * Output encoding control
	 */
        size_t oi = 0;
        int next_code, curr_code_len, acc = 0;
	/*
	 * Initialize data structures
	 */
	SLZW_ENC_RESET(node_codes, node_lutref, lut_stack_in_use,
		       node_stack_in_use, next_code, curr_code_len);
	/*
	 * Encoding loop
	 */
	size_t i = 0;
	slzw_ndx_t curr_node;
	for (; i < ss;) {
#ifdef SLZW_ENABLE_RLE_ENC
		if ((i + SLRE_CMPS * 2) < ss) {
			const srle_cmp_t *u = (srle_cmp_t *)(s + i),
					 *v = (srle_cmp_t *)(s + i + 1),
					 u0 = u[0];
			if (u0 == u[1] && u0 == v[0]) {
				int j = (i + SLRE_CMPS);
				int max_cs = i + ((1 << curr_code_len) - 1) *
					     SLZW_RLE_CSIZE;
				int ss2 = S_MIN(ss, max_cs);
				for (; j + SLRE_CMPS < ss2 ; j += SLRE_CMPS)
					if (u0 != *(srle_cmp_t *)(s + j + SLRE_CMPS))
						break;
				if (j - i >= SLZW_RLE_CSIZE) {
					int count_cs = (j - i) / SLZW_RLE_CSIZE;
					SLZW_ENC_WRITE(o, oi, acc, SLZW_RLE, curr_code_len);
					SLZW_ENC_WRITE(o, oi, acc, count_cs, curr_code_len);
					SLZW_ENC_WRITE(o, oi, acc, s[i], curr_code_len);
					i += count_cs * SLZW_RLE_CSIZE;
				}
			}
			if (i == ss)
				break;
		}
#endif
		/*
		 * Locate pattern
		 */
		unsigned in_byte = s[i++];
		curr_node = in_byte;
		slzw_ndx_t r;
		for (; i < ss; i++) {
			in_byte = s[i];
			const slzw_ndx_t nlut = node_lutref[curr_node];
			if (nlut < 0 && in_byte == node_child[curr_node]) {
				curr_node = -nlut;
				continue;
			}
			if (nlut <= 0 || !(r = lut_stack[nlut][in_byte]))
				break;
			curr_node = r;
		}
		if (i == ss)
			break;
		/*
		 * Add new code to the tree
		 */
		const slzw_ndx_t new_node = node_stack_in_use++;
		if (node_lutref[curr_node] <= 0) {
			if (node_lutref[curr_node] == 0) { /* empty */
				node_lutref[curr_node] = -new_node;
				node_child[curr_node] = in_byte;
			} else { /* single node: convert it to LUT */
				slzw_ndx_t alt_n = -node_lutref[curr_node];
				slzw_ndx_t new_lut = lut_stack_in_use++;
				node_lutref[curr_node] = new_lut;
				memset(&lut_stack[new_lut], 0, sizeof(lut_stack[0]));
				lut_stack[new_lut][node_child[curr_node]] = alt_n;
			}
		}
		if (node_lutref[curr_node] > 0)
			lut_stack[node_lutref[curr_node]][in_byte] = new_node;
		/*
		 * Write pattern code to the output stream
		 */
		node_codes[new_node] = next_code;
		node_lutref[new_node] = 0;
		SLZW_ENC_WRITE(o, oi, acc, node_codes[curr_node], curr_code_len);
		if (next_code == (1 << curr_code_len))
			curr_code_len++;
		/*
		 * Reset tree if tree code limit is reached or if running
		 * out of LUTs
		 */
		if (++next_code == SLZW_MAX_CODE ||
		    lut_stack_in_use == SLZW_MAX_LUTS)
		        SLZW_ENC_RESET(node_codes, node_lutref,
				       lut_stack_in_use, node_stack_in_use,
				       next_code, curr_code_len);
	}
	/*
	 * Write last code, the "end of information" mark, and fill bits with 0
	 */
	SLZW_ENC_WRITE(o, oi, acc, node_codes[curr_node], curr_code_len);
	SLZW_ENC_WRITE(o, oi, acc, SLZW_STOP, curr_code_len);
	if (acc)
		o[++oi] = 0;
	return oi;
}

size_t sdec_lzw(const unsigned char *s, const size_t ss, unsigned char *o)
{
	RETURN_IF(!s || !o || !ss, 0);
	size_t i, j;
	size_t acc = 0, accbuf = 0, oi = 0;
	size_t last_code, curr_code_len, next_inc_code;
	size_t parents[SLZW_CODE_LIMIT];
	unsigned char xbyte[SLZW_CODE_LIMIT], pattern[SLZW_CODE_LIMIT], lastwc = 0;
	/*
	 * Initialize root node
	 */
	slzw_setseq256s8((unsigned *)xbyte);
	SLZW_DEC_RESET(j, curr_code_len, last_code, next_inc_code, parents);
	/*
	 * Code expand loop
	 */
	for (i = 0; i < ss;) {
		int new_code;
		SLZW_DEC_READ(new_code, s, ss, i, acc, accbuf, curr_code_len);
#ifdef SLZW_ENABLE_RLE
		/*
		 * Write RLE pattern
		 */
		if (new_code == SLZW_RLE) {
			size_t count, lre_byte, write_size;
			SLZW_DEC_READ(count, s, ss, i, acc, accbuf, curr_code_len);
			SLZW_DEC_READ(lre_byte, s, ss, i, acc, accbuf, curr_code_len);
			write_size = count * SLZW_RLE_CSIZE;
			memset(o + oi, lre_byte, write_size);
			oi += write_size;
			continue;
		}
#endif
		if (new_code == SLZW_RESET) {
			SLZW_DEC_RESET(j, curr_code_len, last_code, next_inc_code, parents);
			continue;
		}
		if (new_code == SLZW_STOP) {
			break;
		}
		if (last_code == SLZW_CODE_LIMIT) {
			o[oi++] = lastwc = xbyte[new_code];
			last_code = new_code;
			continue;
		}
		size_t code, pattern_off = SLZW_CODE_LIMIT - 1;
		if (new_code == next_inc_code) {
			pattern[pattern_off--] = lastwc;
			code = last_code;
		} else {
			code = new_code;
		}
		for (; code >= SLZW_FIRST;) {
			pattern[pattern_off--] = xbyte[code];
			code = parents[code];
		}
		pattern[pattern_off--] = lastwc = xbyte[next_inc_code] = xbyte[code];
		parents[next_inc_code] = last_code;
		if (next_inc_code < SLZW_CODE_LIMIT)
			next_inc_code++;
		if (next_inc_code == 1 << curr_code_len &&
		    next_inc_code < SLZW_CODE_LIMIT) {
			curr_code_len++;
		}
		last_code = new_code;
		/*
		 * Write LZW pattern
		 */
		size_t write_size = SLZW_CODE_LIMIT - 1 - pattern_off;
		memcpy(o + oi, pattern + pattern_off + 1, write_size);
		oi += write_size;
	}
	return oi;
}

#ifdef STANDALONE_TEST

#define BUF_IN_SIZE (220 * 1024 * 1024)
#define BUF_OUT_SIZE (BUF_IN_SIZE * 2)

static int syntax_error(const char **argv, const int exit_code)
{
	const char *v0 = argv[0];
	fprintf(stderr,
		"Error [%i] Syntax: %s [-eb|-db|-eh|-eH|-dh]\nExamples:\n"
		"%s -eb <in >out.b64\n%s -db <in.b64 >out\n"
		"%s -eh <in >out.hex\n%s -eH <in >out.HEX\n"
		"%s -dh <in.hex >out\n%s -dh <in.HEX >out\n"
		"%s -ez <in >in.z\n%s -dz <in.z >out\n",
		exit_code, v0, v0, v0, v0, v0, v0, v0, v0, v0);
	return exit_code;
}

/* TODO: buffered I/O instead of unbuffered */
int main(int argc, const char **argv)
{
	size_t lo = 0;
	char *buf0 = (char *)malloc(BUF_IN_SIZE + BUF_OUT_SIZE);
	char *buf = buf0;
	char *bufo = buf0 + BUF_IN_SIZE;
	int f_in = 0, f_out = 1;
	ssize_t l;
	if (argc < 2 || (l = posix_read(f_in, buf, BUF_IN_SIZE)) < 0)
		return syntax_error(argv, 1);
	const int mode = !strncmp(argv[1], "-eb", 3) ? 1 :
			 !strncmp(argv[1], "-db", 3) ? 2 :
			 !strncmp(argv[1], "-eh", 3) ? 3 :
			 !strncmp(argv[1], "-eH", 3) ? 4 :
			 !strncmp(argv[1], "-dh", 3) ? 5 :
			 !strncmp(argv[1], "-ez", 3) ? 6 :
			 !strncmp(argv[1], "-dz", 3) ? 7 : 0;
	if (!mode)
		return syntax_error(argv, 2);
	size_t i = 0, imax;
	switch (argv[1][3] == 'x') {
	case 'x': imax = 100000; break;
	case 'X': imax = 1000000; break;
	default:  imax = 1;
	}
	for (; i < imax; i++) {
		switch (mode) {
		case 1: lo = senc_b64(buf, (size_t)l, bufo); break;
		case 2: lo = sdec_b64(buf, (size_t)l, bufo); break;
		case 3: lo = senc_hex(buf, (size_t)l, bufo); break;
		case 4: lo = senc_HEX(buf, (size_t)l, bufo); break;
		case 5: lo = sdec_hex(buf, (size_t)l, bufo); break;
		case 6: lo = senc_lzw(buf, (size_t)l, bufo); break;
		case 7: lo = sdec_lzw(buf, (size_t)l, bufo); break;
		}
	}
	int r = posix_write(f_out, bufo, lo);
	fprintf(stderr, "in: %u * %u bytes, out: %u * %u bytes [write result: %u]\n",
		(unsigned)l, (unsigned)i, (unsigned)lo, (unsigned)i, (unsigned)r);
	return 0;
}
#endif

