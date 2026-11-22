/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "vnccrypto.h"

#include <QtEndian>

#include <array>
#include <cstring>
#include <vector>

namespace {

// The standard's tables. Bits are numbered from 1, most significant first.
constexpr quint8 kInitialPermutation[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
};
constexpr quint8 kFinalPermutation[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25,
};
constexpr quint8 kExpansion[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
};
constexpr quint8 kPermutation[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25,
};
constexpr quint8 kPermutedChoice1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4,
};
constexpr quint8 kPermutedChoice2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
};
constexpr quint8 kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
constexpr quint8 kSBoxes[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
     0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
     15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
     3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
     13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
     13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
     1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
     13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
     3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
     14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
     11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
     10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
     4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
     13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
     6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
     1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
     2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11},
};

// Bit i of the result (from the top) is bit table[i] of the inBits-wide input.
template<int N>
quint64 permute(quint64 in, int inBits, const quint8 (&table)[N])
{
    quint64 out = 0;
    for (quint8 bit : table)
        out = (out << 1) | ((in >> (inBits - bit)) & 1);
    return out;
}

quint32 rotate28(quint32 half, int by)
{
    return ((half << by) | (half >> (28 - by))) & 0x0fffffff;
}

} // namespace

void desEncryptBlock(const quint8 key[8], quint8 block[8])
{
    // The sixteen round keys.
    const quint64 cd = permute(qFromBigEndian<quint64>(key), 64, kPermutedChoice1);
    quint32 c = quint32(cd >> 28), d = quint32(cd & 0x0fffffff);
    quint64 roundKeys[16];
    for (int round = 0; round < 16; ++round) {
        c = rotate28(c, kShifts[round]);
        d = rotate28(d, kShifts[round]);
        roundKeys[round] = permute((quint64(c) << 28) | d, 56, kPermutedChoice2);
    }

    const quint64 in = permute(qFromBigEndian<quint64>(block), 64, kInitialPermutation);
    quint32 left = quint32(in >> 32), right = quint32(in);
    for (int round = 0; round < 16; ++round) {
        const quint64 mixed = permute(right, 32, kExpansion) ^ roundKeys[round];
        quint32 substituted = 0;
        for (int box = 0; box < 8; ++box) {
            const int six = int(mixed >> (42 - 6 * box)) & 0x3f;
            const int row = ((six & 0x20) >> 4) | (six & 1);
            const int column = (six >> 1) & 0x0f;
            substituted = (substituted << 4) | kSBoxes[box][row * 16 + column];
        }
        const quint32 next = left ^ quint32(permute(substituted, 32, kPermutation));
        left = right;
        right = next;
    }
    // The halves go back the other way round.
    const quint64 out = permute((quint64(right) << 32) | left, 64, kFinalPermutation);
    qToBigEndian(out, block);
}

/*
 * AES-128
 */

namespace {

// Multiplication in AES's field, GF(2^8) mod x^8 + x^4 + x^3 + x + 1.
quint8 gfMultiply(quint8 a, quint8 b)
{
    quint8 product = 0;
    for (; b; b >>= 1) {
        if (b & 1)
            product ^= a;
        a = quint8((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
    }
    return product;
}

// The S-box, worked out as the standard defines it (inverse, then the affine
// map) rather than copied out.
const std::array<quint8, 256> &aesSBox()
{
    static const std::array<quint8, 256> box = [] {
        std::array<quint8, 256> out{};
        const auto rotate = [](quint8 v, int by) { return quint8((v << by) | (v >> (8 - by))); };
        for (int x = 0; x < 256; ++x) {
            quint8 inverse = 0;   // x^254, which is 0 for 0
            if (x) {
                quint8 power = quint8(x);
                inverse = 1;
                for (int e = 254; e; e >>= 1) {
                    if (e & 1)
                        inverse = gfMultiply(inverse, power);
                    power = gfMultiply(power, power);
                }
            }
            out[x] = inverse ^ rotate(inverse, 1) ^ rotate(inverse, 2) ^ rotate(inverse, 3)
                     ^ rotate(inverse, 4) ^ 0x63;
        }
        return out;
    }();
    return box;
}

} // namespace

void aes128EncryptBlock(const quint8 key[16], quint8 block[16])
{
    const std::array<quint8, 256> &sbox = aesSBox();

    // Eleven round keys, one after the other.
    quint8 keys[176];
    std::memcpy(keys, key, 16);
    quint8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        quint8 word[4] = {keys[i - 4], keys[i - 3], keys[i - 2], keys[i - 1]};
        if (i % 16 == 0) {
            // Rotate, substitute, and add the round constant.
            const quint8 first = word[0];
            word[0] = sbox[word[1]] ^ rcon;
            word[1] = sbox[word[2]];
            word[2] = sbox[word[3]];
            word[3] = sbox[first];
            rcon = gfMultiply(rcon, 2);
        }
        for (int j = 0; j < 4; ++j)
            keys[i + j] = keys[i - 16 + j] ^ word[j];
    }

    // The state is column by column: byte r + 4c is row r, column c.
    for (int i = 0; i < 16; ++i)
        block[i] ^= keys[i];
    for (int round = 1; round <= 10; ++round) {
        quint8 shifted[16];
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column)
                shifted[row + 4 * column] = sbox[block[row + 4 * ((column + row) % 4)]];
        }
        if (round < 10) {
            for (int column = 0; column < 4; ++column) {
                const quint8 *a = shifted + 4 * column;
                const quint8 mixed[4] = {
                    quint8(gfMultiply(a[0], 2) ^ gfMultiply(a[1], 3) ^ a[2] ^ a[3]),
                    quint8(a[0] ^ gfMultiply(a[1], 2) ^ gfMultiply(a[2], 3) ^ a[3]),
                    quint8(a[0] ^ a[1] ^ gfMultiply(a[2], 2) ^ gfMultiply(a[3], 3)),
                    quint8(gfMultiply(a[0], 3) ^ a[1] ^ a[2] ^ gfMultiply(a[3], 2)),
                };
                std::memcpy(shifted + 4 * column, mixed, 4);
            }
        }
        for (int i = 0; i < 16; ++i)
            block[i] = shifted[i] ^ keys[16 * round + i];
    }
}

