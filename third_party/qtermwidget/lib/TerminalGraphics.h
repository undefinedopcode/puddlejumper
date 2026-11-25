/*
 * puddlejumper: images in the terminal, by kitty's graphics protocol.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#ifndef TERMINALGRAPHICS_H
#define TERMINALGRAPHICS_H

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>
#include <QSet>
#include <QSize>

namespace Konsole
{

/*
 * Where an image shows: anchored to a cell, which moves with the text as it
 * scrolls (into the history, too) and goes when that text does.
 */
struct GraphicsPlacement
{
    quint32 imageId = 0;
    quint32 placementId = 0;
    int row = 0;            // the anchor cell's line, from the screen's top (negative: in the history)
    int column = 0;
    QRect source;           // the part of the image shown, in its pixels
    qreal offsetX = 0;      // from the anchor cell's corner, in cells
    qreal offsetY = 0;
    qreal width = 0;        // in cells
    qreal height = 0;
    int columns = 1;        // the cells it covers, from the anchor
    int rows = 1;
    qint32 z = 0;           // below zero: under the text
    quint64 serial = 0;     // order of placing
    bool isVirtual = false; // U=1: shown only through placeholder characters
};

/*
 * The graphics of one screen (the normal and alternate screens each have
 * their own, as in kitty): images sent by programs, and where they show.
 *
 *   ESC _ G <key=value,...> ; <base64 payload> ESC \
 *
 * Spec: https://sw.kovidgoyal.net/kitty/graphics-protocol/. Images come in
 * the escape code itself (in chunks), from a file or from shared memory, as
 * PNG, RGB, RGBA or indexed colour, zlib-compressed or not. Images can be
 * animated: frames, composed from parts of others, played by the terminal
 * (see animate()) or stepped through by the program. Not done: relative
 * placements.
 */
class TerminalGraphics
{
public:
    // Kitty allows 320 MB a screen.
    static constexpr qint64 kQuota = 320LL * 1024 * 1024;

    // What a command needs to know of the terminal.
    struct Context
    {
        int cursorRow = 0;
        int cursorColumn = 0;
        int lines = 24;
        int columns = 80;
        QSize cellSize;      // in the pixels images are measured in
    };

    // What the emulation is to do afterwards.
    struct Result
    {
        QByteArray reply;    // to send back, escape code and all (empty: none)
        bool changed = false;
        bool moveCursor = false;
        int cursorColumns = 0;   // to move right
        int cursorRows = 0;      // to move down (scrolling as need be)
    };

    /* One command, the bytes between "ESC _ G" and the String Terminator. */
    Result command(const QByteArray &body, const Context &context);

    // The screen's text moved; placements follow it.

    /* Lines first..last (screen rows) moved by delta; those written over
     * (and not moved themselves) lose what was anchored there. */
    void linesMoved(int first, int last, int delta);
    /* Screen line 0 went into the history, which now has historyLines. */
    void historyLineAdded(int historyLines);
    /* The screen was cleared (ED 2): what shows on it goes. */
    void screenCleared(int lines);
    /* The history was cleared, or now holds historyLines. */
    void historyCleared();
    void historyResized(int historyLines);
    /* Everything goes: a reset. */
    void clear();

    bool isEmpty() const { return m_placements.isEmpty(); }
    QList<GraphicsPlacement> placements() const { return m_placements; }
    /* The image's frame showing now. An indexed-colour image (f=8, f=16) is
     * Format_Grayscale16 holding the palette index in the low byte and the
     * alpha in the high one; colourize() makes it a picture. */
    QImage image(quint32 id) const;
    static bool isIndexed(const QImage &image) { return image.format() == QImage::Format_Grayscale16; }
    /* An indexed image in the colours given: 0-247 the 256-colour table's,
     * 248 the default background, 249 the default foreground, and so on. */
    static QImage colourize(const QImage &indexed, const QRgb *palette256);

