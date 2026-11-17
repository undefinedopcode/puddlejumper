/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * This decoder is a port of remotex's (frontend/wasm/apple-adaptive,
 * github.com/andrewtheguy/remotex), which is:
 *
 * MIT License
 *
 * Copyright (c) 2026 Andrew Chen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "appleadaptive.h"

#include <QCoreApplication>

#include <array>
#include <cstring>
#include <vector>

/*
 * What follows is Apple's viewer's own decoding as remotex worked it out,
 * down to what it does with a stream that is not what it should be, since
 * the Mac codes each update against what its viewer will have made of the
 * last. A body is, by its first byte:
 *   0  a full update: two precisions, a command stream and a data stream;
 *   1  a partial update: one stream refining the last full update's tiles;
 *   2  the quantization tables, which come before the first picture.
 * The coefficients are JPEG's: zigzag order, the reference library's integer
 * inverse transform and its YCbCr, each block at full resolution.
 */

namespace {

constexpr int kFull = 0, kPartial = 1, kTables = 2;
constexpr qsizetype kTablesSize = 129;   // the kind, 64 luminance entries, 64 chrominance
// How far past a body's end its streams may be read before it is refused: a
// stream is read a few bytes ahead of what it spends.
constexpr qsizetype kOverread = 500;
// Both of a full update's streams end with the first; a partial update's
// with all three, "mvs".
constexpr quint32 kMarks[3] = {0x6d, 0x76, 0x73};

// The cache of refined tiles: indices run from 1, are written up to 64,999
// and then from 1 again, and are read up to 65,000. A cached tile is 64
// luminance coefficients, 15 Cr and 20 Cb, in zigzag order.
constexpr quint16 kCacheReadMax = 65000;
constexpr quint16 kCacheWriteMax = 64999;
constexpr int kCachedY = 64, kCachedCr = 15, kCachedCb = 20;

constexpr quint32 kWhite = 0xffffffff;
constexpr quint32 kBlack = 0xff000000;
// What a two-colour tile that reuses its colours draws before any were read.
constexpr quint32 kFirstPair[2] = {0xffffffff, 0xfffed5b5};

// JPEG's zigzag order: the natural position of the kth coefficient, with
// sixteen entries past the end for a run that reads beyond it.
constexpr int kNatural[80] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
    13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59,
    52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63,
};

/* A stream read most significant bit first. Past its end it reads zeros. */
class Bits
{
public:
    Bits(const uchar *data, qsizetype size, qsizetype at) : m_data(data), m_size(size), m_next(at) {}

    /* The next `n` bits, at most 24. */
    quint32 get(int n)
    {
        while (m_held < n) {
            const quint64 byte = m_next < m_size ? m_data[m_next] : 0;
            ++m_next;
            m_window = (m_window << 8 | byte) & 0xffffffffffffULL;
            m_held += 8;
        }
        m_held -= n;
        return quint32(m_window >> m_held) & ((1u << n) - 1);
    }
    bool bit() { return get(1) == 1; }
    /* Read further past the body than a stream in order is. */
    bool past() const { return m_next > m_size + kOverread; }

private:
    const uchar *m_data;
    qsizetype m_size;
    qsizetype m_next;   // the next byte to take into the window
    quint64 m_window = 0;
    int m_held = 0;
};

/*
 * How many tiles a command covers. A 0 bit is one tile; a 1 and four bits
 * below 15 are that many plus two; 1, 1111 and a number of up to three
 * bytes, least significant seven bits first, are that number plus 17.
 */
quint32 tilesCovered(Bits &bits)
{
    if (!bits.bit())
        return 1;
    const quint32 small = bits.get(4);
    if (small != 15)
        return small + 2;
    const quint32 first = bits.get(8);
    if (!(first & 0x80))
        return first + 17;
    const quint32 second = bits.get(8);
    if (!(second & 0x80))
        return (second << 7 | (first & 0x7f)) + 17;
    const quint32 third = bits.get(8);
    return (third << 14 | (second & 0x7f) << 7 | (first & 0x7f)) + 17;
}

/*
 * A signed number: k one bits and a zero, then for k 0 a bit, which when set
 * is 1 and is followed by a sign; for 1 and 2, 2 or 4 plus one or two bits;
 * and above, 8(k − 2) plus three bits, each followed by a sign, set for
 * negative. False for one that never ends.
 */
