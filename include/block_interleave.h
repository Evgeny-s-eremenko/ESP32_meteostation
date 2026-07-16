#ifndef BLOCK_INTERLEAVE_H
#define BLOCK_INTERLEAVE_H

#include <stdint.h>

// Байтовое перемежение / депемежение (stride=5, N=24)
// Одна и та же функция для обоих направлений: 5*5 ≡ 1 (mod 24)
#define INTERLEAVE_SIZE 24

#ifdef __cplusplus
extern "C" {
#endif

void block_interleave(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif // BLOCK_INTERLEAVE_H
