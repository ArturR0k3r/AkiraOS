/**
 * @file se050.c
 * @brief NXP EdgeLock SE050 secure element — SE05x APDU applet layer + driver.
 *
 * See se050.h for the API and scope caveats. The SE05x command set is TLV
 * based: each APDU is [CLA INS P1 P2 Lc <TLVs> Le] and the response is
 * [<TLVs> SW1 SW2]. We implement exactly the commands AkiraConsole uses.
 */

#define DT_DRV_COMPAT nxp_se050

#include "se050.h"
#include "se050_transport.h"
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#endif

LOG_MODULE_REGISTER(se050, CONFIG_I2C_LOG_LEVEL);

/* =========================================================================
 * SE05x APDU constants (track the public SE05x APDU specification).
 * Centralised so they can be confirmed against the datasheet in one place.
 * ========================================================================= */
#define CLA_PROP            0x80   /* proprietary class */

#define INS_WRITE           0x01
#define INS_READ            0x02
#define INS_CRYPTO          0x03
#define INS_MGMT            0x04

/* P1 key-type (bits 6:5) and credential-type (bits 4:0) */
#define P1_DEFAULT          0x00
#define P1_KEY_PAIR         0x60
#define P1_EC               0x01   /* credential type: EC */
#define P1_BINARY           0x06   /* credential type: binary file */
#define P1_SIGNATURE        0x0C   /* credential type: signature (ECDSA sign) */

/* P2 operation */
#define P2_DEFAULT          0x00
#define P2_GENERATE         0x03
#define P2_EXIST            0x27
#define P2_SIGN             0x09
#define P2_RANDOM           0x49

/* TLV tags */
#define TAG_1               0x41   /* object id / primary operand */
#define TAG_2               0x42   /* offset / curve id / algo */
#define TAG_3               0x43   /* length / input data */
#define TAG_4               0x44   /* data */
#define TAG_5               0x45

#define ECCURVE_NIST_P256   0x03
#define ECSIG_SHA256        0x21   /* ECDSA with SHA-256 */

#define SW_OK               0x9000

/* Bounded scratch for one command/response (short APDUs only). */
#define APDU_MAX            (SE050_MAX_INF)

struct se050_config {
    struct i2c_dt_spec  i2c;
    struct gpio_dt_spec ena;
};

struct se050_data {
    struct se050_transport link;
    struct k_mutex         lock;
    bool                   ready;
};

/* =========================================================================
 * TLV helpers (short form: length < 0x80; SE05x uses 1/2/3-byte length —
 * our payloads are <128 B so 1-byte length is always valid here).
 * ========================================================================= */
static int tlv_put(uint8_t *buf, size_t cap, size_t *off,
                   uint8_t tag, const uint8_t *val, size_t len)
{
    if (len >= 0x80 || *off + 2 + len > cap) {
        return -ENOSPC;
    }
    buf[(*off)++] = tag;
    buf[(*off)++] = (uint8_t)len;
    if (len) {
        memcpy(&buf[*off], val, len);
        *off += len;
    }
    return 0;
}

/* Find TLV @tag in @buf; return pointer to its value and set *vlen.
 * Handles BER-TLV length forms used by the SE05x: short (L < 0x80), and long
 * form 0x81 (1 length byte) / 0x82 (2 length bytes). The SE050 emits long form
 * even for small lengths (e.g. GetRandom returns "41 82 00 10 ..."). */
static const uint8_t *tlv_find(const uint8_t *buf, size_t len,
                               uint8_t tag, size_t *vlen)
{
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = buf[i];
        uint8_t l0 = buf[i + 1];
        size_t hdr;               /* tag + length bytes */
        size_t vl;                /* value length */

        if (l0 < 0x80) {          /* short form */
            hdr = 2;
            vl  = l0;
        } else if (l0 == 0x81) {  /* 1-byte length */
            if (i + 3 > len) break;
            hdr = 3;
            vl  = buf[i + 2];
        } else if (l0 == 0x82) {  /* 2-byte length */
            if (i + 4 > len) break;
            hdr = 4;
            vl  = ((size_t)buf[i + 2] << 8) | buf[i + 3];
        } else {
            break;                /* 0x83+ not used by SE05x here */
        }

        if (i + hdr + vl > len) {
            break;
        }
        if (t == tag) {
            *vlen = vl;
            return &buf[i + hdr];
        }
        i += hdr + vl;
    }
    return NULL;
}

static void put_u32(uint8_t out[4], uint32_t v)
{
    out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v;
}

