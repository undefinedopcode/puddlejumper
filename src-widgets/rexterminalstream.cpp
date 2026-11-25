/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexterminalstream.h"

#include <QJsonDocument>
#include <QtEndian>

namespace {
const QByteArray ST = QByteArrayLiteral("\033\\");
}

RexTerminalStream::RexTerminalStream(quint64 nonce, bool resumed, quint64 offset)
    : m_prefix("\033_rex1;" + QByteArray::number(nonce) + ';')
    , m_haveSnapshot(resumed)
    , m_received(offset)
{
}

bool RexTerminalStream::parseWelcome(QByteArrayView welcome, quint64 *nonce, quint32 *graceMs, QByteArray *token)
{
    if (welcome.size() != 32 || quint8(welcome[0]) != 0x81) {
        if (nonce)
            *nonce = 0;
        return false;
    }
    const auto *p = reinterpret_cast<const uchar *>(welcome.data());
    if (token)
        *token = welcome.sliced(1, 16).toByteArray();
    if (graceMs)
        *graceMs = qFromLittleEndian<quint32>(p + 20);
    if (nonce)
        *nonce = qFromLittleEndian<quint64>(p + 24);
    return true;
}

QList<RexTerminalStream::Event> RexTerminalStream::feed(QByteArrayView bytes)
{
    QList<Event> out;
    if (failed())
        return out;
    m_received += quint64(bytes.size());
    m_buffer += bytes;

    if (!m_haveSnapshot) {
        const qsizetype end = GhosttySnapshot::scan(m_buffer);
        if (end < 0) {
            m_error = QStringLiteral("the stream doesn't start with a Ghostty snapshot");
            return out;
        }
        if (end == 0)
            return out;  // more to come
        GhosttySnapshot::Result r = GhosttySnapshot::decode(QByteArrayView(m_buffer).first(end));
        if (!r.snapshot) {
            m_error = QStringLiteral("snapshot: ") + r.error;
            return out;
        }
        Event e;
        e.kind = Event::Snapshot;
        e.snapshot = std::move(r.snapshot);
        out.append(std::move(e));
        m_buffer.remove(0, end);
        m_haveSnapshot = true;
    }
    takeOutput(out);
    return out;
}

void RexTerminalStream::takeOutput(QList<Event> &out)
{
    auto output = [&](qsizetype n) {
        if (n <= 0)
            return;
        if (!out.isEmpty() && out.last().kind == Event::Output) {
            out.last().bytes += QByteArrayView(m_buffer).first(n);
        } else {
            Event e;
            e.bytes = m_buffer.first(n);
            out.append(std::move(e));
        }
        m_buffer.remove(0, n);
    };

    for (;;) {
        const qsizetype at = m_buffer.indexOf(m_prefix);
        if (at < 0) {
            // Hold back a tail that could be the start of a marker.
            qsizetype keep = std::min(m_buffer.size(), m_prefix.size() - 1);
            while (keep > 0 && !m_prefix.startsWith(QByteArrayView(m_buffer).last(keep)))
                --keep;
            output(m_buffer.size() - keep);
            return;
        }
        output(at);
        const qsizetype end = m_buffer.indexOf(ST, m_prefix.size());
        if (end < 0) {
            if (m_buffer.size() <= kMaxMarker)
                return;  // its end is still to come
            // Never ended: it was output that looked like one.
            output(m_prefix.size());
            continue;
        }
        const QByteArray json = m_buffer.sliced(m_prefix.size(), end - m_prefix.size());
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
        Event e;
        e.kind = Event::Marker;
        if (err.error == QJsonParseError::NoError && doc.isObject())
            e.marker = doc.object();
        else
            e.marker.insert(QStringLiteral("type"), QStringLiteral("invalid"));
        out.append(std::move(e));
        m_buffer.remove(0, end + ST.size());
    }
}