bool signedNumber(Bits &bits, qint16 *number)
{
    int ones = 0;
    while (bits.bit()) {
        if (++ones == 39)
            return false;
    }
    quint32 magnitude;
    if (ones == 0) {
        if (!bits.bit()) {
            *number = 0;
            return true;
        }
        magnitude = 1;
    } else if (ones == 1) {
        magnitude = 2 + bits.get(1);
    } else if (ones == 2) {
        magnitude = 4 + bits.get(2);
    } else {
        magnitude = 8 * quint32(ones - 2) + bits.get(3);
    }
    const bool negative = bits.bit();
    *number = qint16(negative ? -qint32(magnitude) : qint32(magnitude));
    return true;
}

/*
 * JPEG's standard Huffman table for luminance AC coefficients, which a
 * refinement's chrominance is coded with.
 */
constexpr quint8 kAcBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr quint8 kAcValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

struct Huffman
{
    int max[17];      // the largest code of each length, or -1 where there is none
    int offset[17];   // the first value of each length, less its first code

    Huffman()
    {
        int code = 0, first = 0;
        for (int length = 1; length <= 16; ++length) {
            const int count = kAcBits[length - 1];
            max[length] = -1;
            offset[length] = 0;
            if (count > 0) {
                offset[length] = first - code;
                code += count;
                max[length] = code - 1;
                first += count;
            }
            code <<= 1;
        }
    }

    /* The next symbol, or 0, an end of block, for a code no length has. */
    quint8 decode(Bits &bits) const
    {
        int code = 0;
        for (int length = 1; length <= 16; ++length) {
            code = code << 1 | int(bits.get(1));
            if (code <= max[length]) {
                const int at = code + offset[length];
                return at >= 0 && at < 162 ? kAcValues[at] : 0;
            }
        }
        bits.get(1);
        return 0;
    }
};

/* The three blocks of a tile, each in natural order. */
struct Block
{
    qint16 y[64] = {};
    qint16 cb[64] = {};
    qint16 cr[64] = {};
};

/*
 * What the last full update that coded a tile with coefficients left in it,
 * for a partial update to refine.
 */
struct Coded
{
    quint8 precision = 0;
    qint8 y[64] = {};   // in zigzag order, the DC first
    // The chrominance DCs, where the tile has them.
    bool hasChroma = false;
    qint8 cr = 0;
    qint8 cb = 0;
};

/* What is kept for each tile of the screen. */
struct Tile
{
    Coded coded;
    quint32 update = 0;   // the full update that last drew it
    // Where that update copied it from, if it did: the tile, and the place
    // in the picture.
    bool copied = false;
    qsizetype copiedTile = 0;
    qsizetype copiedAt = 0;
};

/* A refined tile, in zigzag order, as the cache keeps it. */
struct Refined
{
    qint8 y[kCachedY] = {};
    qint8 cr[kCachedCr] = {};
    qint8 cb[kCachedCb] = {};
    // The whole of each chroma block, of which the cache keeps the start.
    qint16 crBlock[64] = {};
    qint16 cbBlock[64] = {};
};

/* The tiles of a rectangle, in rows, left to right and top to bottom. */
struct Scan
{
    QRect rect;
    int dx = 0, dy = 0;     // where the tile is in the rectangle
    qsizetype index;        // of the tile among the screen's
    qsizetype row;          // the index its row starts at
    qsizetype across;

    Scan(const QRect &r, qsizetype tilesAcross)
        : rect(r), index((r.y() >> 3) * tilesAcross + (r.x() >> 3)), row(index), across(tilesAcross)
    {
    }
    int x() const { return rect.x() + dx; }
    int y() const { return rect.y() + dy; }
    /* The tile's top left pixel's place in a picture `width` wide. */
    qsizetype at(int width) const { return qsizetype(y()) * width + x(); }
    /* How much of the tile the rectangle covers. */
    int clippedWidth() const { return qMin(rect.width() - dx, 8); }
    int clippedHeight() const { return qMin(rect.height() - dy, 8); }
    /* On to the next tile; true if the one just done was the last. */
    bool advance()
    {
        dx += 8;
        if (dx < rect.width()) {
            ++index;
            return false;
        }
        dy += 8;
        if (dy >= rect.height())
            return true;
        dx = 0;
        row += across;
        index = row;
        return false;
    }
};

