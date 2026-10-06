/* ==================================================================
 * auth.c - パスワードの鍵と確かめ
 *
 *  ini にはパスワードそのものを置かない。ソルト(16 バイト、毎回乱数)と
 *  PBKDF2-SHA256(IIV_PBKDF2_ITER 回)で導いた 32 バイトの鍵だけを置く:
 *      password=pbkdf2-sha256$<回数>$<ソルト 16 進>$<鍵 16 進>
 *  接続のたびにサーバーはノンス(32 バイト乱数)を送り、相手は同じソルトと回数で
 *  鍵を導いて HMAC-SHA256(鍵, ノンス || "iiv-auth") を返す。鍵は回線を通らない。
 *  どれも Windows 標準の BCrypt(Windows 7 以降)。
 * ================================================================== */

#include "iiv.h"
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

static const char k_label[] = "iiv-auth";

BOOL auth_random(void *p, ULONG n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

BOOL auth_derive(const char *utf8pw, int len, const BYTE *salt, DWORD iter, BYTE key[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BOOL ok;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return FALSE;
    ok = BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(alg, (PUCHAR)utf8pw, (ULONG)len, (PUCHAR)salt, 16, iter, key, 32, 0));
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

BOOL auth_hmac(const BYTE key[32], const void *data, ULONG n, BYTE out[32])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    BOOL ok = FALSE;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return FALSE;
    if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, (PUCHAR)key, 32, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)data, n, 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(h, out, 32, 0))) ok = TRUE;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

BOOL auth_set_password(PwKey *k, const WCHAR *pw)
{
    char *u;
    int   len;
    BOOL  ok;
    ZeroMemory(k, sizeof(*k));
    if (!pw || !pw[0]) return TRUE;
    u = utf16_to_utf8(pw, &len);
    if (!u) return FALSE;
    k->iter = IIV_PBKDF2_ITER;
    ok = auth_random(k->salt, 16) && auth_derive(u, len, k->salt, k->iter, k->key);
    SecureZeroMemory(u, (size_t)len);
    free(u);
    k->set = ok;
    return ok;
}

static void to_hex(const BYTE *p, int n, char *out)
{
    static const char d[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) { out[i * 2] = d[p[i] >> 4]; out[i * 2 + 1] = d[p[i] & 15]; }
    out[n * 2] = 0;
}

static BOOL from_hex(const char *s, BYTE *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int hi, lo;
        char a = s[i * 2], b = a ? s[i * 2 + 1] : 0;
        hi = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
        lo = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return FALSE;
        p[i] = (BYTE)(hi * 16 + lo);
    }
    return TRUE;
}

void auth_to_text(const PwKey *k, char *out, int cap)
{
    char salt[33], key[65];
    if (!k->set) { if (cap) out[0] = 0; return; }
    to_hex(k->salt, 16, salt);
    to_hex(k->key, 32, key);
    _snprintf(out, (size_t)cap, "pbkdf2-sha256$%lu$%s$%s", (unsigned long)k->iter, salt, key);
    out[cap - 1] = 0;
}

void auth_from_text(PwKey *k, const char *s)
{
    const char *p;
    ZeroMemory(k, sizeof(*k));
    if (strncmp(s, "pbkdf2-sha256$", 14)) return;
    p = s + 14;
    k->iter = (DWORD)strtoul(p, NULL, 10);
    p = strchr(p, '$');
    if (!p || k->iter < 1000 || !from_hex(p + 1, k->salt, 16) || p[33] != '$' || !from_hex(p + 34, k->key, 32)) {
        ZeroMemory(k, sizeof(*k));
        return;
    }
    k->set = TRUE;
}

BOOL auth_check(const PwKey *k, const BYTE nonce[32], const BYTE proof[32])
{
    BYTE msg[32 + sizeof(k_label) - 1], mac[32];
    int  i, diff = 0;
    if (!k->set) return FALSE;
    memcpy(msg, nonce, 32);
    memcpy(msg + 32, k_label, sizeof(k_label) - 1);
    if (!auth_hmac(k->key, msg, sizeof(msg), mac)) return FALSE;
    for (i = 0; i < 32; i++) diff |= mac[i] ^ proof[i];     /* 時間で漏らさない */
    return diff == 0;
}