    /* Moves the running animations of the images `shown` on to the frame due
     * now; returns the milliseconds until one is next due (-1: none). Others
     * wait, and catch up when shown again. */
    qint64 animate(const QSet<quint32> &shown);
    /* Changes with every placement made, moved or gone. */
    quint64 generation() const { return m_generation; }

private:
    struct Frame
    {
        QImage image;
        int gap = 0;            // ms it shows for when playing; 0: skipped over
    };

    struct Image
    {
        QList<Frame> frames;    // the first is the image itself (the root frame)
        quint32 number = 0;
        quint64 serial = 0;     // order of arriving
        // Animation
        enum State { Stopped, Loading, Running } state = Stopped;
        int current = 0;        // the frame showing
        qint64 shownAt = 0;     // when it began to (clock())
        int loops = 0;          // v: 0 or 1 forever, else loops - 1 times
        int loopsDone = 0;
        qint64 bytes() const;
    };

    struct Command
    {
        char action = 't';      // a
        char medium = 'd';      // t
        char compression = 0;   // o
        char deleteWhat = 'a';  // d
        int format = 32;        // f
        int quiet = 0;          // q
        bool more = false;      // m
        quint32 id = 0;         // i
        quint32 number = 0;     // I
        quint32 placementId = 0;    // p
        int width = 0, height = 0;  // s, v: the data's size in pixels (a=a: state, loops)
        int dataSize = 0;       // S
        qint64 dataOffset = 0;  // O
        int x = 0, y = 0, w = 0, h = 0;     // the source rectangle
        int cellX = 0, cellY = 0;           // X, Y: offset in the cell, in pixels
        int columns = 0, rows = 0;          // c, r
        quint32 keyX = 0, keyY = 0;         // X, Y and c, r as given: in animation
        quint32 keyC = 0, keyR = 0;         // they are flags, colours and frames
        qint32 z = 0;
        bool noCursorMove = false;          // C=1
        bool unicodePlaceholder = false;    // U=1
        bool relative = false;              // P, Q, H or V given
        bool hasId = false, hasNumber = false, hasPlacementId = false, hasZ = false;
        bool hasAction = false;
        char featureAction = 0;             // e: a feature query about this action
    };

    static bool parse(const QByteArray &control, Command &cmd);
    Result transmit(const Command &cmd, const QByteArray &data, const Context &context);
    Result put(const Command &cmd, quint32 id, const Context &context);
    Result remove(const Command &cmd, const Context &context);
    Result frame(const Command &cmd, const QByteArray &data);
    Result animation(const Command &cmd);
    Result compose(const Command &cmd);
    QByteArray decode(const Command &cmd, const QByteArray &data, QImage &image) const;
    /* The data from where t= says: the payload itself, a file or shared
     * memory named by it. */
    static QByteArray read(const Command &cmd, const QByteArray &payload, QByteArray &data);
    /* Make room for `bytes` more, dropping the oldest images (unplaced first),
     * but not `keep`. */
    void makeRoom(qint64 bytes, quint32 keep = 0);
    bool isPlaced(quint32 id) const;
    static qint64 clock();
    void dropImage(quint32 id);
    quint32 newestWithNumber(quint32 number) const;
    quint32 freeId() const;
    /* Remove the placements `match` picks; with `freeImages`, also the images
     * left with none. */
    template <typename Match>
    void removePlacements(Match match, bool freeImages);
    void prune(int historyLines);
    static QByteArray reply(const Command &cmd, quint32 id, const QByteArray &message);
    static QByteArray featureReply(const Command &cmd, const QByteArray &control);

    QHash<quint32, Image> m_images;
    QList<GraphicsPlacement> m_placements;
    quint64 m_serial = 0;
    quint64 m_generation = 0;

    // An image coming in chunks: its first chunk's command, and the data so far.
    bool m_loading = false;
    Command m_load;
    QByteArray m_loadData;
};

}

#endif // TERMINALGRAPHICS_H