/* =========================================================================
 * APDU transceive: frame [CLA INS P1 P2 Lc data Le], strip SW, return payload.
 * ========================================================================= */
static int se05x_apdu(struct se050_data *d, uint8_t ins, uint8_t p1, uint8_t p2,
                      const uint8_t *data, size_t dlen,
                      uint8_t *resp, size_t resp_cap, size_t *resp_len,
                      uint16_t *sw)
{
    uint8_t cmd[4 + 1 + APDU_MAX + 1];
    size_t n = 0;

    if (dlen > APDU_MAX) {
        return -EINVAL;
    }
    cmd[n++] = CLA_PROP;
    cmd[n++] = ins;
    cmd[n++] = p1;
    cmd[n++] = p2;
    cmd[n++] = (uint8_t)dlen;          /* Lc (short APDU) */
    if (dlen) {
        memcpy(&cmd[n], data, dlen);
        n += dlen;
    }
    cmd[n++] = 0x00;                   /* Le — expect a response */

    uint8_t rbuf[SE050_MAX_INF];
    size_t rlen = 0;
    int ret = se050_transport_apdu(&d->link, cmd, n, rbuf, sizeof(rbuf), &rlen);
    if (ret < 0) {
        return ret;
    }
    if (rlen < 2) {
        return -EPROTO;
    }

    uint16_t status = ((uint16_t)rbuf[rlen - 2] << 8) | rbuf[rlen - 1];
    if (sw) {
        *sw = status;
    }
    size_t payload = rlen - 2;
    if (resp) {
        if (payload > resp_cap) {
            return -ENOSPC;
        }
        memcpy(resp, rbuf, payload);
    }
    if (resp_len) {
        *resp_len = payload;
    }
    return 0;
}

/* =========================================================================
 * Applet SELECT — the SE050 IoT applet must be selected (ISO7816 SELECT by
 * AID, CLA 0x00) before any SE05x command; otherwise the card manager answers
 * every INS with SW=0x6D00 ("instruction not supported"). The SELECT response
 * carries the applet version info (ignored here).
 * ========================================================================= */
static const uint8_t SE050_APPLET_AID[] = {
    0xA0, 0x00, 0x00, 0x03, 0x96, 0x54, 0x53,
    0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00
};

static int se050_select_applet(struct se050_data *d)
{
    uint8_t cmd[5 + sizeof(SE050_APPLET_AID) + 1];
    size_t n = 0;

    cmd[n++] = 0x00;                       /* CLA (ISO7816, not proprietary) */
    cmd[n++] = 0xA4;                       /* INS = SELECT */
    cmd[n++] = 0x04;                       /* P1  = select by DF name (AID) */
    cmd[n++] = 0x00;                       /* P2  = first/only occurrence */
    cmd[n++] = (uint8_t)sizeof(SE050_APPLET_AID);   /* Lc */
    memcpy(&cmd[n], SE050_APPLET_AID, sizeof(SE050_APPLET_AID));
    n += sizeof(SE050_APPLET_AID);
    cmd[n++] = 0x00;                       /* Le */

    uint8_t rbuf[SE050_MAX_INF];
    size_t rlen = 0;
    int ret = se050_transport_apdu(&d->link, cmd, n, rbuf, sizeof(rbuf), &rlen);
    if (ret < 0) {
        return ret;
    }
    if (rlen < 2) {
        return -EPROTO;
    }
    uint16_t sw = ((uint16_t)rbuf[rlen - 2] << 8) | rbuf[rlen - 1];
    if (sw != SW_OK) {
        LOG_ERR("SE050 SELECT applet SW=0x%04X", sw);
        return -EIO;
    }
    return 0;
}

/* Reset the link (best-effort) and select the applet. Marks the part ready.
 * Caller must hold d->lock (or be single-threaded at init). */
static int se050_bringup(struct se050_data *d)
{
    /* Soft reset is best-effort: some SE050 revisions answer APDUs without a
     * successful T=1' interface reset, so a reset failure is not fatal. */
    (void)se050_transport_reset(&d->link);

    int ret = se050_select_applet(d);
    d->ready = (ret == 0);
    return ret;
}

/* =========================================================================
 * Public API
 * ========================================================================= */
const struct device *se050_get_device(void)
{
    const struct device *dev = DEVICE_DT_GET_ANY(nxp_se050);
    return (dev && device_is_ready(dev)) ? dev : NULL;
}