/* JPEG's YCbCr to RGB, with the reference library's fixed-point tables. */
quint32 ycc(int y, int cb, int cr)
{
    constexpr int half = 1 << 15;
    cb -= 128;
    cr -= 128;
    const int r = y + ((91881 * cr + half) >> 16);
    const int g = y + ((-46802 * cr + -22554 * cb + half) >> 16);
    const int b = y + ((116130 * cb + half) >> 16);
    return 0xff000000u | quint32(qBound(0, r, 255)) << 16 | quint32(qBound(0, g, 255)) << 8
           | quint32(qBound(0, b, 255));
}

/* A colour: 8-bit Y, then the top six bits of Cb and of Cr. */
quint32 colour(Bits &data)
{
    const int y = int(data.get(8));
    const int cb = int(data.get(6) << 2);
    const int cr = int(data.get(6) << 2);
    return ycc(y, cb, cr);
}

/*
 * A row mask, then for each row whose bit is clear a pixel mask: a set bit
 * takes `set`, a clear one `clear`, and a row whose bit is set is `set`
 * throughout.
 */
void masked(Bits &data, quint32 *tile, quint32 set, quint32 clear)
{
    const quint32 rows = data.get(8);
    for (int row = 0; row < 8; ++row) {
        if (rows & (0x80u >> row)) {
            std::fill_n(tile + row * 8, 8, set);
            continue;
        }
        const quint32 pixels = data.get(8);
        for (int column = 0; column < 8; ++column)
            tile[row * 8 + column] = pixels & (0x80u >> column) ? set : clear;
    }
}

/*
 * The JPEG reference library's integer inverse DCT (jidctint.c), with its
 * output range limit.
 */
void idct(const qint16 *coefficients, const int *quantization, quint8 *samples)
{
    constexpr int constBits = 13, pass1Bits = 2;
    constexpr qint64 f0_298631336 = 2446, f0_390180644 = 3196, f0_541196100 = 4433,
                     f0_765366865 = 6270, f0_899976223 = 7373, f1_175875602 = 9633,
                     f1_501321110 = 12299, f1_847759065 = 15137, f1_961570560 = 16069,
                     f2_053119869 = 16819, f2_562915447 = 20995, f3_072711026 = 25172;
    auto descale = [](qint64 x, int n) { return (x + (qint64(1) << (n - 1))) >> n; };
    // The even and odd halves of one row or column: its eight outputs before
    // they are descaled, in order.
    auto butterfly = [&](const qint64 *v, qint64 *out) {
        qint64 z2 = v[2], z3 = v[6];
        qint64 z1 = (z2 + z3) * f0_541196100;
        const qint64 tmp2 = z1 + z3 * -f1_847759065;
        const qint64 tmp3 = z1 + z2 * f0_765366865;
        const qint64 tmp0 = (v[0] + v[4]) * (qint64(1) << constBits);
        const qint64 tmp1 = (v[0] - v[4]) * (qint64(1) << constBits);
        const qint64 tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        qint64 t0 = v[7], t1 = v[5], t2 = v[3], t3 = v[1];
        z1 = t0 + t3;
        z2 = t1 + t2;
        z3 = t0 + t2;
        qint64 z4 = t1 + t3;
        const qint64 z5 = (z3 + z4) * f1_175875602;
        t0 *= f0_298631336;
        t1 *= f2_053119869;
        t2 *= f3_072711026;
        t3 *= f1_501321110;
        z1 *= -f0_899976223;
        z2 *= -f2_562915447;
        z3 = z3 * -f1_961570560 + z5;
        z4 = z4 * -f0_390180644 + z5;
        t0 += z1 + z3;
        t1 += z2 + z4;
        t2 += z2 + z3;
        t3 += z1 + z4;
        out[0] = tmp10 + t3;
        out[1] = tmp11 + t2;
        out[2] = tmp12 + t1;
        out[3] = tmp13 + t0;
        out[4] = tmp13 - t0;
        out[5] = tmp12 - t1;
        out[6] = tmp11 - t2;
        out[7] = tmp10 - t3;
    };
    // What a sample past 0–255 becomes: the library's table, read through
    // its ten-bit mask.
    auto limit = [](qint64 x) -> quint8 {
        const int i = int(x) & 1023;
        if (i < 128)
            return quint8(i + 128);
        if (i < 512)
            return 255;
        if (i < 896)
            return 0;
        return quint8(i - 896);
    };

    qint32 workspace[64];
    for (int column = 0; column < 8; ++column) {
        qint64 v[8];
        bool flat = true;
        for (int row = 0; row < 8; ++row) {
            v[row] = qint64(coefficients[row * 8 + column]) * quantization[row * 8 + column];
            if (row > 0 && coefficients[row * 8 + column] != 0)
                flat = false;
        }
        if (flat) {
            const qint32 dc = qint32(v[0] * (1 << pass1Bits));
            for (int row = 0; row < 8; ++row)
                workspace[row * 8 + column] = dc;
            continue;
        }
        qint64 out[8];
        butterfly(v, out);
        for (int row = 0; row < 8; ++row)
            workspace[row * 8 + column] = qint32(descale(out[row], constBits - pass1Bits));
    }
    for (int row = 0; row < 8; ++row) {
        const qint32 *line = workspace + row * 8;
        quint8 *out = samples + row * 8;
        bool flat = true;
        for (int i = 1; i < 8; ++i)
            flat = flat && line[i] == 0;
        if (flat) {
            std::fill_n(out, 8, limit(descale(line[0], pass1Bits + 3)));
            continue;
        }
        qint64 v[8], values[8];
        for (int i = 0; i < 8; ++i)
            v[i] = line[i];
        butterfly(v, values);
        for (int i = 0; i < 8; ++i)
            out[i] = limit(descale(values[i], constBits + pass1Bits + 3));
    }
}

