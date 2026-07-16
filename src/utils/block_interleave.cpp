#include "block_interleave.h"

// Перемежение и депемежение — одна и та же операция
// interleave[j] = original[(j * 5) % 24]
// 5 является собственным обратным: 5*5 = 25 ≡ 1 (mod 24)
void block_interleave(uint8_t *buf) {
    uint8_t tmp[INTERLEAVE_SIZE];
    for (int j = 0; j < INTERLEAVE_SIZE; j++) {
        tmp[j] = buf[(j * 5) % INTERLEAVE_SIZE];
    }
    for (int j = 0; j < INTERLEAVE_SIZE; j++) {
        buf[j] = tmp[j];
    }
}