int se050_power_on(const struct device *dev)
{
    struct se050_data *d = dev->data;
    k_mutex_lock(&d->lock, K_FOREVER);
    int ret = se050_transport_power_on(&d->link);
    if (ret == 0) {
        ret = se050_bringup(d);
    }
    k_mutex_unlock(&d->lock);
    return ret;
}

int se050_power_off(const struct device *dev)
{
    struct se050_data *d = dev->data;
    k_mutex_lock(&d->lock, K_FOREVER);
    int ret = se050_transport_power_off(&d->link);
    k_mutex_unlock(&d->lock);
    return ret;
}

int se050_get_random(const struct device *dev, uint8_t *buf, size_t len)
{
    struct se050_data *d = dev->data;
    if (len == 0 || len > SE050_MAX_INF - 4) {
        return -EINVAL;
    }

    uint8_t tlv[8];
    size_t off = 0;
    uint8_t sz[2] = { len >> 8, len & 0xFF };
    int ret = tlv_put(tlv, sizeof(tlv), &off, TAG_1, sz, sizeof(sz));
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint8_t resp[SE050_MAX_INF];
    size_t rlen = 0;
    uint16_t sw = 0;
    ret = se05x_apdu(d, INS_MGMT, P1_DEFAULT, P2_RANDOM, tlv, off,
                     resp, sizeof(resp), &rlen, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        return ret;
    }
    if (sw != SW_OK) {
        LOG_ERR("GetRandom SW=0x%04X", sw);
        return -EIO;
    }

    size_t vlen = 0;
    const uint8_t *rnd = tlv_find(resp, rlen, TAG_1, &vlen);
    if (!rnd || vlen != len) {
        return -EPROTO;
    }
    memcpy(buf, rnd, len);
    return 0;
}

int se050_write_binary(const struct device *dev, uint32_t objid,
                       const uint8_t *data, size_t len)
{
    struct se050_data *d = dev->data;
    if (len == 0 || len > APDU_MAX - 16) {
        return -EINVAL;
    }

    uint8_t tlv[APDU_MAX];
    size_t off = 0;
    uint8_t id[4];   put_u32(id, objid);
    uint8_t offset[2] = { 0, 0 };
    uint8_t flen[2]   = { len >> 8, len & 0xFF };

    /* SE05x WriteBinary: TAG_1=objid, TAG_2=offset, TAG_3=file length (on
     * create), TAG_4=data. */
    int ret = tlv_put(tlv, sizeof(tlv), &off, TAG_1, id, sizeof(id));
    if (ret == 0) ret = tlv_put(tlv, sizeof(tlv), &off, TAG_2, offset, sizeof(offset));
    if (ret == 0) ret = tlv_put(tlv, sizeof(tlv), &off, TAG_3, flen, sizeof(flen));
    if (ret == 0) ret = tlv_put(tlv, sizeof(tlv), &off, TAG_4, data, len);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint16_t sw = 0;
    ret = se05x_apdu(d, INS_WRITE, P1_BINARY, P2_DEFAULT, tlv, off,
                     NULL, 0, NULL, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        return ret;
    }
    if (sw != SW_OK) {
        LOG_ERR("WriteBinary SW=0x%04X", sw);
        return -EIO;
    }
    return 0;
}

