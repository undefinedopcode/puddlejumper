/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

/*
 * A real Ghostty terminal, from libghostty-vt, for the dev tools: bytes in,
 * snapshots and state out. Built only when CMake is given PJ_GHOSTTY_VT (a
 * libghostty-vt build: `zig build -Demit-lib-vt` in a Ghostty checkout,
 * then -DPJ_GHOSTTY_VT=<checkout>/zig-out).
 */

#include <ghostty/vt.h>

#include <QByteArray>

#include <cstdint>

class GhosttyVt
{
public:
    GhosttyVt(quint16 columns, quint16 rows)
    {
        if (ghostty_terminal_new(nullptr, &m_term, columns, rows) != GHOSTTY_SUCCESS) {
            m_term = nullptr;
            return;
        }
        // Cells of 10x20 pixels (QtermProbe's too), so images take the
        // same room in both.
        ghostty_terminal_resize(m_term, columns, rows, 10, 20);
        // Snapshots of a stream cut anywhere need the unfinished sequence.
        const size_t continuation = 64 * 1024;
        ghostty_terminal_set(m_term, GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES, &continuation);
        const size_t unlimited = SIZE_MAX;
        ghostty_terminal_set(m_term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &unlimited);
    }
    ~GhosttyVt()
    {
        if (m_term)
            ghostty_terminal_free(m_term);
    }
    GhosttyVt(const GhosttyVt &) = delete;
    GhosttyVt &operator=(const GhosttyVt &) = delete;

    bool ok() const { return m_term; }

    void write(const QByteArray &bytes)
    {
        ghostty_terminal_vt_write(m_term, reinterpret_cast<const uint8_t *>(bytes.constData()),
                                  size_t(bytes.size()));
    }

    // GHOSTSNP bytes, empty on failure.
    QByteArray snapshot() const
    {
        uint8_t *ptr = nullptr;
        size_t len = 0;
        if (ghostty_snapshot_encode_alloc(m_term, nullptr, &ptr, &len) != GHOSTTY_SUCCESS)
            return {};
        QByteArray out(reinterpret_cast<const char *>(ptr), qsizetype(len));
        ghostty_free(nullptr, ptr, len);
        return out;
    }

    // The active screen as plain text, a row per line, trailing blanks
    // trimmed (Ghostty's formatter).
    QByteArray plainText() const
    {
        GhosttyFormatter f = nullptr;
        if (ghostty_formatter_terminal_new(nullptr, &f, m_term) != GHOSTTY_SUCCESS)
            return {};
        const GhosttyFormatterFormat format = GHOSTTY_FORMATTER_FORMAT_PLAIN;
        ghostty_formatter_set(f, GHOSTTY_FORMATTER_OPT_EMIT, &format);
        uint8_t *ptr = nullptr;
        size_t len = 0;
        QByteArray out;
        if (ghostty_formatter_format_alloc(f, nullptr, &ptr, &len) == GHOSTTY_SUCCESS) {
            out = QByteArray(reinterpret_cast<const char *>(ptr), qsizetype(len));
            ghostty_free(nullptr, ptr, len);
        }
        ghostty_formatter_free(f);
        return out;
    }

    template<typename T>
    T get(GhosttyTerminalData what) const
    {
        T value{};
        ghostty_terminal_get(m_term, what, &value);
        return value;
    }
    int cursorX() const { return get<uint16_t>(GHOSTTY_TERMINAL_DATA_CURSOR_X); }
    int cursorY() const { return get<uint16_t>(GHOSTTY_TERMINAL_DATA_CURSOR_Y); }
    bool pendingWrap() const { return get<bool>(GHOSTTY_TERMINAL_DATA_CURSOR_PENDING_WRAP); }
    bool cursorVisible() const { return get<bool>(GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE); }
    int activeScreen() const { return int(get<GhosttyTerminalScreen>(GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN)); }
    size_t scrollbackRows() const { return get<size_t>(GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS); }
    QByteArray title() const { return string(GHOSTTY_TERMINAL_DATA_TITLE); }
    QByteArray pwd() const { return string(GHOSTTY_TERMINAL_DATA_PWD); }

private:
    QByteArray string(GhosttyTerminalData what) const
    {
        GhosttyString s{};
        if (ghostty_terminal_get(m_term, what, &s) != GHOSTTY_SUCCESS || !s.ptr)
            return {};
        return QByteArray(reinterpret_cast<const char *>(s.ptr), qsizetype(s.len));
    }

    GhosttyTerminal m_term = nullptr;
};
