/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QImage>
#include <QRect>
#include <QString>

#include <memory>

/*
 * A Mac's Adaptive picture: Screen Sharing's encoding 0x3f3 in RFB 003.889
 * (docs/apple-rfb-889.md). The screen comes in 8×8 tiles, each white, a copy
 * of a neighbour, one or two colours, or JPEG-like coefficients sent coarse
 * and refined by later updates; so a rectangle draws on what the ones before
 * it left, and the decoder keeps what each tile was last coded as.
 *
 * One decoder serves one connection: the Mac codes every update against what
 * its viewer made of the last, and a rectangle that fails leaves the two
 * apart for good.
 */
class AppleAdaptive
{
public:
    AppleAdaptive();
    ~AppleAdaptive();

    /* The screen's new size: no tile is coded any more. */
    void resize(int width, int height);

    /*
     * One rectangle's body (what follows its length) onto `image`, which is
     * the whole screen in RGB32, of the size given to resize(). `drew` says
     * pixels changed: the quantization tables are a rectangle too. False,
     * with why, for one that does not decode.
     */
    bool decode(const QRect &rect, const uchar *body, qsizetype size, QImage *image,
                bool *drew, QString *error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