bool se050_object_exists(const struct device *dev, uint32_t objid)
{
    struct se050_data *d = dev->data;
    uint8_t id[4]; put_u32(id, objid);
    uint8_t tlv[8]; size_t off = 0;
    if (tlv_put(tlv, sizeof(tlv), &off, TAG_1, id, sizeof(id)) < 0) {
        return false;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint8_t resp[8]; size_t rlen = 0; uint16_t sw = 0;
    int ret = se05x_apdu(d, INS_MGMT, P1_DEFAULT, P2_EXIST, tlv, off,
                         resp, sizeof(resp), &rlen, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0 || sw != SW_OK || rlen < 1) {
        return false;
    }
    /* SE05x returns a Result TLV: 0x01 = exists. */
    size_t vlen = 0;
    const uint8_t *r = tlv_find(resp, rlen, TAG_1, &vlen);
    return r && vlen >= 1 && r[0] == 0x01;
}

int se050_read_binary(const struct device *dev, uint32_t objid,
                      uint8_t *buf, size_t cap, size_t *out_len)
{
    struct se050_data *d = dev->data;
    if (!se050_object_exists(dev, objid)) {
        return -ENOENT;
    }

    uint8_t id[4]; put_u32(id, objid);
    uint8_t tlv[8]; size_t off = 0;
    int ret = tlv_put(tlv, sizeof(tlv), &off, TAG_1, id, sizeof(id));
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint8_t resp[SE050_MAX_INF]; size_t rlen = 0; uint16_t sw = 0;
    ret = se05x_apdu(d, INS_READ, P1_DEFAULT, P2_DEFAULT, tlv, off,
                     resp, sizeof(resp), &rlen, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        return ret;
    }
    if (sw != SW_OK) {
        return -EIO;
    }

    size_t vlen = 0;
    const uint8_t *v = tlv_find(resp, rlen, TAG_1, &vlen);
    if (!v) {
        return -EPROTO;
    }
    if (vlen > cap) {
        return -ENOSPC;
    }
    memcpy(buf, v, vlen);
    if (out_len) {
        *out_len = vlen;
    }
    return 0;
}

int se050_ecc_gen_key(const struct device *dev, uint32_t objid)
{
    struct se050_data *d = dev->data;
    if (se050_object_exists(dev, objid)) {
        return 0;   /* already provisioned */
    }

    uint8_t id[4]; put_u32(id, objid);
    uint8_t curve = ECCURVE_NIST_P256;
    uint8_t tlv[16]; size_t off = 0;
    int ret = tlv_put(tlv, sizeof(tlv), &off, TAG_1, id, sizeof(id));
    if (ret == 0) ret = tlv_put(tlv, sizeof(tlv), &off, TAG_2, &curve, 1);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint16_t sw = 0;
    /* WriteECKey to generate: P1 = KEY_PAIR | EC (key-part = pair), P2 = DEFAULT,
     * with only TAG_1=objid and TAG_2=curve (no private/public key value) ->
     * on-die key-pair generation. Verified on hardware: P1=0x61, P2=0x00 -> 0x9000
     * (P1=0x01 -> 0x6A86, P2=0x03 -> 0x6B00). */
    ret = se05x_apdu(d, INS_WRITE, (uint8_t)(P1_KEY_PAIR | P1_EC), P2_DEFAULT,
                     tlv, off, NULL, 0, NULL, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        return ret;
    }
    if (sw != SW_OK) {
        LOG_ERR("EC keygen SW=0x%04X", sw);
        return -EIO;
    }
    return 0;
}

int se050_ecc_get_pub(const struct device *dev, uint32_t objid,
                     uint8_t pub[SE050_P256_PUB_LEN])
{
    struct se050_data *d = dev->data;
    uint8_t id[4]; put_u32(id, objid);
    uint8_t tlv[8]; size_t off = 0;
    int ret = tlv_put(tlv, sizeof(tlv), &off, TAG_1, id, sizeof(id));
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint8_t resp[SE050_MAX_INF]; size_t rlen = 0; uint16_t sw = 0;
    ret = se05x_apdu(d, INS_READ, P1_DEFAULT, P2_DEFAULT, tlv, off,
                     resp, sizeof(resp), &rlen, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        return ret;
    }
    if (sw != SW_OK) {
        return -EIO;
    }

    size_t vlen = 0;
    const uint8_t *v = tlv_find(resp, rlen, TAG_1, &vlen);
    if (!v) {
        return -EPROTO;
    }
    /* Value is the uncompressed EC point 0x04||X||Y (possibly with a DER
     * wrapper). Locate the 65-byte 0x04-prefixed point. */
    for (size_t i = 0; i + SE050_P256_PUB_LEN <= vlen; i++) {
        if (v[i] == 0x04) {
            memcpy(pub, &v[i], SE050_P256_PUB_LEN);
            return 0;
        }
    }
    if (vlen == SE050_P256_PUB_LEN && v[0] == 0x04) {
        memcpy(pub, v, SE050_P256_PUB_LEN);
        return 0;
    }
    return -EPROTO;
}

/* Convert a DER-encoded ECDSA signature (SEQ{ INT r, INT s }) to raw r||s. */
static int der_to_raw(const uint8_t *der, size_t der_len, uint8_t out[64])
{
    if (der_len < 8 || der[0] != 0x30) {
        return -EINVAL;
    }
    size_t i = 2;                       /* skip SEQ tag + length */
    memset(out, 0, 64);

    for (int part = 0; part < 2; part++) {
        if (i + 2 > der_len || der[i] != 0x02) {
            return -EINVAL;
        }
        size_t ilen = der[i + 1];
        i += 2;
        if (i + ilen > der_len) {
            return -EINVAL;
        }
        const uint8_t *p = &der[i];
        /* strip a leading 0x00 sign byte */
        while (ilen > 32 && *p == 0x00) { p++; ilen--; }
        if (ilen > 32) {
            return -EINVAL;
        }
        memcpy(out + part * 32 + (32 - ilen), p, ilen);
        i += der[i - 1];                /* advance by the original ilen */
    }
    return 0;
}

int se050_ecc_sign(const struct device *dev, uint32_t objid,
                  const uint8_t hash[32], uint8_t sig_rs[SE050_P256_SIG_LEN])
{
    struct se050_data *d = dev->data;
    uint8_t id[4]; put_u32(id, objid);
    uint8_t algo = ECSIG_SHA256;
    uint8_t tlv[64]; size_t off = 0;
    int ret = tlv_put(tlv, sizeof(tlv), &off, TAG_1, id, sizeof(id));
    if (ret == 0) ret = tlv_put(tlv, sizeof(tlv), &off, TAG_2, &algo, 1);
    if (ret == 0) ret = tlv_put(tlv, sizeof(tlv), &off, TAG_3, hash, 32);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&d->lock, K_FOREVER);
    uint8_t resp[SE050_MAX_INF]; size_t rlen = 0; uint16_t sw = 0;
    ret = se05x_apdu(d, INS_CRYPTO, P1_SIGNATURE, P2_SIGN, tlv, off,
                     resp, sizeof(resp), &rlen, &sw);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        return ret;
    }
    if (sw != SW_OK) {
        LOG_ERR("ECDSASign SW=0x%04X", sw);
        return -EIO;
    }

    size_t vlen = 0;
    const uint8_t *der = tlv_find(resp, rlen, TAG_1, &vlen);
    if (!der) {
        return -EPROTO;
    }
    return der_to_raw(der, vlen, sig_rs);
}