/* What a DCT tile leaves of itself for its tile's record. */
enum class Saved {
    Coded,
    Last,      // the previous DCT tile's record over again, for a tile that copies it
    Nothing,
};

/*
 * A DCT tile of a full update: the copy bit, the chroma and precision bits,
 * the chroma's DCs where they are sent, Y's DC, then Y's AC coefficients.
 * False for coefficients that do not read.
 */
bool expandBlock(Bits &data, const Block &previous, const quint8 *precisions, bool hasLast,
                 Block *block, Saved *saved, Coded *coded)
{
    *block = Block();
    if (data.bit()) {
        if (!hasLast) {
            *saved = Saved::Nothing;
            return true;
        }
        std::memcpy(block->y, previous.y, sizeof block->y);
        block->cb[0] = previous.cb[0];
        block->cr[0] = previous.cr[0];
        *saved = Saved::Last;
        return true;
    }
    const bool keep = data.bit();
    const quint8 precision = data.bit() ? precisions[1] : precisions[0];
    qint16 number = 0;
    if (keep) {
        block->cb[0] = previous.cb[0];
        block->cr[0] = previous.cr[0];
    } else {
        // Half the previous DC, rounded towards zero, less the number, doubled.
        if (!signedNumber(data, &number))
            return false;
        block->cb[0] = qint16((previous.cb[0] / 2 - number) * 2);
        if (!signedNumber(data, &number))
            return false;
        block->cr[0] = qint16((previous.cr[0] / 2 - number) * 2);
    }
    if (!signedNumber(data, &number))
        return false;
    block->y[0] = qint16(previous.y[0] - number);

    const bool fine = precision >= 15;
    int k = 1;
    bool kept = true;
    while (k < 64) {
        const bool early = k < 6;
        const bool below = k < precision;
        // A coefficient is sent short of its low bits: a unit is 1 shifted
        // left by that many.
        const int shift = fine ? (below ? 1 : 3) : (below ? 3 : 4);
        if (data.bit()) {
            int ones = 0;
            bool stop = false;
            while (data.bit()) {
                ++ones;
                if (early && ones == 33)
                    return false;
                if (!early && ones == 19) {
                    // The viewer keeps what it has and leaves the record as it was.
                    stop = true;
                    break;
                }
            }
            if (stop) {
                kept = false;
                break;
            }
            int magnitude;
            if (early)
                magnitude = ones < 4 ? 2 + 4 * ones + int(data.get(2)) : 18 + 8 * (ones - 4) + int(data.get(3));
            else if (ones == 0)
                magnitude = 2 + int(data.get(1));
            else if (ones == 1)
                magnitude = 4 + int(data.get(2));
            else
                magnitude = 8 * (ones - 1) + int(data.get(3));
            const int value = data.bit() ? -magnitude : magnitude;
            block->y[kNatural[k]] = qint16(value * (1 << shift));
            ++k;
            continue;
        }
        const qint16 unit = qint16(1 << shift);
        const quint32 code = data.get(2);
        if (code == 0) {
            ++k;
        } else if (code == 2) {
            block->y[kNatural[k]] = unit;
            ++k;
        } else if (code == 3) {
            block->y[kNatural[k]] = qint16(-unit);
            ++k;
        } else {
            if (!data.bit())
                break;   // the block ends
            const quint32 r = data.get(2);
            quint32 run = r + 3;
            if (r == 3) {
                for (;;) {
                    const quint32 more = data.get(3);
                    run += more;
                    if (more != 7 || k + int(run & 0xff) >= 64)
                        break;
                }
                if (k + int(run & 0xff) > 63)
                    return false;
                k += int(run & 0xff);
            } else {
                k += int(run);
            }
        }
    }
    if (!kept) {
        *saved = Saved::Nothing;
        return true;
    }
    *coded = Coded();
    coded->precision = precision;
    for (int i = 0; i < 64; ++i)
        coded->y[i] = qint8(block->y[kNatural[i]]);
    coded->hasChroma = true;
    coded->cr = qint8(block->cr[0]);
    coded->cb = qint8(block->cb[0]);
    *saved = Saved::Coded;
    return true;
}

/*
 * A partial update's code 1: the bits the full update left out of a tile's Y
 * coefficients, and its chrominance as JPEG codes it. False, with why, for a
 * tile that cannot be refined.
 */
bool refine(Bits &bits, const Coded &coded, int crCount, int cbCount, const Huffman &huffman,
            Refined *refined, const char **why)
{
    const int precision = coded.precision;
    const int count = int(bits.get(6)) + 1;
    bool ok = true;
    auto number = [&]() {
        qint16 value = 0;
        ok = signedNumber(bits, &value) && ok;
        return qint8(value);
    };
    // Moved further from zero by `n` more bits.
    auto further = [&bits](qint8 coefficient, int n) {
        const int more = qint8(bits.get(n));
        return qint8(coefficient + (coefficient < 0 ? -more : more));
    };
    qint8 *y = refined->y;
    y[0] = coded.y[0];
    for (int k = 1; ok && k < qMin(count, precision); ++k) {
        const qint8 coefficient = coded.y[k];
        if (precision < 15) {
            y[k] = coefficient == 0 ? number() : further(coefficient, 3);
        } else if (coefficient == 0) {
            // Below the precision of a fine tile a zero is near enough: a
            // bit, and when it is set a sign for 1 or −1.
            y[k] = !bits.bit() ? 0 : bits.bit() ? -1 : 1;
        } else {
            y[k] = further(coefficient, 1);
        }
    }
    for (int k = qMax(precision, 1); ok && k < count; ++k) {
        const qint8 coefficient = coded.y[k];
        y[k] = coefficient == 0 ? number() : further(coefficient, precision < 15 ? 4 : 3);
    }
    if (!ok) {
        *why = QT_TRANSLATE_NOOP("AppleAdaptive", "a tile's coefficients do not read");
        return false;
    }
    if (!coded.hasChroma) {
        *why = QT_TRANSLATE_NOOP("AppleAdaptive", "a refinement of a tile the last full update did not code");
        return false;
    }

    auto chroma = [&](qint8 dc, int limit, qint16 *block) {
        const bool bit = bits.bit();
        if (dc == 0) {
            block[0] = !bit ? 0 : bits.bit() ? -1 : 1;
        } else {
            const int step = bit ? 1 : 0;
            block[0] = qint8(dc + (dc < 0 ? -step : step));
        }
        int k = 0;
        while (k < limit) {
            const quint8 symbol = huffman.decode(bits);
            const int run = symbol >> 4, size = symbol & 15;
            if (size == 0) {
                if (run != 15)
                    break;
                k += 15;
            } else {
                k += run;
                int value = int(bits.get(size));
                if (value < 1 << (size - 1))
                    value += 1 - (1 << size);
                block[kNatural[k + 1]] = qint16(value);
            }
            ++k;
        }
    };
    chroma(coded.cr, crCount, refined->crBlock);
    chroma(coded.cb, cbCount, refined->cbBlock);
    for (int k = 0; k < kCachedCr; ++k)
        refined->cr[k] = qint8(refined->crBlock[kNatural[k]]);
    for (int k = 0; k < kCachedCb; ++k)
        refined->cb[k] = qint8(refined->cbBlock[kNatural[k]]);
    return true;
}

} // namespace

struct AppleAdaptive::Private
{
    Q_DECLARE_TR_FUNCTIONS(AppleAdaptive)

public:
    int width = 0;
    int height = 0;
    quint32 *pixels = nullptr;   // the picture, for the length of one decode()
    qsizetype tilesAcross = 0;
    std::vector<Tile> tiles;
    bool hasTables = false;
    int luminance[64];           // how message 2 dequantizes, in natural order
    int chrominance[64];
    Huffman huffman;
    quint32 updates = 0;         // full updates read so far
    // Cached tiles by index, allocated at the first.
    std::vector<std::array<qint8, kCachedY + kCachedCr + kCachedCb>> cache;
    quint32 cached = 0;
    quint16 written = 0;
    // The index a tile was last drawn from the cache by, which "the next"
    // counts from.
    quint16 drawn = 0;

    bool full(const QRect &rect, const uchar *body, qsizetype size, QString *error);
    bool partial(const QRect &rect, const uchar *body, qsizetype size, QString *error);

    /* The three blocks dequantized, inverted and converted to pixels. */
    void inverse(const Block &block, quint32 *tile) const
    {
        quint8 y[64], cb[64], cr[64];
        idct(block.y, luminance, y);
        idct(block.cb, chrominance, cb);
        idct(block.cr, chrominance, cr);
        for (int i = 0; i < 64; ++i)
            tile[i] = ycc(y[i], cb[i], cr[i]);
    }

    /* The tile under `scan` as the picture has it, black past the rectangle's edge. */
    void readTile(const Scan &scan, quint32 *tile) const
    {
        std::fill_n(tile, 64, kBlack);
        const int w = scan.clippedWidth(), h = scan.clippedHeight();
        for (int row = 0; row < h; ++row)
            std::memcpy(tile + row * 8, pixels + qsizetype(scan.y() + row) * width + scan.x(),
                        size_t(w) * 4);
    }

    /* `tile` onto the picture, as much of it as the rectangle covers. */
    void writeTile(const Scan &scan, const quint32 *tile)
    {
        const int w = scan.clippedWidth(), h = scan.clippedHeight();
        for (int row = 0; row < h; ++row)
            std::memcpy(pixels + qsizetype(scan.y() + row) * width + scan.x(), tile + row * 8,
                        size_t(w) * 4);
    }

    /*
     * Eight rows of eight pixels from `at`, a pixel's place in the picture
     * read as one run of rows, into `tile`. A row that starts before the
     * picture is left as it was, and so is a pixel after its end.
     */
    void copyInto(quint32 *tile, qsizetype at) const
    {
        const qsizetype end = qsizetype(width) * height;
        for (int row = 0; row < 8; ++row) {
            const qsizetype start = at + qsizetype(row) * width;
            if (start < 0)
                continue;
            for (int column = 0; column < 8; ++column) {
                if (start + column < end)
                    tile[row * 8 + column] = pixels[start + column];
            }
        }
    }

    /*
     * The cached tile at `entry`, where there is one, which is then the one
     * "the next" counts from.
     */
    void drawCached(quint16 entry, quint32 *tile)
    {
        if (entry == 0 || entry > kCacheReadMax || entry > cached || cache.empty())
            return;
        const auto &kept = cache[entry];
        Block block;
        for (int k = 0; k < kCachedY; ++k)
            block.y[kNatural[k]] = kept[k];
        for (int k = 0; k < kCachedCr; ++k)
            block.cr[kNatural[k]] = kept[kCachedY + k];
        for (int k = 0; k < kCachedCb; ++k)
            block.cb[kNatural[k]] = kept[kCachedY + kCachedCr + k];
        inverse(block, tile);
        drawn = entry;
    }

    void cacheEntry(const Refined &entry)
    {
        if (cache.empty())
            cache.resize(size_t(kCacheReadMax) + 1);
        ++cached;
        written = written == kCacheWriteMax ? 1 : written + 1;
        auto &slot = cache[written];
        std::memcpy(slot.data(), entry.y, kCachedY);
        std::memcpy(slot.data() + kCachedY, entry.cr, kCachedCr);
        std::memcpy(slot.data() + kCachedY + kCachedCr, entry.cb, kCachedCb);
    }
};

