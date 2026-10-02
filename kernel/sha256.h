#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[64];
    uint32_t buffer_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256(const void *data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif
