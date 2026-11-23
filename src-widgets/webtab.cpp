/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "webtab.h"

#include "appicon.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWebEngineCertificateError>
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

/*
 * The page's view. Only to send what a page opens in a new window (a link to
 * _blank, window.open()) to a tab of its own.
 */
class WebTabView : public QWebEngineView
{
public:
    WebTabView(WebTab *tab)
        : QWebEngineView(WebTab::profile(), tab), m_tab(tab)
    {
    }

protected:
    QWebEngineView *createWindow(QWebEnginePage::WebWindowType type) override
    {
        if (type == QWebEnginePage::WebDialog || !m_tab->m_newTab)
            return this;
        WebTab *tab = m_tab->m_newTab();
        return tab ? tab->view() : this;
    }

private:
    WebTab *m_tab;
};

WebTab::WebTab(const QUrl &url, QWidget *parent)
    : Tab(parent), m_view(new WebTabView(this)), m_snapshotTimer(new QTimer(this))
{
    const auto button = [this](const char *icon, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setIcon(appIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    m_back = button("arrow-left", tr("Back"));
    m_forward = button("arrow-right", tr("Forward"));
    m_reload = button("rotate-cw", tr("Reload"));
    m_devToolsButton = button("code-xml", tr("Developer Tools (F12)"));
    m_devToolsButton->setCheckable(true);
    m_address = new QLineEdit(this);
    m_address->setPlaceholderText(tr("Address"));
    m_address->setClearButtonEnabled(true);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(4, 4, 4, 4);
    bar->setSpacing(2);
    bar->addWidget(m_back);
    bar->addWidget(m_forward);
    bar->addWidget(m_reload);
    bar->addWidget(m_address, 1);
    bar->addWidget(m_devToolsButton);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_view);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_splitter, 1);
    setFocusProxy(m_view);

    connect(m_back, &QToolButton::clicked, m_view, &QWebEngineView::back);
    connect(m_forward, &QToolButton::clicked, m_view, &QWebEngineView::forward);
    connect(m_reload, &QToolButton::clicked, this, [this]() {
        if (m_loading)
            m_view->stop();
        else
            reload();
    });
    connect(m_devToolsButton, &QToolButton::toggled, this, &WebTab::setDevToolsShown);
    connect(m_address, &QLineEdit::returnPressed, this, [this]() {
        const QUrl url = urlFromInput(m_address->text());
        if (url.isValid()) {
            load(url);
            m_view->setFocus();
        }
    });
    // Ctrl+L to the address, as in a browser; Escape back to the page.
    auto *focusAddress = new QAction(this);
    focusAddress->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    focusAddress->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(focusAddress, &QAction::triggered, this, [this]() {
        m_address->setFocus();
        m_address->selectAll();
    });
    addAction(focusAddress);
    auto *devTools = new QAction(this);
    devTools->setShortcut(QKeySequence(Qt::Key_F12));
    devTools->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(devTools, &QAction::triggered, this, [this]() { setDevToolsShown(!devToolsShown()); });
    addAction(devTools);

    connect(m_view, &QWebEngineView::titleChanged, this, &WebTab::changed);
    connect(m_view, &QWebEngineView::iconChanged, this, &WebTab::changed);
    connect(m_view, &QWebEngineView::urlChanged, this, [this]() {
        updateControls();
        emit changed();
    });
    connect(m_view, &QWebEngineView::loadStarted, this, [this]() {
        m_loading = true;
        updateControls();
    });
    connect(m_view, &QWebEngineView::loadFinished, this, [this]() {
        m_loading = false;
        updateControls();
        // Once it has drawn.
        QTimer::singleShot(300, this, &WebTab::takeSnapshot);
        emit changed();
    });

    // A development server's own certificate (mkcert's, say) is the usual
    // reason for one that can't be verified: asked about, not refused.
    connect(m_view->page(), &QWebEnginePage::certificateError, this,
            [this](QWebEngineCertificateError error) {
        error.defer();
        auto *box = new QMessageBox(QMessageBox::Warning, tr("Certificate"),
                                    tr("The certificate of %1 can't be trusted:\n%2\n\n"
                                       "Open the page anyway?")
                                        .arg(error.url().host(), error.description()),
                                    QMessageBox::Yes | QMessageBox::No, this);
        box->setDefaultButton(QMessageBox::No);
        box->setAttribute(Qt::WA_DeleteOnClose);
        connect(box, &QMessageBox::finished, this, [box, error]() mutable {
            if (box->clickedButton() == box->button(QMessageBox::Yes))
                error.acceptCertificate();
            else
                error.rejectCertificate();
        });
        box->open();
    });

    // Pages change without loading (a hot reload, a single-page app): the
    // card follows while the tab is on screen.
    m_snapshotTimer->setInterval(2000);
    connect(m_snapshotTimer, &QTimer::timeout, this, &WebTab::takeSnapshot);
    m_snapshotTimer->start();

    updateControls();
    if (url.isValid())
        load(url);
}

