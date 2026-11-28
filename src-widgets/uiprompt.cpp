/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "uiprompt.h"

#include "popupframe.h"
#include "rexprompt.h"

#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QFontDatabase>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace {

/* A message or a question, in the prompts' panel: a heading, the text, a
 * detail if any, and its buttons at the right. exec() gives the index of
 * the choice, or -1 for Cancel. */
class MessagePanel : public QDialog
{
public:
    MessagePanel(QWidget *parent, const QString &title, const QString &text, const QString &detail,
                 const QList<uiprompt::Choice> &choices, bool cancel, int defaultChoice)
        : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setModal(true);
        setWindowTitle(title);

        const int s = popupframe::kShadow;
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(s + 22, s + 18, s + 18, s + 16);
        outer->setSpacing(8);

        auto *heading = new QLabel(title, this);
        QFont headingFont = heading->font();
        headingFont.setPointSizeF(headingFont.pointSizeF() * 1.08);
        headingFont.setWeight(QFont::DemiBold);
        heading->setFont(headingFont);
        heading->setWordWrap(true);
        outer->addWidget(heading);

        auto *body = new QLabel(text, this);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QPalette quiet = palette();
        quiet.setColor(QPalette::WindowText, cardstyle::mix(palette().color(QPalette::Window),
                                                            palette().color(QPalette::WindowText), 0.8));
        body->setPalette(quiet);
        body->setVisible(!text.isEmpty());
        outer->addWidget(body);

        if (!detail.isEmpty()) {
            auto *box = new QPlainTextEdit(detail, this);
            box->setReadOnly(true);
            box->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            box->setFrameShape(QFrame::NoFrame);
            // As tall as its lines, up to a few; it scrolls past that.
            const int lines = int(detail.count(QLatin1Char('\n'))) + 1;
            box->setFixedHeight(std::min(160, lines * QFontMetrics(box->font()).lineSpacing() + 14));
            QPalette pal = box->palette();
            pal.setColor(QPalette::Base, cardstyle::mix(popupframe::panelColour(palette()),
                                                        palette().color(QPalette::WindowText), 0.06));
            box->setPalette(pal);
            outer->addWidget(box);
        }
        outer->addSpacing(8);

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        buttons->addStretch();
        QPushButton *focus = nullptr;
        if (cancel) {
            auto *no = new QPushButton(QObject::tr("Cancel"), this);
            no->setAutoDefault(false);
            connect(no, &QPushButton::clicked, this, [this]() { done(-1); });
            buttons->addWidget(no);
            if (defaultChoice < 0)
                focus = no;
        }
        for (int i = 0; i < choices.size(); ++i) {
            auto *b = new QPushButton(choices.at(i).label, this);
            b->setAutoDefault(false);
            if (choices.at(i).destructive) {
                QPalette red = b->palette();
                red.setColor(QPalette::ButtonText, QColor(0xd8, 0x3b, 0x3b));
                b->setPalette(red);
            }
            b->setObjectName(choices.at(i).name);
            connect(b, &QPushButton::clicked, this, [this, i]() { done(i); });
            buttons->addWidget(b);
            if (i == defaultChoice)
                focus = b;
        }
        outer->addLayout(buttons);
        if (focus) {
            focus->setDefault(true);
            focus->setFocus();
        }
    }

    // Esc (or closing) is Cancel.
    void reject() override { done(-1); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        popupframe::paint(p, rect(), palette());
    }

    void showEvent(QShowEvent *event) override
    {
        // Where the prompts are: across the top of the window.
        const int width = 440 + 2 * popupframe::kShadow;
        resize(width, heightForWidth(width) > 0 ? heightForWidth(width) : sizeHint().height());
        QWidget *over = parentWidget() ? parentWidget()->window() : nullptr;
        const QRect area = over ? over->geometry()
                                : (screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800));
        move(area.center().x() - width / 2, area.top() + std::max(40, int(area.height() * 0.12)) - popupframe::kShadow);
        QDialog::showEvent(event);
    }
};

/* Paints a framed dialog's panel under its widgets, and puts it where the
 * prompts go when it's shown. */
