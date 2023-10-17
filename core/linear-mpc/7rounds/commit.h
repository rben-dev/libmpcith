#ifndef LIBMPCITH_COMMIT_H
#define LIBMPCITH_COMMIT_H

#include "parameters.h"
#include "mpc.h"
#include <stdint.h>

// Commitment x1
void commit_share(uint8_t* digest, const vec_share_t* share, const uint8_t* salt, uint16_t e, uint16_t i);
void commit_seed(uint8_t* digest, const uint8_t* seed, const uint8_t* salt, uint16_t e, uint16_t i);
void commit_seed_and_wit(uint8_t* digest, const uint8_t* seed, const vec_wit_t* wit, const uint8_t* salt, uint16_t e, uint16_t i);
void commit_hint(uint8_t* digest, const vec_hint_t* hint, const uint8_t* salt, uint16_t e, uint16_t i);

// Commitment x4
void commit_share_x4(uint8_t** digest, vec_share_t const* const* share, const uint8_t* salt, uint16_t e, const uint16_t* i);
void commit_seed_x4(uint8_t** digest, uint8_t const* const* seed, const uint8_t* salt, uint16_t e, const uint16_t* i);
void commit_seed_and_wit_x4(uint8_t** digest, uint8_t const* const* seed, vec_wit_t const* const* wit, const uint8_t* salt, uint16_t e, const uint16_t* i);
void commit_hint_x4(uint8_t** digest, vec_hint_t const* const* hint, const uint8_t* salt, uint16_t e, const uint16_t* i);

#endif /* LIBMPCITH_COMMIT_H */