/* =========================================================================
 * Init
 * ========================================================================= */
static int se050_init(const struct device *dev)
{
    const struct se050_config *cfg = dev->config;
    struct se050_data *d = dev->data;

    k_mutex_init(&d->lock);

    int ret = se050_transport_configure(&d->link, &cfg->i2c, &cfg->ena);
    if (ret < 0) {
        LOG_ERR("SE050 link configure failed: %d", ret);
        return ret;
    }

    ret = se050_transport_power_on(&d->link);
    if (ret < 0) {
        LOG_ERR("SE050 power-on failed: %d", ret);
        return ret;
    }

    ret = se050_bringup(d);
    if (ret < 0) {
        /* Leave the part powered so the shell/consumers can retry; the bus
         * or applet may not be present/selectable on this unit. */
        LOG_WRN("SE050 bring-up failed (%d) — driver idle, retry via `se050`", ret);
        return 0;
    }

    LOG_INF("SE050 secure element ready (applet selected)");
    return 0;
}

/* =========================================================================
 * Optional bring-up shell: `se050 info` / `se050 rand <n>`
 * ========================================================================= */
#ifdef CONFIG_SHELL
static int cmd_se050_info(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    const struct device *dev = se050_get_device();
    if (!dev) {
        shell_error(sh, "SE050 not available");
        return -ENODEV;
    }
    struct se050_data *d = dev->data;
    shell_print(sh, "SE050: powered=%d ready=%d ATR=%u bytes",
                se050_transport_is_powered(&d->link), d->ready,
                (unsigned)d->link.atr_len);
    return 0;
}

static int cmd_se050_rand(const struct shell *sh, size_t argc, char **argv)
{
    const struct device *dev = se050_get_device();
    if (!dev) {
        shell_error(sh, "SE050 not available");
        return -ENODEV;
    }
    int n = (argc > 1) ? atoi(argv[1]) : 16;
    if (n <= 0 || n > 64) {
        shell_error(sh, "usage: se050 rand <1..64>");
        return -EINVAL;
    }
    uint8_t buf[64];
    int ret = se050_get_random(dev, buf, n);
    if (ret < 0) {
        shell_error(sh, "get_random failed: %d", ret);
        return ret;
    }
    shell_fprintf(sh, SHELL_NORMAL, "random: ");
    for (int i = 0; i < n; i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%02X", buf[i]);
    }
    shell_print(sh, "");
    return 0;
}

/* Raw APDU probe: `se050 apdu 80040049024100` → sends the hex APDU and prints
 * the raw response (payload + SW). Bring-up/debug aid only. */