class FrameFilter : public QObject
{
public:
    explicit FrameFilter(QDialog *dialog) : QObject(dialog), m_dialog(dialog) {}

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_dialog)
            return false;
        if (event->type() == QEvent::Paint) {
            QPainter p(m_dialog);
            popupframe::paint(p, m_dialog->rect(), m_dialog->palette());
        } else if (event->type() == QEvent::Show) {
            const QSize hint = m_dialog->sizeHint();
            const int width = std::max(hint.width(), 440 + 2 * popupframe::kShadow);
            const int height = m_dialog->hasHeightForWidth() ? m_dialog->heightForWidth(width) : hint.height();
            m_dialog->resize(width, std::max(height, hint.height()));
            QWidget *over = m_dialog->parentWidget() ? m_dialog->parentWidget()->window() : nullptr;
            const QRect area = over ? over->geometry()
                                    : (m_dialog->screen() ? m_dialog->screen()->availableGeometry()
                                                          : QRect(0, 0, 1280, 800));
            m_dialog->move(area.center().x() - m_dialog->width() / 2,
                           area.top() + std::max(40, int(area.height() * 0.12)) - popupframe::kShadow);
        }
        return false;
    }

private:
    QDialog *m_dialog;
};

} // namespace

namespace uiprompt {

void frame(QDialog *dialog)
{
    dialog->setWindowFlags(dialog->windowFlags() | Qt::FramelessWindowHint);
    dialog->setAttribute(Qt::WA_TranslucentBackground);
    const int s = popupframe::kShadow;
    auto *heading = new QLabel(dialog->windowTitle(), dialog);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.08);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    if (auto *box = qobject_cast<QBoxLayout *>(dialog->layout()))
        box->insertWidget(0, heading);
    else if (auto *form = qobject_cast<QFormLayout *>(dialog->layout()))
        form->insertRow(0, heading);
    if (QLayout *layout = dialog->layout())
        layout->setContentsMargins(s + 22, s + 18, s + 18, s + 16);
    dialog->installEventFilter(new FrameFilter(dialog));
}


QString text(QWidget *parent, const QString &title, const QString &placeholder, const QString &value, bool *ok,
             const QString &confirm)
{
    RexPrompt prompt(QJsonObject{{QStringLiteral("kind"), QStringLiteral("input")},
                                 {QStringLiteral("title"), title},
                                 {QStringLiteral("placeholder"), placeholder},
                                 {QStringLiteral("value"), value},
                                 {QStringLiteral("confirm"), confirm.isEmpty() ? QObject::tr("OK") : confirm}},
                     parent);
    const bool accepted = prompt.exec() == QDialog::Accepted;
    if (ok)
        *ok = accepted;
    return accepted ? prompt.answer().value(QStringLiteral("text")).toString() : QString();
}

QString pick(QWidget *parent, const QString &title, const QStringList &items, bool *ok, bool allowOther,
             const QString &placeholder)
{
    RexPrompt prompt(QJsonObject{{QStringLiteral("kind"), QStringLiteral("pick")},
                                 {QStringLiteral("title"), title},
                                 {QStringLiteral("placeholder"), placeholder},
                                 {QStringLiteral("items"), QJsonArray::fromStringList(items)},
                                 {QStringLiteral("allow_other"), allowOther}},
                     parent);
    const bool accepted = prompt.exec() == QDialog::Accepted;
    if (ok)
        *ok = accepted;
    if (!accepted)
        return QString();
    const QJsonObject answer = prompt.answer();
    const QJsonArray ids = answer.value(QStringLiteral("ids")).toArray();
    return ids.isEmpty() ? answer.value(QStringLiteral("text")).toString() : ids.first().toString();
}

bool confirm(QWidget *parent, const QString &title, const QString &text, const QString &action, bool destructive)
{
    // A destructive answer isn't the one Return gives by accident.
    return choose(parent, title, text, {{action, destructive}}, destructive ? -1 : 0) == 0;
}

void message(QWidget *parent, const QString &title, const QString &text, const QString &detail)
{
    MessagePanel panel(parent, title, text, detail, {{QObject::tr("OK"), false}}, false, 0);
    panel.exec();
}

int choose(QWidget *parent, const QString &title, const QString &text, const QList<Choice> &choices,
           int defaultChoice, const QString &detail)
{
    MessagePanel panel(parent, title, text, detail, choices, true, defaultChoice);
    return panel.exec();
}

QDialog *chooseLater(QWidget *parent, const QString &title, const QString &text, const QList<Choice> &choices,
                     int defaultChoice, const QString &detail, std::function<void(int)> done)
{
    auto *panel = new MessagePanel(parent, title, text, detail, choices, true, defaultChoice);
    panel->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(panel, &QDialog::finished, panel, [done = std::move(done)](int result) { done(result); });
    panel->open();
    return panel;
}

} // namespace uiprompt
