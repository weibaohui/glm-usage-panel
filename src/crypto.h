/* crypto.h — SHA-256, HMAC-SHA256, base64url and GLM (Zhipu BigModel) JWT minting.
 * Pure C99, no dependencies: the app signs a fresh HS256 JWT for every request,
 * because the BigModel servers also check the JWT's timestamp freshness. */
#ifndef GLM_CRYPTO_H
#define GLM_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

void glm_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void glm_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]);

/* base64url without padding; returns the number of characters written (excluding NUL). */
size_t glm_b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* Mint a BigModel API JWT from an API key of the form "<id>.<secret>".
 * Header {"alg":"HS256","sign_type":"SIGN"}, payload {"api_key","exp","timestamp"},
 * one hour of validity, signed with the secret part of the key.
 * Returns true and writes a NUL-terminated JWT to out on success. */
int glm_jwt_mint(const char *api_key, long long now_epoch, char *out, size_t cap);

#endif
