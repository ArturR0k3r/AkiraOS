/**
 * @file akpkg.c
 * @brief .akpkg archive decompressor for AkiraOS
 *
 * Implements:
 *   - Gzip header parser (RFC 1952)
 *   - Raw DEFLATE decompressor (RFC 1951) — stored, fixed, and dynamic blocks
 *   - POSIX tar entry walker
 *
 * No external compression library is required; everything runs from SRAM/PSRAM.
 */

#include "akpkg.h"
#include "mem_helper.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(akpkg, CONFIG_AKIRA_LOG_LEVEL);

/*===========================================================================*/
/* DEFLATE decompressor (RFC 1951)                                           */
/*===========================================================================*/

/* Limits */
#define INF_MAX_BITS 15
#define INF_MAX_LSYMS 288 /* max literal/length symbols */
#define INF_MAX_DSYMS 32  /* max distance symbols       */
#define INF_MAX_CSYMS 19  /* code-length alphabet size  */

/* Canonical Huffman table */
typedef struct
{
    int max_bits;
    uint16_t counts[INF_MAX_BITS + 1]; /* codes per bit length */
    uint16_t first[INF_MAX_BITS + 1];  /* first canonical code per length */
    uint16_t offset[INF_MAX_BITS + 1]; /* index into syms[] per length    */
    uint16_t syms[INF_MAX_LSYMS];      /* symbols sorted by length/value  */
} huff_t;

/* Decompressor state */
typedef struct
{
    /* input */
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint32_t bit_buf;
    int bit_cnt;
    /* output */
    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
    /* error code — set on any decode error */
    int err;
} inf_t;

/* -------------------------------------------------------------------------
 * Bit reader — DEFLATE stores integers LSB-first.
 * -------------------------------------------------------------------------*/
static inline void inf_refill(inf_t *s)
{
    while (s->bit_cnt <= 24 && s->in_pos < s->in_len)
    {
        s->bit_buf |= (uint32_t)s->in[s->in_pos++] << s->bit_cnt;
        s->bit_cnt += 8;
    }
}

static inline uint32_t inf_bits(inf_t *s, int n)
{
    if (n == 0)
    {
        return 0;
    }
    inf_refill(s);
    if (s->bit_cnt < n)
    {
        s->err = -1;
        return 0;
    }
    uint32_t val = s->bit_buf & ((1u << n) - 1u);
    s->bit_buf >>= n;
    s->bit_cnt -= n;
    return val;
}

/* Discard bits to reach the next byte boundary (used by stored blocks). */
static inline void inf_align(inf_t *s)
{
    int excess = s->bit_cnt & 7;
    if (excess)
    {
        s->bit_buf >>= excess;
        s->bit_cnt -= excess;
    }
}

/* Emit one byte to the output window. */
static inline int inf_emit(inf_t *s, uint8_t byte)
{
    if (s->out_pos >= s->out_cap)
    {
        s->err = -2;
        return -1;
    }
    s->out[s->out_pos++] = byte;
    return 0;
}

/* -------------------------------------------------------------------------
 * Canonical Huffman table builder.
 *
 * Given an array of code lengths (0 = unused symbol), builds the table
 * needed for canonical Huffman decoding.
 * -------------------------------------------------------------------------*/
