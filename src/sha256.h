/* SHA-256 (FIPS 180-4), used to verify downloaded updates. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t bits;
  uint8_t  buf[64];
  size_t   used;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);
/* Writes the 64-char lowercase hex digest of data plus a NUL. */
void sha256_hex(const void *data, size_t len, char out[65]);
