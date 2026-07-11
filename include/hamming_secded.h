#ifndef HAMMING_SECDED_H
#define HAMMING_SECDED_H

#include <stdint.h>
#include <stddef.h>

// Побайтовый Хэмминг SECDED: (12,8) код
// Упаковка: 2 байта данных → 3 байта закодированных
// 16 байт данных → 24 байта + burst_id (1) + CRC (1) = 26 байт

#define HAMMING_DATA_SIZE    16   // байт данных (payload без burst_id и CRC)
#define HAMMING_CODED_SIZE   24   // закодированные байты (16 × 12 / 8)
#define HAMMING_PACKET_SIZE  26   // burst_id + 24 + CRC

#ifdef __cplusplus
extern "C" {
#endif

// Кодирование: in[16] → out[24]
void hamming_encode(const uint8_t *in, uint8_t *out);

// Декодирование: in[24] → out[16]
// Возвращает кол-во исправленных бит (0..8), или -1 если неисправимо
int hamming_decode(const uint8_t *in, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif // HAMMING_SECDED_H