AppleAdaptive::AppleAdaptive() : d(std::make_unique<Private>()) {}

AppleAdaptive::~AppleAdaptive() = default;

void AppleAdaptive::resize(int width, int height)
{
    // The tables and the cache outlive a size, as the Mac's do.
    d->width = width;
    d->height = height;
    d->tilesAcross = (width + 7) / 8;
    d->tiles.assign(size_t(d->tilesAcross) * size_t((height + 7) / 8), Tile());
}

bool AppleAdaptive::decode(const QRect &rect, const uchar *body, qsizetype size, QImage *image,
                           bool *drew, QString *error)
{
    *drew = false;
    if (size < 1) {
        *error = Private::tr("an empty update");
        return false;
    }
    const int kind = body[0];
    if (kind == kTables) {
        if (size != kTablesSize) {
            *error = Private::tr("quantization tables in %1 bytes").arg(size);
            return false;
        }
        for (int i = 0; i < 64; ++i) {
            d->luminance[i] = body[1 + i];
            d->chrominance[i] = body[65 + i];
        }
        d->hasTables = true;
        return true;
    }
    if (kind != kFull && kind != kPartial) {
        *error = Private::tr("an update of kind %1").arg(kind);
        return false;
    }
    const bool fits = image->format() == QImage::Format_RGB32 && image->width() == d->width
                      && image->height() == d->height
                      && image->bytesPerLine() == qsizetype(d->width) * 4
                      && rect.x() >= 0 && rect.y() >= 0 && rect.width() >= 0 && rect.height() >= 0
                      && rect.x() < d->width && rect.x() + rect.width() <= d->width
                      && rect.y() < d->height && rect.y() + rect.height() <= d->height;
    if (!fits) {
        *error = Private::tr("a rectangle outside the screen");
        return false;
    }
    if (!d->hasTables) {
        *error = Private::tr("a picture before the quantization tables");
        return false;
    }
    d->pixels = reinterpret_cast<quint32 *>(image->bits());
    const bool ok = kind == kFull ? d->full(rect, body, size, error) : d->partial(rect, body, size, error);
    d->pixels = nullptr;
    *drew = true;   // as far as it got
    return ok;
}

bool AppleAdaptive::Private::full(const QRect &rect, const uchar *body, qsizetype size, QString *error)
{
    if (size < 6) {
        *error = tr("a full update in %1 bytes").arg(size);
        return false;
    }
    const quint8 precisions[2] = {body[1], body[2]};
    const qsizetype offset = qsizetype(body[3]) << 16 | qsizetype(body[4]) << 8 | body[5];
    if (offset > size) {
        *error = tr("a data stream past the update's end");
        return false;
    }
    Bits commands(body, size, 6);
    Bits data(body, size, offset);
    commands.get(1);
    ++updates;

    Scan scan(rect, tilesAcross);
    quint32 single = kBlack;
    quint32 pair[2] = {kFirstPair[0], kFirstPair[1]};
    Block previous;
    qsizetype lastCoded = -1;
    // Where the tile before this one is, which "a copy of the previous tile"
    // copies, and at the update's first tile the tile itself.
    qsizetype before = scan.at(width);
    for (;;) {
        if (commands.past()) {
            *error = tr("an update that ran past its end");
            return false;
        }
        const quint32 command = commands.get(3);
        const quint32 covered = tilesCovered(commands);
        for (quint32 i = 0; i < covered; ++i) {
            const qsizetype index = scan.index;
            const qsizetype here = scan.at(width);
            if (index < 0 || size_t(index) >= tiles.size()) {
                *error = tr("a rectangle outside the screen");
                return false;
            }
            tiles[index].update = updates;
            tiles[index].copied = false;
            quint32 tile[64];
            readTile(scan, tile);
            switch (command) {
            case 0:
                std::fill_n(tile, 64, kWhite);
                break;
            case 1:   // the previous tile
                if (index >= 1) {
                    tiles[index].copied = true;
                    tiles[index].copiedTile = index - 1;
                    tiles[index].copiedAt = before;
                }
                copyInto(tile, before);
                break;
            case 2: {   // the tile above
                const qsizetype above = here - 8 * qsizetype(width);
                if (index >= tilesAcross) {
                    tiles[index].copied = true;
                    tiles[index].copiedTile = index - tilesAcross;
                    tiles[index].copiedAt = qMax<qsizetype>(above, 0);
                }
                copyInto(tile, above);
                break;
            }
            case 3:   // black and white
                masked(data, tile, kWhite, kBlack);
                break;
            case 4: {   // one colour or two, new or the last read
                const bool two = data.bit();
                const bool reuse = data.bit();
                if (two) {
                    if (!reuse) {
                        pair[0] = colour(data);
                        pair[1] = colour(data);
                    }
                    masked(data, tile, pair[0], pair[1]);
                } else {
                    if (!reuse)
                        single = colour(data);
                    std::fill_n(tile, 64, single);
                }
                break;
            }
            case 5: {   // coefficients
                const bool hasLast = lastCoded >= 0;
                const Tile last = hasLast ? tiles[lastCoded] : Tile();
                Block block;
                Saved saved;
                Coded coded;
                if (!expandBlock(data, previous, precisions, hasLast, &block, &saved, &coded)) {
                    *error = tr("a tile's coefficients do not read");
                    return false;
                }
                if (saved == Saved::Coded)
                    tiles[index].coded = coded;
                else if (saved == Saved::Last)
                    tiles[index] = last;
                previous = block;
                lastCoded = index;
                inverse(block, tile);
                break;
            }
            case 6:   // a cached tile, by its index
                drawCached(quint16(data.get(16)), tile);
                break;
            default:   // the cached tile after the last
                drawCached(quint16(drawn + 1), tile);
                break;
            }
            writeTile(scan, tile);
            before = here;
            if (scan.advance()) {
                for (Bits *stream : {&commands, &data}) {
                    if (const quint32 got = stream->get(8); got != kMarks[0]) {
                        *error = tr("an update that ended on 0x%1").arg(got, 2, 16, QLatin1Char('0'));
                        return false;
                    }
                }
                return true;
            }
        }
        if (data.past()) {
            *error = tr("an update that ran past its end");
            return false;
        }
    }
}

