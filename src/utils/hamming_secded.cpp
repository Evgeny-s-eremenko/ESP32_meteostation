#include "hamming_secded.h"

// Побайтовый Хэмминг SECDED: (12,8) код
// Упаковка: 2 байта данных → 3 байта закодированных (2 × 12 = 24 бита)
// 8 пар × 3 = 24 байта. Вместе с burst_id (1) и CRC (1) = 26 байт.

// Расклад 12-битного слова:
//   Позиция:  1  2  3  4  5  6  7  8  9 10 11 12
//   Бит:     p1 p2 d0 p4 d1 d2 d3 p8 d4 d5 d6 d7
//
//   p1 = d0 ^ d1 ^ d3 ^ d4 ^ d6   (позиции с битом 0 в индексе)
//   p2 = d0 ^ d2 ^ d3 ^ d5 ^ d6   (позиции с битом 1)
//   p4 = d1 ^ d2 ^ d3 ^ d7        (позиции с битом 2)
//   p8 = d4 ^ d5 ^ d6 ^ d7        (позиции с битом 3)

static uint16_t encode_byte(uint8_t d) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (d >> i) & 1;

    uint8_t p1 = b[0] ^ b[1] ^ b[3] ^ b[4] ^ b[6];
    uint8_t p2 = b[0] ^ b[2] ^ b[3] ^ b[5] ^ b[6];
    uint8_t p4 = b[1] ^ b[2] ^ b[3] ^ b[7];
    uint8_t p8 = b[4] ^ b[5] ^ b[6] ^ b[7];

    return (p1) | (p2 << 1) | (b[0] << 2) | (p4 << 3) |
           (b[1] << 4) | (b[2] << 5) | (b[3] << 6) | (p8 << 7) |
           (b[4] << 8) | (b[5] << 9) | (b[6] << 10) | (b[7] << 11);
}

// Декодирование: 12 бит → 8 бит + коррекция
// Возвращает 0 (без ошибок), 1 (исправлено 1 бит), или -1 (неисправимо)
static int decode_word(uint16_t code, uint8_t *out) {
    uint8_t r[12];
    for (int i = 0; i < 12; i++) r[i] = (code >> i) & 1;

    // Вычисление синдрома
    uint8_t s1 = r[0] ^ r[2] ^ r[4] ^ r[6] ^ r[8] ^ r[10];
    uint8_t s2 = r[1] ^ r[2] ^ r[5] ^ r[6] ^ r[9] ^ r[10];
    uint8_t s4 = r[3] ^ r[4] ^ r[5] ^ r[6] ^ r[11];
    uint8_t s8 = r[7] ^ r[8] ^ r[9] ^ r[10] ^ r[11];
    uint8_t syndrome = s1 | (s2 << 1) | (s4 << 2) | (s8 << 3);

    if (syndrome == 0) {
        // Ошибок нет
    } else if (syndrome <= 12) {
        // Одна ошибка — корректируем бит в позиции syndrome
        r[syndrome - 1] ^= 1;
    } else {
        // 2+ ошибки — неисправимо
        return -1;
    }

    // Извлекаем данные: d0(2) d1(4) d2(5) d3(6) d4(8) d5(9) d6(10) d7(11)
    *out = (r[2]) | (r[4] << 1) | (r[5] << 2) | (r[6] << 3) |
           (r[8] << 4) | (r[9] << 5) | (r[10] << 6) | (r[11] << 7);
    return (syndrome != 0) ? 1 : 0;
}

// Кодирование: in[16] → out[24]
void hamming_encode(const uint8_t *in, uint8_t *out) {
    for (int i = 0; i < HAMMING_DATA_SIZE / 2; i++) {
        uint16_t c0 = encode_byte(in[i * 2]);
        uint16_t c1 = encode_byte(in[i * 2 + 1]);

        // Упаковка: [c0_0..c0_7] [c0_8..c0_11 | c1_0..c1_3] [c1_4..c1_11]
        out[i * 3 + 0] = (uint8_t)(c0 & 0xFF);
        out[i * 3 + 1] = (uint8_t)((c0 >> 8) | ((c1 & 0x0F) << 4));
        out[i * 3 + 2] = (uint8_t)(c1 >> 4);
    }
}

// Декодирование: in[24] → out[16]
// Возвращает кол-во исправленных бит (0..8), или -1 если хотя бы 1 байт неисправим
int hamming_decode(const uint8_t *in, uint8_t *out) {
    int total_corrected = 0;

    for (int i = 0; i < HAMMING_DATA_SIZE / 2; i++) {
        // Распаковка: 3 байта → 2 × 12-битных слова
        uint8_t b0 = in[i * 3 + 0];
        uint8_t b1 = in[i * 3 + 1];
        uint8_t b2 = in[i * 3 + 2];

        uint16_t c0 = b0 | ((uint16_t)(b1 & 0x0F) << 8);
        uint16_t c1 = ((uint16_t)(b1 >> 4)) | ((uint16_t)b2 << 4);

        uint8_t d0, d1;
        int r0 = decode_word(c0, &d0);
        int r1 = decode_word(c1, &d1);

        if (r0 < 0 || r1 < 0) return -1;

        out[i * 2]     = d0;
        out[i * 2 + 1] = d1;
        total_corrected += r0 + r1;
    }

    return total_corrected;
}