static int huff_build(huff_t *h, const uint8_t *lens, int n)
{
    memset(h->counts, 0, sizeof(h->counts));
    h->max_bits = 0;

    for (int i = 0; i < n; i++)
    {
        if (lens[i] > INF_MAX_BITS)
        {
            return -1;
        }
        if (lens[i])
        {
            h->counts[lens[i]]++;
            if (lens[i] > h->max_bits)
            {
                h->max_bits = lens[i];
            }
        }
    }

    /* Compute first canonical code for each bit length. */
    uint16_t code = 0;
    for (int b = 1; b <= h->max_bits; b++)
    {
        h->first[b] = code;
        code = (uint16_t)((code + h->counts[b]) << 1);
    }

    /* Compute per-length offset into syms[]. */
    {
        uint16_t off = 0;
        for (int b = 1; b <= h->max_bits; b++)
        {
            h->offset[b] = off;
            off += h->counts[b];
        }
    }

    /* Place symbols sorted by code length then symbol value. */
    uint16_t tmp[INF_MAX_BITS + 1];
    memcpy(tmp, h->offset, sizeof(tmp));
    for (int i = 0; i < n; i++)
    {
        if (lens[i])
        {
            h->syms[tmp[lens[i]]++] = (uint16_t)i;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Huffman decoder.
 *
 * DEFLATE Huffman codes are transmitted with the MSB first within the
 * LSB-first bit stream.  We shift each new bit into the MSB position of
 * our running code word and compare against the canonical code ranges.
 * -------------------------------------------------------------------------*/
static int huff_decode(inf_t *s, const huff_t *h)
{
    uint32_t code = 0;

    for (int b = 1; b <= h->max_bits; b++)
    {
        code = (code << 1) | inf_bits(s, 1);
        if (s->err)
        {
            return -1;
        }
        if (h->counts[b] &&
            code >= (uint32_t)h->first[b] &&
            code < (uint32_t)(h->first[b] + h->counts[b]))
        {
            return h->syms[h->offset[b] + (code - h->first[b])];
        }
    }

    s->err = -3;
    return -1;
}

/* -------------------------------------------------------------------------
 * RFC 1951 length/distance tables.
 * -------------------------------------------------------------------------*/
static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10,
    11, 13, 15, 17,
    19, 23, 27, 31,
    35, 43, 51, 59,
    67, 83, 99, 115,
    131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
    3, 3, 3, 3,
    4, 4, 4, 4,
    5, 5, 5, 5, 0};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4,
    5, 7, 9, 13,
    17, 25, 33, 49,
    65, 97, 129, 193,
    257, 385, 513, 769,
    1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289,
    16385, 24577};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0,
    1, 1, 2, 2,
    3, 3, 4, 4,
    5, 5, 6, 6,
    7, 7, 8, 8,
    9, 9, 10, 10,
    11, 11, 12, 12,
    13, 13};

/* -------------------------------------------------------------------------
 * Decode literal/length + distance pairs until end-of-block (symbol 256).
 * -------------------------------------------------------------------------*/