bool AppleAdaptive::Private::partial(const QRect &rect, const uchar *body, qsizetype size, QString *error)
{
    if (size < 3) {
        *error = tr("a partial update in %1 bytes").arg(size);
        return false;
    }
    // How many chroma coefficients a refined tile gets.
    const int crCount = qMin<int>(body[1], 64);
    const int cbCount = qMin<int>(body[2], 64);
    Bits bits(body, size, 3);
    Scan scan(rect, tilesAcross);
    for (;;) {
        const qsizetype index = scan.index;
        if (index < 0 || size_t(index) >= tiles.size()) {
            *error = tr("a rectangle outside the screen");
            return false;
        }
        const Tile &record = tiles[index];
        quint32 tile[64];
        switch (bits.get(2)) {
        case 0:   // as it is
            break;
        case 1: {   // refined
            readTile(scan, tile);
            Refined entry;
            const char *why = nullptr;
            if (!refine(bits, record.coded, crCount, cbCount, huffman, &entry, &why)) {
                *error = tr(why);
                return false;
            }
            Block block;
            for (int k = 0; k < kCachedY; ++k)
                block.y[kNatural[k]] = entry.y[k];
            std::memcpy(block.cr, entry.crBlock, sizeof block.cr);
            std::memcpy(block.cb, entry.cbBlock, sizeof block.cb);
            inverse(block, tile);
            cacheEntry(entry);
            writeTile(scan, tile);
            break;
        }
        case 2:
            // The copy the full update made, while the tile it copied is
            // still that update's.
            if (record.copied && size_t(record.copiedTile) < tiles.size()
                && tiles[record.copiedTile].update == record.update) {
                readTile(scan, tile);
                copyInto(tile, record.copiedAt);
                writeTile(scan, tile);
            }
            break;
        default: {   // a cached tile: the next, or by its index
            const quint16 entry = bits.bit() ? quint16(drawn + 1) : quint16(bits.get(16));
            readTile(scan, tile);
            drawCached(entry, tile);
            writeTile(scan, tile);
            break;
        }
        }
        if (scan.advance()) {
            for (quint32 want : kMarks) {
                if (const quint32 got = bits.get(8); got != want) {
                    *error = tr("an update that ended on 0x%1").arg(got, 2, 16, QLatin1Char('0'));
                    return false;
                }
            }
            return true;
        }
        if (bits.past()) {
            *error = tr("an update that ran past its end");
            return false;
        }
    }
}
