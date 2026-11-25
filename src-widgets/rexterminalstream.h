/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "ghosttysnapshot.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>

/*
 * The payload of a Rex terminal data stream (rex.data.terminal.v1), after the
 * handshake, taken apart. A fresh stream is
 *
 *   Ghostty snapshot (GHOSTSNP ... FINISH), then terminal output with
 *   markers between escape sequences:  ESC _ rex1;<nonce>;<JSON> ESC \
 *
 * where the nonce is the one in the stream's WELCOME (bytes 24-31, u64 LE,
 * written in decimal). The server sends a resize marker right after the
 * snapshot, then theme and exit markers when they apply, and a resize marker
 * whenever the terminal's size changes. WebSocket messages don't delimit any
 * of it, so bytes go in as they come, in pieces of any size.
 *
 * Only markers with this stream's nonce are taken out: a program printing
 * something like one can't fake it. Everything else, other APC sequences
 * included, is terminal output.
 *
 * A resumed stream has no snapshot: it carries on from the byte count the
 * client gave, which received() keeps (snapshot and markers included).
 */
class RexTerminalStream
{
public:
    struct Event
    {
        enum Kind { Snapshot, Output, Marker };
        Kind kind = Output;
        QByteArray bytes;                                // Output
        std::optional<GhosttySnapshot::Snapshot> snapshot;  // Snapshot
        QJsonObject marker;                              // Marker
        QString type() const { return marker.value(QLatin1String("type")).toString(); }
    };

    // The nonce from the WELCOME message.
    explicit RexTerminalStream(quint64 nonce, bool resumed = false);

    // The nonce in a 32-byte WELCOME (81, token, reserved, grace, nonce); 0
    // and false if it isn't one.
    static bool parseWelcome(QByteArrayView welcome, quint64 *nonce, quint32 *graceMs = nullptr,
                             QByteArray *token = nullptr);

    // Takes the next bytes of the stream; returns what they complete, in
    // order. After an error nothing more comes out.
    QList<Event> feed(QByteArrayView bytes);

    bool failed() const { return !m_error.isEmpty(); }
    QString error() const { return m_error; }
    // Payload bytes taken so far: what a RESUME asks to carry on from.
    quint64 received() const { return m_received; }

    // A marker is held back until its end comes; one this long without
    // ending wasn't a marker after all.
    static constexpr qsizetype kMaxMarker = 64 * 1024;

private:
    void takeOutput(QList<Event> &out);

    QByteArray m_prefix;    // ESC _ rex1;<nonce>;
    bool m_haveSnapshot;
    QByteArray m_buffer;    // not yet passed on
    quint64 m_received = 0;
    QString m_error;
};