static int inf_decode_codes(inf_t *s, const huff_t *ll, const huff_t *dist)
{
    for (;;)
    {
        int sym = huff_decode(s, ll);
        if (s->err || sym < 0)
        {
            return -1;
        }

        if (sym < 256)
        {
            /* Literal byte */
            if (inf_emit(s, (uint8_t)sym))
            {
                return -1;
            }
        }
        else if (sym == 256)
        {
            /* End of block */
            break;
        }
        else
        {
            /* Length/distance back-reference */
            int lidx = sym - 257;
            if (lidx >= 29)
            {
                s->err = -4;
                return -1;
            }
            uint32_t length = k_len_base[lidx] + inf_bits(s, k_len_extra[lidx]);
            if (s->err)
            {
                return -1;
            }

            int dsym = huff_decode(s, dist);
            if (s->err || dsym < 0)
            {
                return -1;
            }
            if (dsym >= 30)
            {
                s->err = -5;
                return -1;
            }
            uint32_t distance = k_dist_base[dsym] + inf_bits(s, k_dist_extra[dsym]);
            if (s->err)
            {
                return -1;
            }
            if (distance > s->out_pos)
            {
                s->err = -6;
                return -1;
            }

            /* Copy with wrap-around (handles overlapping src/dst). */
            size_t src = s->out_pos - distance;
            for (uint32_t i = 0; i < length; i++)
            {
                if (inf_emit(s, s->out[src + (i % distance)]))
                {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Block type 00: stored (uncompressed).
 * -------------------------------------------------------------------------*/
static int inf_block_stored(inf_t *s)
{
    inf_align(s);

    /* inf_refill pre-fetches whole bytes into bit_buf and advances in_pos.
     * After bit-aligning, any complete bytes still in bit_buf represent bytes
     * that in_pos has already been advanced past.  Rewind in_pos so that
     * direct byte reads below start at the correct stream position. */
    s->in_pos -= (size_t)(s->bit_cnt >> 3);
    s->bit_buf = 0;
    s->bit_cnt = 0;

    if (s->in_pos + 4 > s->in_len)
    {
        s->err = -7;
        return -1;
    }
    uint16_t len = (uint16_t)(s->in[s->in_pos] | ((uint16_t)s->in[s->in_pos + 1] << 8));
    uint16_t nlen = (uint16_t)(s->in[s->in_pos + 2] | ((uint16_t)s->in[s->in_pos + 3] << 8));
    s->in_pos += 4;

    if ((uint16_t)(len ^ nlen) != 0xFFFFu)
    {
        s->err = -8;
        return -1;
    }
    if (s->in_pos + len > s->in_len)
    {
        s->err = -9;
        return -1;
    }
    for (uint16_t i = 0; i < len; i++)
    {
        if (inf_emit(s, s->in[s->in_pos++]))
        {
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Block type 01: fixed Huffman codes.
 * -------------------------------------------------------------------------*/
static int inf_block_fixed(inf_t *s)
{
    uint8_t ll_lens[INF_MAX_LSYMS];
    for (int i = 0; i <= 143; i++)
        ll_lens[i] = 8;
    for (int i = 144; i <= 255; i++)
        ll_lens[i] = 9;
    for (int i = 256; i <= 279; i++)
        ll_lens[i] = 7;
    for (int i = 280; i <= 287; i++)
        ll_lens[i] = 8;

    uint8_t d_lens[INF_MAX_DSYMS];
    for (int i = 0; i < 30; i++)
        d_lens[i] = 5;
    d_lens[30] = d_lens[31] = 0;

    huff_t *ll = akira_malloc_buffer(sizeof(huff_t));
    huff_t *dist = akira_malloc_buffer(sizeof(huff_t));
    if (!ll || !dist)
    {
        akira_free_buffer(ll);
        akira_free_buffer(dist);
        s->err = -10;
        return -1;
    }
    if (huff_build(ll, ll_lens, INF_MAX_LSYMS) ||
        huff_build(dist, d_lens, INF_MAX_DSYMS))
    {
        akira_free_buffer(ll);
        akira_free_buffer(dist);
        s->err = -10;
        return -1;
    }
    int ret = inf_decode_codes(s, ll, dist);
    akira_free_buffer(ll);
    akira_free_buffer(dist);
    return ret;
}

/* -------------------------------------------------------------------------
 * Block type 10: dynamic Huffman codes.
 * -------------------------------------------------------------------------*/
static int inf_block_dynamic(inf_t *s)
{
    int hlit = (int)inf_bits(s, 5) + 257;
    int hdist = (int)inf_bits(s, 5) + 1;
    int hclen = (int)inf_bits(s, 4) + 4;
    if (s->err)
    {
        return -1;
    }
    if (hlit > INF_MAX_LSYMS || hdist > INF_MAX_DSYMS)
    {
        s->err = -11;
        return -1;
    }

    /* Order in which HCLEN code lengths are transmitted. */
    static const uint8_t clen_order[INF_MAX_CSYMS] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
        11, 4, 12, 3, 13, 2, 14, 1, 15};

    uint8_t clen_lens[INF_MAX_CSYMS] = {0};
    for (int i = 0; i < hclen; i++)
    {
        clen_lens[clen_order[i]] = (uint8_t)inf_bits(s, 3);
    }
    if (s->err)
    {
        return -1;
    }

    huff_t *clen_tab = akira_malloc_buffer(sizeof(huff_t));
    if (!clen_tab)
    {
        s->err = -12;
        return -1;
    }
    if (huff_build(clen_tab, clen_lens, INF_MAX_CSYMS))
    {
        akira_free_buffer(clen_tab);
        s->err = -12;
        return -1;
    }

    /* Decode all literal/length and distance code lengths. */
    uint8_t *lens = akira_malloc_buffer(INF_MAX_LSYMS + INF_MAX_DSYMS);
    if (!lens)
    {
        akira_free_buffer(clen_tab);
        s->err = -12;
        return -1;
    }
    memset(lens, 0, INF_MAX_LSYMS + INF_MAX_DSYMS);
    int total = hlit + hdist;
    int i = 0;

    while (i < total)
    {
        int sym = huff_decode(s, clen_tab);
        if (s->err || sym < 0)
        {
            akira_free_buffer(clen_tab);
            akira_free_buffer(lens);
            return -1;
        }
        if (sym < 16)
        {
            lens[i++] = (uint8_t)sym;
        }
        else if (sym == 16)
        {
            if (i == 0)
            {
                akira_free_buffer(clen_tab);
                akira_free_buffer(lens);
                s->err = -13;
                return -1;
            }
            uint8_t prev = lens[i - 1];
            int rep = (int)inf_bits(s, 2) + 3;
            if (s->err)
            {
                akira_free_buffer(clen_tab);
                akira_free_buffer(lens);
                return -1;
            }
            while (rep-- && i < total)
                lens[i++] = prev;
        }
        else if (sym == 17)
        {
            int rep = (int)inf_bits(s, 3) + 3;
            if (s->err)
            {
                akira_free_buffer(clen_tab);
                akira_free_buffer(lens);
                return -1;
            }
            while (rep-- && i < total)
                lens[i++] = 0;
        }
        else
        { /* sym == 18 */
            int rep = (int)inf_bits(s, 7) + 11;
            if (s->err)
            {
                akira_free_buffer(clen_tab);
                akira_free_buffer(lens);
                return -1;
            }
            while (rep-- && i < total)
                lens[i++] = 0;
        }
    }
    akira_free_buffer(clen_tab);
    if (s->err)
    {
        akira_free_buffer(lens);
        return -1;
    }

    huff_t *ll = akira_malloc_buffer(sizeof(huff_t));
    huff_t *dist = akira_malloc_buffer(sizeof(huff_t));
    if (!ll || !dist)
    {
        akira_free_buffer(lens);
        akira_free_buffer(ll);
        akira_free_buffer(dist);
        s->err = -14;
        return -1;
    }
    if (huff_build(ll, lens, hlit) ||
        huff_build(dist, lens + hlit, hdist))
    {
        akira_free_buffer(lens);
        akira_free_buffer(ll);
        akira_free_buffer(dist);
        s->err = -14;
        return -1;
    }
    akira_free_buffer(lens);
    int ret = inf_decode_codes(s, ll, dist);
    akira_free_buffer(ll);
    akira_free_buffer(dist);
    return ret;
}

/* -------------------------------------------------------------------------
 * Top-level inflate: process all DEFLATE blocks.
 * -------------------------------------------------------------------------*/
static int inflate_raw(inf_t *s)
{
    int bfinal;
    do
    {
        bfinal = (int)inf_bits(s, 1);
        int btype = (int)inf_bits(s, 2);
        if (s->err)
        {
            return -1;
        }

        int ret;
        switch (btype)
        {
        case 0:
            ret = inf_block_stored(s);
            break;
        case 1:
            ret = inf_block_fixed(s);
            break;
        case 2:
            ret = inf_block_dynamic(s);
            break;
        default:
            s->err = -15;
            return -1;
        }
        if (ret)
        {
            return ret;
        }
    } while (!bfinal);

    return 0;
}

/*===========================================================================*/
/* Gzip header parser (RFC 1952)                                             */
/*===========================================================================*/

#define GZIP_ID1 0x1fu
#define GZIP_ID2 0x8bu
#define GZIP_METHOD 0x08u
#define GZIP_FHCRC 0x02u
#define GZIP_FEXTRA 0x04u
#define GZIP_FNAME 0x08u
#define GZIP_FCOMMENT 0x10u

/**
 * Skip the variable-length gzip header and return the byte offset of the
 * DEFLATE payload.  Also reads the ISIZE field (last 4 bytes of the stream).
 */
static int gzip_parse(const uint8_t *data, size_t len,
                      size_t *deflate_start, size_t *isize_out)
{
    if (len < 18)
    { /* 10-byte fixed header + ≥1 byte deflate + 8-byte trailer */
        return -EINVAL;
    }
    if (data[0] != GZIP_ID1 || data[1] != GZIP_ID2)
    {
        return -EINVAL;
    }
    if (data[2] != GZIP_METHOD)
    {
        return -ENOTSUP;
    }

    uint8_t flg = data[3];
    size_t pos = 10; /* fixed header length */

    if (flg & GZIP_FEXTRA)
    {
        if (pos + 2 > len)
            return -EINVAL;
        uint16_t xlen = (uint16_t)(data[pos] | ((uint16_t)data[pos + 1] << 8));
        pos += 2u + xlen;
    }
    if (flg & GZIP_FNAME)
    {
        while (pos < len && data[pos])
            pos++;
        if (pos >= len)
            return -EINVAL;
        pos++;
    }
    if (flg & GZIP_FCOMMENT)
    {
        while (pos < len && data[pos])
            pos++;
        if (pos >= len)
            return -EINVAL;
        pos++;
    }
    if (flg & GZIP_FHCRC)
    {
        pos += 2;
    }

    /* Need at least 8 bytes of trailer (CRC32 + ISIZE) after the header. */
    if (pos + 8 > len)
    {
        return -EINVAL;
    }

    *deflate_start = pos;

    /* ISIZE: bytes [len-4 .. len-1], little-endian. */
    *isize_out = (size_t)data[len - 4] | ((size_t)data[len - 3] << 8) | ((size_t)data[len - 2] << 16) | ((size_t)data[len - 1] << 24);

    return 0;
}

/*===========================================================================*/
/* Tar walker                                                                */
/*===========================================================================*/

#define TAR_BLOCK_SZ 512u

/* Parse an octal ASCII field of up to @p field_len characters. */
static size_t tar_octal(const uint8_t *field, int field_len)
{
    size_t v = 0;
    for (int i = 0; i < field_len; i++)
    {
        uint8_t c = field[i];
        if (c < '0' || c > '7')
            break;
        v = v * 8u + (size_t)(c - '0');
    }
    return v;
}

/*===========================================================================*/
/* Public API                                                                */
/*===========================================================================*/

bool akpkg_is_gzip(const uint8_t *data, size_t len)
{
    return len >= 2 && data[0] == GZIP_ID1 && data[1] == GZIP_ID2;
}

int32_t akpkg_inflate(const uint8_t *gz, size_t gz_len,
                      uint8_t *out, size_t out_cap)
{
    size_t deflate_start, isize;
    int ret = gzip_parse(gz, gz_len, &deflate_start, &isize);
    if (ret)
    {
        LOG_ERR("akpkg: invalid gzip header (%d)", ret);
        return ret;
    }

    if (isize == 0 || isize > out_cap)
    {
        LOG_ERR("akpkg: ISIZE %zu exceeds output buffer %zu", isize, out_cap);
        return -ENOMEM;
    }

    LOG_DBG("akpkg: ISIZE=%zu deflate_start=%zu in_len=%zu",
            isize, deflate_start, gz_len - deflate_start - 8u);

    /* Deflate payload is everything between the header and the 8-byte trailer. */
    inf_t s = {
        .in = gz + deflate_start,
        .in_len = gz_len - deflate_start - 8u,
        .in_pos = 0,
        .bit_buf = 0,
        .bit_cnt = 0,
        .out = out,
        .out_cap = isize,
        .out_pos = 0,
        .err = 0,
    };

    if (inflate_raw(&s) || s.err)
    {
        LOG_ERR("akpkg: inflate failed (err=%d, wrote=%zu/%zu)",
                s.err, s.out_pos, isize);
        return -EIO;
    }

    return (ssize_t)s.out_pos;
}

size_t akpkg_base64_decode(const char *src, size_t src_len,
                           uint8_t *out, size_t out_cap)
{
    static const int8_t k_dtab[256] = {
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1, /* 0x00 */
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1, /* 0x10 */
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        62,
        -1,
        -1,
        -1,
        63, /* 0x20  +  / */
        52,
        53,
        54,
        55,
        56,
        57,
        58,
        59,
        60,
        61,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1, /* 0-9 */
        -1,
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14, /* A-O */
        15,
        16,
        17,
        18,
        19,
        20,
        21,
        22,
        23,
        24,
        25,
        -1,
        -1,
        -1,
        -1,
        -1, /* P-Z */
        -1,
        26,
        27,
        28,
        29,
        30,
        31,
        32,
        33,
        34,
        35,
        36,
        37,
        38,
        39,
        40, /* a-o */
        41,
        42,
        43,
        44,
        45,
        46,
        47,
        48,
        49,
        50,
        51,
        -1,
        -1,
        -1,
        -1,
        -1, /* p-z */
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
        -1,
    };

    size_t out_pos = 0;
    uint32_t acc = 0;
    int acc_bits = 0;

    for (size_t i = 0; i < src_len; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ')
            continue;
        int v = k_dtab[c];
        if (v < 0)
            break; /* non-alphabet character — stop */
        acc = (acc << 6) | (uint32_t)v;
        acc_bits += 6;
        if (acc_bits >= 8)
        {
            acc_bits -= 8;
            if (out_pos >= out_cap)
                return 0; /* overflow */
            out[out_pos++] = (uint8_t)((acc >> acc_bits) & 0xFFu);
        }
    }
    return out_pos;
}

int akpkg_tar_extract(const uint8_t *tar, size_t tar_len,
                      const uint8_t **wasm_ptr, size_t *wasm_size,
                      const char **manifest_ptr, size_t *manifest_size,
                      const uint8_t **model_ptr, size_t *model_size)
{
    *wasm_ptr = NULL;
    *wasm_size = 0;
    *manifest_ptr = NULL;
    *manifest_size = 0;
    if (model_ptr)  *model_ptr  = NULL;
    if (model_size) *model_size = 0;

    size_t pos = 0;

    while (pos + TAR_BLOCK_SZ <= tar_len)
    {
        /* Two consecutive null-filled 512-byte blocks mark end-of-archive. */
        bool all_zero = true;
        for (size_t k = 0; k < TAR_BLOCK_SZ && all_zero; k++)
        {
            if (tar[pos + k])
                all_zero = false;
        }
        if (all_zero)
            break;

        const char *name = (const char *)(tar + pos);
        size_t fsize = tar_octal(tar + pos + 124, 12);
        char ttype = (char)tar[pos + 156];

        pos += TAR_BLOCK_SZ; /* advance past the 512-byte header */

        if (fsize > tar_len || pos > tar_len - fsize)
        {
            /* Truncated or malformed archive — stop gracefully. */
            break;
        }

        if (ttype == '0' || ttype == '\0')
        {
            /* Regular file — check filename (path component stripped). */
            const char *base = strrchr(name, '/');
            base = base ? base + 1 : name;

            if (strcmp(base, "app.wasm") == 0 || strcmp(base, "app.aot") == 0)
            {
                *wasm_ptr = tar + pos;
                *wasm_size = fsize;
            }
            else if (strcmp(base, "manifest.json") == 0)
            {
                *manifest_ptr = (const char *)(tar + pos);
                *manifest_size = fsize;
            }
            else if (strcmp(base, "model.tflite") == 0 && model_ptr && model_size)
            {
                *model_ptr  = tar + pos;
                *model_size = fsize;
            }
        }

        /* Advance past the file data, rounded up to the next tar block. */
        pos += (fsize + TAR_BLOCK_SZ - 1u) & ~(TAR_BLOCK_SZ - 1u);
    }

    if (!(*wasm_ptr) || !(*manifest_ptr))
    {
        LOG_ERR("akpkg: missing %s%s in archive",
                (*wasm_ptr) ? "" : "app.wasm/app.aot ",
                (*manifest_ptr) ? "" : "manifest.json");
        return -ENOENT;
    }

    if (model_ptr && *model_ptr)
        LOG_DBG("akpkg: model.tflite found (%zu B)", *model_size);

    return 0;
}
