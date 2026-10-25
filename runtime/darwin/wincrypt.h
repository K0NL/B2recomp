/* [Recomps#1] Darwin only: the CryptoAPI subset the runtime uses (nnmisc.c nn::crypto, nnnet.c): random bytes (getrandom)
 * and SHA-1 / MD5 hashes (win32_compat.c, small implementations of RFC 3174 / RFC 1321, checked by compat_test.c). */
#ifndef RECOMPS_DARWIN_WINCRYPT_H
#define RECOMPS_DARWIN_WINCRYPT_H
#include <windows.h>
typedef ULONG_PTR HCRYPTPROV, HCRYPTHASH, HCRYPTKEY;
typedef unsigned int ALG_ID;
#define PROV_RSA_FULL 1
#define PROV_RSA_AES 24
#define CRYPT_VERIFYCONTEXT 0xF0000000u
#define CALG_MD5 0x8003u
#define CALG_SHA1 0x8004u
#define HP_HASHVAL 0x2u
#define HP_HASHSIZE 0x4u
BOOL CryptAcquireContextA(HCRYPTPROV *prov, LPCSTR container, LPCSTR provider, DWORD type, DWORD flags);
#define CryptAcquireContext CryptAcquireContextA
BOOL CryptReleaseContext(HCRYPTPROV prov, DWORD flags);
BOOL CryptGenRandom(HCRYPTPROV prov, DWORD n, BYTE *out);
BOOL CryptCreateHash(HCRYPTPROV prov, ALG_ID alg, HCRYPTKEY key, DWORD flags, HCRYPTHASH *out);
BOOL CryptHashData(HCRYPTHASH h, const BYTE *data, DWORD n, DWORD flags);
BOOL CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *out, DWORD *n, DWORD flags);
BOOL CryptDestroyHash(HCRYPTHASH h);
#endif
