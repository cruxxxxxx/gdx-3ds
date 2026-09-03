/* Spike stub: blake2b surface Archive.cpp uses for record hashing. Real monocypher
 * is portable C and will be vendored for the actual port. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct crypto_blake2b_ctx {
    uint64_t hash[8];
    uint64_t input_offset[2];
    uint64_t input[16];
    size_t input_idx;
    size_t hash_size;
} crypto_blake2b_ctx;

void crypto_blake2b_init(crypto_blake2b_ctx* ctx, size_t hash_size);
void crypto_blake2b_update(crypto_blake2b_ctx* ctx, const uint8_t* message, size_t message_size);
void crypto_blake2b_final(crypto_blake2b_ctx* ctx, uint8_t* hash);
void crypto_blake2b(uint8_t* hash, size_t hash_size, const uint8_t* message, size_t message_size);

#ifdef __cplusplus
}
#endif
