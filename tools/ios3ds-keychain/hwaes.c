/*
 * ios3ds-hwaes.dylib: software stand-ins for IOAESAccelerator and
 * AppleKeyStore in securityd, so the iOS keychain works on the 3DS.
 *
 * iOS 4.2.1's securityd (keychain-2.db) encrypts each item with a random
 * key (CCCrypt) and has AppleKeyStore wrap that key with the class key of the
 * item's protection class (IOConnectCallMethod selector 10 = KeyWrap,
 * 11 = KeyUnwrap, RFC 3394 sizes: 32 -> 40 bytes). The class keys live in
 * the system keybag, which keybagd loads at boot; keybagd needs the iPod's
 * AES engine and effaceable storage and is disabled here (re-log 18), and
 * there is no IOAESAccelerator either. So every SecItemAdd failed with
 * errSecInteractionNotAllowed (-25308): no Wi-Fi passwords, no mail
 * accounts. Lookups still worked (they found nothing).
 *
 * This library is inserted into securityd (DYLD_INSERT_LIBRARIES in its
 * launchd plist, rootfs recipe) and answers exactly those paths:
 *
 *   IOServiceMatching("IOAESAccelerator" / "AppleKeyStore") -> IOResources
 *   IOServiceOpen(IOResources)        -> a private connection handle
 *   AppleKeyStore selector 0 (open)   -> ok
 *            selector 10/11           -> RFC 3394 AES key wrap / unwrap with
 *                                        one software key for every class
 *            anything else            -> kIOReturnUnsupported
 *   IOAESAccelerator selector 1       -> AES-CBC in software; hardware keys
 *                                        (UID/GID/0x835) = the software key
 *
 * The keychain at rest is therefore NOT bound to the console: anyone with
 * the SD card's rootfs.hfs can decrypt it (docs/wifi.md section 10).
 * IOAESAccelerator request layout (securityd 0xaacc): cleartext*,
 * ciphertext*, size, iv[16], mode (0 encrypt, 1 decrypt), key bits, key[32],
 * mask, zero. KeyWrap call (securityd 0xae64): scalars {keybag, class},
 * input struct = key, output struct capacity = input + 8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

typedef unsigned int io_object_t, io_connect_t, mach_port_t, task_port_t;
typedef int kern_return_t;
typedef unsigned char Boolean;
typedef struct __CFDictionary *CFMutableDictionaryRef;
CFMutableDictionaryRef IOServiceMatching(const char *);
kern_return_t IOServiceOpen(io_object_t, task_port_t, uint32_t, io_connect_t *);
kern_return_t IOServiceClose(io_connect_t);
Boolean IOObjectConformsTo(io_object_t, const char *);
kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
kern_return_t IOConnectCallMethod(mach_port_t, uint32_t, const uint64_t *, uint32_t, const void *, size_t,
                                  uint64_t *, uint32_t *, void *, size_t *);
const char *getprogname(void);
int unsetenv(const char *);

/* CommonCrypto (libSystem) */
typedef int32_t CCCryptorStatus;
CCCryptorStatus CCCrypt(uint32_t op, uint32_t alg, uint32_t options, const void *key, size_t keyLength,
                        const void *iv, const void *dataIn, size_t dataInLength, void *dataOut,
                        size_t dataOutAvailable, size_t *dataOutMoved);
#define kCCEncrypt      0
#define kCCDecrypt      1
#define kCCAlgorithmAES128 0

#define AES_CONN        0x1ae5c0deu     /* never valid Mach port names here */
#define KS_CONN         0x1ae5c0dfu
#define kIOReturnBadArgument 0xe00002c2
#define kIOReturnError       0xe00002bc
#define kIOReturnUnsupported 0xe00002c7
#define kCCOptionECBMode     2

struct aes_req {                        /* 0x4C bytes, iOS 4.2.1 */
    void *in, *out;
    uint32_t size;
    uint8_t iv[16];
    uint32_t mode;
    uint32_t bits;
    uint8_t key[32];
    uint32_t mask;
    uint32_t zero;
};
_Static_assert(sizeof(struct aes_req) == 0x4C, "IOAESAccelerator request layout");

/* stands in for the console's UID-derived hardware keys */
static const uint8_t soft_key[16] = {
    0x69, 0x6f, 0x73, 0x33, 0x64, 0x73, 0x2d, 0x6b, 0x65, 0x79, 0x63, 0x68, 0x61, 0x69, 0x6e, 0x31
};
static uint32_t pending;            /* the special service last matched */

static void say(const char *fmt, const char *a, int ks)
{
    char line[160];
    int n = snprintf(line, sizeof line, fmt, a, ks ? "AppleKeyStore (key wrap)" : "IOAESAccelerator");
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0 && n > 0) { if (write(fd, line, (size_t)n) < 0) { } close(fd); }
}

static CFMutableDictionaryRef my_IOServiceMatching(const char *name)
{
    if (name && (!strcmp(name, "IOAESAccelerator") || !strcmp(name, "AppleKeyStore"))) {
        pending = name[0] == 'I' ? AES_CONN : KS_CONN;
        return IOServiceMatching("IOResources");
    }
    return IOServiceMatching(name);
}

static kern_return_t my_IOServiceOpen(io_object_t s, task_port_t t, uint32_t type, io_connect_t *c)
{
    if (pending && c && IOObjectConformsTo(s, "IOResources")) {
        *c = pending;
        say("[hwaes] %s: software %s\n", getprogname(), pending == AES_CONN ? 0 : 1);
        pending = 0;
        return 0;
    }
    return IOServiceOpen(s, t, type, c);
}

static kern_return_t my_IOServiceClose(io_connect_t c)
{
    return c == AES_CONN || c == KS_CONN ? 0 : IOServiceClose(c);
}

static kern_return_t my_IOConnectCallStructMethod(mach_port_t c, uint32_t sel, const void *in, size_t inlen,
                                                  void *out, size_t *outlen)
{
    if (c != AES_CONN)
        return IOConnectCallStructMethod(c, sel, in, inlen, out, outlen);
    const struct aes_req *r = in;
    if (sel != 1 || inlen < sizeof *r || !r->in || !r->out || (r->size & 15) ||
        (r->mask == 0 && r->bits != 128 && r->bits != 192 && r->bits != 256))
        return kIOReturnBadArgument;
    const void *key = r->mask ? soft_key : r->key;
    size_t keylen = r->mask ? sizeof soft_key : r->bits / 8, moved = 0;
    /* encrypt: in = cleartext -> out = ciphertext; decrypt the other way */
    const void *src = r->mode == 0 ? r->in : r->out;
    void *dst = r->mode == 0 ? r->out : r->in;
    if (CCCrypt(r->mode == 0 ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES128, 0, key, keylen, r->iv,
                src, r->size, dst, r->size, &moved) != 0)
        return kIOReturnBadArgument;
    if (out && outlen && *outlen >= sizeof *r) memcpy(out, r, sizeof *r);
    return 0;
}

/* RFC 3394 AES key wrap with the software key (n = 64-bit blocks of plaintext) */
static int aes_block(int op, uint8_t b[16])
{
    size_t moved = 0;
    return CCCrypt(op, kCCAlgorithmAES128, kCCOptionECBMode, soft_key, sizeof soft_key, NULL,
                   b, 16, b, 16, &moved) == 0 && moved == 16 ? 0 : -1;
}

static int key_wrap(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t n = len / 8;
    uint8_t a[8], b[16];
    memset(a, 0xA6, 8);
    memmove(out + 8, in, len);
    for (unsigned j = 0; j <= 5; j++)
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            if (aes_block(kCCEncrypt, b)) return -1;
            uint64_t t = (uint64_t)n * j + i;
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++) a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
    return 0;
}

static int key_unwrap(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t n = len / 8 - 1;
    uint8_t a[8], b[16], r[64 * 8];
    if (n > 64) return -1;
    memcpy(a, in, 8);
    memcpy(r, in + 8, 8 * n);
    for (int j = 5; j >= 0; j--)
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * j + i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, r + 8 * (i - 1), 8);
            if (aes_block(kCCDecrypt, b)) return -1;
            memcpy(a, b, 8);
            memcpy(r + 8 * (i - 1), b + 8, 8);
        }
    for (int k = 0; k < 8; k++)
        if (a[k] != 0xA6) return -1;            /* not ours / corrupt */
    memcpy(out, r, 8 * n);
    return 0;
}

static kern_return_t my_IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt,
                                            const void *inStruct, size_t inLen, uint64_t *out, uint32_t *outCnt,
                                            void *outStruct, size_t *outLen)
{
    if (c != KS_CONN)
        return IOConnectCallMethod(c, sel, in, inCnt, inStruct, inLen, out, outCnt, outStruct, outLen);
    if (outCnt) *outCnt = 0;
    switch (sel) {
    case 0:                             /* user client open */
        return 0;
    case 10:                            /* KeyWrap: n -> n + 8 */
        if (!inStruct || !outStruct || !outLen || (inLen & 7) || inLen < 16 || inLen > 512 ||
            *outLen < inLen + 8 || key_wrap(inStruct, inLen, outStruct))
            return kIOReturnBadArgument;
        *outLen = inLen + 8;
        return 0;
    case 11:                            /* KeyUnwrap: n -> n - 8 */
        if (!inStruct || !outStruct || !outLen || (inLen & 7) || inLen < 24 || inLen > 520 ||
            *outLen < inLen - 8)
            return kIOReturnBadArgument;
        if (key_unwrap(inStruct, inLen, outStruct)) return kIOReturnError;
        *outLen = inLen - 8;
        return 0;
    }
    return kIOReturnUnsupported;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *rep, *orig; } interpose[] = {
    { (const void *)my_IOServiceMatching, (const void *)IOServiceMatching },
    { (const void *)my_IOServiceOpen, (const void *)IOServiceOpen },
    { (const void *)my_IOServiceClose, (const void *)IOServiceClose },
    { (const void *)my_IOConnectCallStructMethod, (const void *)IOConnectCallStructMethod },
    { (const void *)my_IOConnectCallMethod, (const void *)IOConnectCallMethod },
};

__attribute__((constructor)) static void init(void)
{
    unsetenv("DYLD_INSERT_LIBRARIES");
}