QUrl WebTab::url() const
{
    // Until the page has come, the one asked for.
    return m_view->url().isEmpty() ? m_requested : m_view->url();
}

void WebTab::load(const QUrl &url)
{
    m_requested = url;
    m_view->load(url);
    updateControls();
}

void WebTab::reload()
{
    m_view->reload();
}

QString WebTab::title() const
{
    const QString title = m_view->title();
    // Until a page has a title of its own, QtWebEngine gives its address.
    if (!title.isEmpty() && title != url().toString())
        return title;
    if (url().isEmpty())
        return tr("New Web Tab");
    return url().host().isEmpty() ? url().toString() : url().host() + (url().port() > 0
        ? QStringLiteral(":%1").arg(url().port()) : QString());
}

QString WebTab::details() const
{
    return url().toDisplayString();
}

void WebTab::setReloadAfterCommands(bool on)
{
    m_reloadAfterCommands = on;
    emit changed();
}

bool WebTab::devToolsShown() const
{
    return m_devTools && m_devTools->isVisible();
}

void WebTab::setDevToolsShown(bool shown)
{
    if (shown && !m_devTools) {
        m_devTools = new QWebEngineView(profile(), this);
        m_view->page()->setDevToolsPage(m_devTools->page());
        m_splitter->addWidget(m_devTools);
        const int width = m_splitter->width();
        m_splitter->setSizes({width * 3 / 5, width * 2 / 5});
    }
    if (m_devTools)
        m_devTools->setVisible(shown);
    const QSignalBlocker block(m_devToolsButton);
    m_devToolsButton->setChecked(shown);
}

void WebTab::updateControls()
{
    m_back->setEnabled(m_view->history()->canGoBack());
    m_forward->setEnabled(m_view->history()->canGoForward());
    m_reload->setIcon(appIcon(m_loading ? "x" : "rotate-cw"));
    m_reload->setToolTip(m_loading ? tr("Stop") : tr("Reload"));
    // Not over what is being typed there.
    if (!m_address->hasFocus() || !m_address->isModified())
        m_address->setText(url().toDisplayString());
}

void WebTab::takeSnapshot()
{
    if (!isVisible() || m_view->width() <= 0 || m_view->height() <= 0)
        return;
    m_snapshot = m_view->grab();
    m_newOutput = true;
}

QUrl WebTab::urlFromInput(const QString &text)
{
    const QString input = text.trimmed();
    if (input.isEmpty())
        return QUrl();
    // host:port reads as a scheme and a path; a bare host as a relative path.
    if (!input.contains(QLatin1String("://"))) {
        const QUrl guess(QStringLiteral("http://") + input);
        if (guess.isValid() && !guess.host().isEmpty())
            return guess;
    }
    return QUrl::fromUserInput(input);
}

bool WebTab::isLocal(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")))
        return true;
    const QHostAddress address(host);
    return !address.isNull()
        && (address.isLoopback() || address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6);
}

QWebEngineProfile *WebTab::profile()
{
    // Named, so it is kept on disk (in the application's data folder).
    static QWebEngineProfile *profile = new QWebEngineProfile(QStringLiteral("web"), qApp);
    return profile;
}

QPixmap WebTab::previewPixmap(const QSize &size, qreal devicePixelRatio) const
{
    QPixmap out(size * devicePixelRatio);
    out.setDevicePixelRatio(devicePixelRatio);
    out.fill(backgroundColour());
    if (m_snapshot.isNull())
        return out;
    // The top of the page, as wide as the card.
    const QSizeF page = m_snapshot.deviceIndependentSize();
    const qreal scale = size.width() / page.width();
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(QRectF(0, 0, size.width(), page.height() * scale), m_snapshot,
                 QRectF(QPointF(0, 0), m_snapshot.size()));
    return out;
}

QColor WebTab::backgroundColour() const
{
    return palette().color(QPalette::Base);
}

QColor WebTab::foregroundColour() const
{
    return palette().color(QPalette::Text);
}

void WebTab::focusContent()
{
    if (url().isEmpty() && m_requested.isEmpty())
        m_address->setFocus();
    else
        m_view->setFocus();
}