static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int cmd_se050_apdu(const struct shell *sh, size_t argc, char **argv)
{
    const struct device *dev = se050_get_device();
    if (!dev) {
        shell_error(sh, "SE050 not available");
        return -ENODEV;
    }
    const char *hex = argv[1];
    size_t hlen = strlen(hex);
    if (hlen == 0 || (hlen & 1)) {
        shell_error(sh, "usage: se050 apdu <even-length hex>");
        return -EINVAL;
    }
    uint8_t cmd[SE050_MAX_INF];
    size_t clen = hlen / 2;
    if (clen > sizeof(cmd)) {
        shell_error(sh, "APDU too long");
        return -EINVAL;
    }
    for (size_t i = 0; i < clen; i++) {
        int hi = hexnib(hex[i * 2]), lo = hexnib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            shell_error(sh, "bad hex at %u", (unsigned)(i * 2));
            return -EINVAL;
        }
        cmd[i] = (uint8_t)((hi << 4) | lo);
    }

    struct se050_data *d = dev->data;
    uint8_t resp[SE050_MAX_INF];
    size_t rlen = 0;
    k_mutex_lock(&d->lock, K_FOREVER);
    int ret = se050_transport_apdu(&d->link, cmd, clen, resp, sizeof(resp), &rlen);
    k_mutex_unlock(&d->lock);
    if (ret < 0) {
        shell_error(sh, "apdu xfer failed: %d", ret);
        return ret;
    }
    shell_fprintf(sh, SHELL_NORMAL, "resp[%u]: ", (unsigned)rlen);
    for (size_t i = 0; i < rlen; i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%02X", resp[i]);
    }
    shell_print(sh, "");
    return 0;
}

/* Exercise the driver ECC path end to end: generate (idempotent) an on-die
 * P-256 key, read its public key, and ECDSA-sign a fixed digest. */
static int cmd_se050_ecctest(const struct shell *sh, size_t argc, char **argv)
{
    const struct device *dev = se050_get_device();
    if (!dev) {
        shell_error(sh, "SE050 not available");
        return -ENODEV;
    }
    uint32_t objid = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0)
                                : 0x50500011;

    int ret = se050_ecc_gen_key(dev, objid);
    if (ret < 0) {
        shell_error(sh, "gen_key(0x%08X) failed: %d", objid, ret);
        return ret;
    }
    uint8_t pub[SE050_P256_PUB_LEN];
    ret = se050_ecc_get_pub(dev, objid, pub);
    if (ret < 0) {
        shell_error(sh, "get_pub failed: %d", ret);
        return ret;
    }
    uint8_t digest[32];
    memset(digest, 0x5A, sizeof(digest));
    uint8_t sig[SE050_P256_SIG_LEN];
    ret = se050_ecc_sign(dev, objid, digest, sig);
    if (ret < 0) {
        shell_error(sh, "sign failed: %d", ret);
        return ret;
    }
    shell_fprintf(sh, SHELL_NORMAL, "pub[04..]: ");
    for (int i = 0; i < 8; i++) shell_fprintf(sh, SHELL_NORMAL, "%02X", pub[i]);
    shell_fprintf(sh, SHELL_NORMAL, "...  sig r||s: ");
    for (int i = 0; i < 8; i++) shell_fprintf(sh, SHELL_NORMAL, "%02X", sig[i]);
    shell_print(sh, "...  OK (key 0x%08X)", objid);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(se050_sub,
    SHELL_CMD(info, NULL, "Show SE050 status", cmd_se050_info),
    SHELL_CMD_ARG(rand, NULL, "Get N random bytes", cmd_se050_rand, 1, 1),
    SHELL_CMD_ARG(apdu, NULL, "Send raw hex APDU, print response", cmd_se050_apdu, 2, 0),
    SHELL_CMD_ARG(ecctest, NULL, "P-256 gen+pub+sign self-test [objid]", cmd_se050_ecctest, 1, 1),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(se050, &se050_sub, "SE050 secure element", NULL);
#endif /* CONFIG_SHELL */

/* =========================================================================
 * Device instantiation
 * ========================================================================= */
#define SE050_INIT(n)                                                         \
    static const struct se050_config se050_cfg_##n = {                        \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                       \
        .ena = GPIO_DT_SPEC_INST_GET_OR(n, enable_gpios, {0}),                \
    };                                                                        \
    static struct se050_data se050_data_##n;                                  \
    DEVICE_DT_INST_DEFINE(n, se050_init, NULL,                                 \
                          &se050_data_##n, &se050_cfg_##n,                     \
                          POST_KERNEL, 60, NULL);

DT_INST_FOREACH_STATUS_OKAY(SE050_INIT)