/*
 * Modular exponentiation, by Montgomery multiplication on 32-bit words
 */

namespace {

using Words = std::vector<quint32>;   // least significant first

Words toWords(const QByteArray &bigEndian, int count)
{
    Words out(count, 0);
    const qsizetype size = bigEndian.size();
    for (qsizetype i = 0; i < size && i / 4 < count; ++i)
        out[i / 4] |= quint32(quint8(bigEndian.at(size - 1 - i))) << (8 * (i % 4));
    return out;
}

QByteArray fromWords(const Words &words, int bytes)
{
    QByteArray out(bytes, '\0');
    for (int i = 0; i < bytes; ++i)
        out[bytes - 1 - i] = char((words[i / 4] >> (8 * (i % 4))) & 0xff);
    return out;
}

int compare(const Words &a, const Words &b)
{
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

void subtract(Words &a, const Words &b)   // a -= b, modulo 2^(32n)
{
    quint64 borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const quint64 difference = quint64(a[i]) - b[i] - borrow;
        a[i] = quint32(difference);
        borrow = (difference >> 32) & 1;
    }
}

/* a * b / R mod m, for a, b < m, where R = 2^(32n); mInverse = -m^-1 mod 2^32. */
Words montgomeryMultiply(const Words &a, const Words &b, const Words &m, quint32 mInverse)
{
    const size_t n = m.size();
    Words t(n + 2, 0);
    for (size_t i = 0; i < n; ++i) {
        quint64 carry = 0;
        for (size_t j = 0; j < n; ++j) {
            const quint64 sum = quint64(t[j]) + quint64(a[j]) * b[i] + carry;
            t[j] = quint32(sum);
            carry = sum >> 32;
        }
        quint64 sum = quint64(t[n]) + carry;
        t[n] = quint32(sum);
        t[n + 1] = quint32(sum >> 32);

        // Add a multiple of m that clears the lowest word, and drop it.
        const quint32 q = t[0] * mInverse;
        carry = (quint64(t[0]) + quint64(q) * m[0]) >> 32;
        for (size_t j = 1; j < n; ++j) {
            const quint64 s = quint64(t[j]) + quint64(q) * m[j] + carry;
            t[j - 1] = quint32(s);
            carry = s >> 32;
        }
        sum = quint64(t[n]) + carry;
        t[n - 1] = quint32(sum);
        t[n] = t[n + 1] + quint32(sum >> 32);
    }
    // Less than 2m: at most one subtraction (which wraps the overflow word away).
    Words result(t.begin(), t.begin() + n);
    if (t[n] != 0 || compare(result, m) >= 0)
        subtract(result, m);
    return result;
}

} // namespace

QByteArray modPow(const QByteArray &base, const QByteArray &exponent, const QByteArray &modulus)
{
    const int bytes = int(modulus.size());
    const int n = (bytes + 3) / 4;
    if (n == 0)
        return {};
    const Words m = toWords(modulus, n);
    if (!(m[0] & 1))
        return {};
    QByteArray trimmed = base;
    while (trimmed.size() > bytes && trimmed.at(0) == 0)
        trimmed.remove(0, 1);
    if (trimmed.size() > bytes)
        return {};
    const Words b = toWords(trimmed, n);
    if (compare(b, m) >= 0)
        return {};

    // -m^-1 mod 2^32, by Newton's iteration (each doubles the correct bits).
    quint32 inverse = m[0];
    for (int i = 0; i < 5; ++i)
        inverse *= 2 - m[0] * inverse;
    const quint32 mInverse = 0 - inverse;

    // R^2 mod m, by doubling 1 that many times, for moving into Montgomery form.
    Words r2(n, 0);
    r2[0] = 1;
    for (int i = 0; i < 64 * n; ++i) {
        const bool overflow = r2[n - 1] >> 31;
        for (int j = n - 1; j > 0; --j)
            r2[j] = (r2[j] << 1) | (r2[j - 1] >> 31);
        r2[0] <<= 1;
        if (overflow || compare(r2, m) >= 0)
            subtract(r2, m);
    }

    Words one(n, 0);
    one[0] = 1;
    const Words x = montgomeryMultiply(b, r2, m, mInverse);
    Words result = montgomeryMultiply(one, r2, m, mInverse);   // 1, in Montgomery form
    for (char byte : exponent) {
        for (int bit = 7; bit >= 0; --bit) {
            result = montgomeryMultiply(result, result, m, mInverse);
            if ((quint8(byte) >> bit) & 1)
                result = montgomeryMultiply(result, x, m, mInverse);
        }
    }
    return fromWords(montgomeryMultiply(result, one, m, mInverse), bytes);
}
