#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <Emulation.h>
#include <qtermwidget.h>

namespace {
// The block/underline/beam enum lives in Konsole::Emulation. QTermWidget
// aliases the type as QTermWidget::KeyboardCursorShape but not its values.
using CursorShape = Konsole::Emulation::KeyboardCursorShape;
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SuperTerm (widgets spike)"));

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    setCentralWidget(m_tabs);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget *w = m_tabs->widget(index);
        m_tabs->removeTab(index);
        w->deleteLater();
        if (m_tabs->count() == 0)
            newTab();
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::updateStatus);

    buildMenus();
    buildStatusBar();

    // --- settings dock: the four things QMLTermWidget cannot reach ---------
    auto *dock = new QDockWidget(tr("Terminal settings"), this);
    dock->setObjectName(QStringLiteral("settingsDock"));
    auto *panel = new QWidget(dock);
    auto *form = new QFormLayout(panel);
    form->setContentsMargins(10, 10, 10, 10);
    form->setSpacing(8);

    // 1. Scrollback -- unreachable in the QML build.
    m_historyBox = new QSpinBox(panel);
    m_historyBox->setRange(0, 200000);
    m_historyBox->setSingleStep(500);
    m_historyBox->setValue(5000);
    form->addRow(tr("Scrollback (lines)"), m_historyBox);
    connect(m_historyBox, &QSpinBox::valueChanged, this, &MainWindow::applyHistorySize);

    // 2. Colour scheme.
    m_schemeBox = new QComboBox(panel);
    m_schemeBox->addItems(QTermWidget::availableColorSchemes());
    const int solar = m_schemeBox->findText(QStringLiteral("Solarized"));
    if (solar >= 0)
        m_schemeBox->setCurrentIndex(solar);
    form->addRow(tr("Colour scheme"), m_schemeBox);
    connect(m_schemeBox, &QComboBox::currentTextChanged, this, &MainWindow::applyColorScheme);

    // 3. Cursor shape -- not offered at all by the QML build.
    m_cursorBox = new QComboBox(panel);
    m_cursorBox->addItem(tr("Block"), int(CursorShape::BlockCursor));
    m_cursorBox->addItem(tr("Underline"), int(CursorShape::UnderlineCursor));
    m_cursorBox->addItem(tr("I-beam"), int(CursorShape::IBeamCursor));
    form->addRow(tr("Cursor shape"), m_cursorBox);
    connect(m_cursorBox, &QComboBox::currentIndexChanged, this, &MainWindow::applyCursorShape);

    // 4. Own line-drawing characters -- unreachable in the QML build.
    m_lineCharsBox = new QCheckBox(tr("Draw own line characters"), panel);
    m_lineCharsBox->setChecked(true);
    form->addRow(QString(), m_lineCharsBox);
    connect(m_lineCharsBox, &QCheckBox::toggled, this, &MainWindow::applyLineChars);

    m_fontSlider = new QSlider(Qt::Horizontal, panel);
    m_fontSlider->setRange(8, 32);
    m_fontSlider->setValue(13);
    form->addRow(tr("Font size"), m_fontSlider);
    connect(m_fontSlider, &QSlider::valueChanged, this, &MainWindow::applyFontSize);

    dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // Prove the platform style is in use (this is what made the KDE theme
    // appear in the earlier probe).
    statusBar()->showMessage(
        tr("Widget style: %1  ·  quick controls are not used here")
            .arg(QApplication::style()->objectName()));

    setMinimumSize(760, 420);
    resize(1100, 650);

    newTab();
}

QTermWidget *MainWindow::currentTerminal() const
{
    return qobject_cast<QTermWidget *>(m_tabs->currentWidget());
}

QTermWidget *MainWindow::createTerminal()
{
    auto *term = new QTermWidget(0, m_tabs);

    // Apply whatever the dock currently says, so new tabs match.
    term->setColorScheme(m_schemeBox->currentText());
    term->setHistorySize(m_historyBox->value());
    term->setDrawLineChars(m_lineCharsBox->isChecked());
    term->setKeyboardCursorShape(
        static_cast<CursorShape>(m_cursorBox->currentData().toInt()));
    term->setBlinkingCursor(true);
    term->setTerminalFont(QFont(QStringLiteral("Hack"), m_fontSlider->value()));

    connect(term, &QTermWidget::titleChanged, this, [this, term]() {
        const int index = m_tabs->indexOf(term);
        if (index < 0)
            return;
        const QString t = term->title();
        m_tabs->setTabText(index, t.isEmpty() ? tr("Terminal") : t);
        if (term == currentTerminal())
            setWindowTitle(tr("%1 — SuperTerm").arg(m_tabs->tabText(index)));
    });

    connect(term, &QTermWidget::finished, this, [this, term]() {
        const int index = m_tabs->indexOf(term);
        if (index < 0)
            return;
        m_tabs->removeTab(index);
        term->deleteLater();
        if (m_tabs->count() == 0)
            newTab();
    });

    term->startShellProgram();
    return term;
}

void MainWindow::newTab()
{
    auto *term = createTerminal();
    const int index = m_tabs->addTab(term, tr("Terminal"));
    m_tabs->setCurrentIndex(index);
    term->setFocus();
    ++m_nextTabId;
}

void MainWindow::closeCurrentTab()
{
    const int index = m_tabs->currentIndex();
    if (index < 0)
        return;
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    w->deleteLater();
    if (m_tabs->count() == 0)
        newTab();
}

void MainWindow::applyHistorySize(int lines)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = qobject_cast<QTermWidget *>(m_tabs->widget(i)))
            t->setHistorySize(lines);
    }
}

void MainWindow::applyColorScheme(const QString &name)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = qobject_cast<QTermWidget *>(m_tabs->widget(i)))
            t->setColorScheme(name);
    }
}

void MainWindow::applyCursorShape(int)
{
    const auto shape = static_cast<CursorShape>(m_cursorBox->currentData().toInt());
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = qobject_cast<QTermWidget *>(m_tabs->widget(i)))
            t->setKeyboardCursorShape(shape);
    }
}

void MainWindow::applyLineChars(bool enabled)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = qobject_cast<QTermWidget *>(m_tabs->widget(i)))
            t->setDrawLineChars(enabled);
    }
}

void MainWindow::applyFontSize(int points)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = qobject_cast<QTermWidget *>(m_tabs->widget(i)))
            t->setTerminalFont(QFont(QStringLiteral("Hack"), points));
    }
}

void MainWindow::updateStatus()
{
    auto *term = currentTerminal();
    if (!term)
        return;
    m_statusDir->setText(term->title());
    m_statusInfo->setText(tr("%1 × %2  ·  scrollback %3")
                              .arg(term->screenColumnsCount())
                              .arg(term->screenLinesCount())
                              .arg(term->historySize()));
}

void MainWindow::buildMenus()
{
    auto *file = menuBar()->addMenu(tr("&File"));
    auto *newAct = file->addAction(tr("&New Tab"));
    newAct->setShortcut(QKeySequence::AddTab);
    connect(newAct, &QAction::triggered, this, &MainWindow::newTab);

    auto *closeAct = file->addAction(tr("&Close Tab"));
    closeAct->setShortcut(QKeySequence::Close);
    connect(closeAct, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    file->addSeparator();
    auto *quitAct = file->addAction(tr("&Quit"));
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    auto *edit = menuBar()->addMenu(tr("&Edit"));
    auto *copyAct = edit->addAction(tr("&Copy"));
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->copyClipboard();
    });

    auto *pasteAct = edit->addAction(tr("&Paste"));
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->pasteClipboard();
    });

    edit->addSeparator();
    auto *findAct = edit->addAction(tr("&Find…"));
    findAct->setShortcut(QKeySequence::Find);
    connect(findAct, &QAction::triggered, this, [this]() {
        // The widgets build has a search bar built in -- no overlay to write.
        if (auto *t = currentTerminal())
            t->toggleShowSearchBar();
    });

    auto *view = menuBar()->addMenu(tr("&View"));
    auto *zoomIn = view->addAction(tr("Zoom &In"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, [this]() {
        m_fontSlider->setValue(m_fontSlider->value() + 1);
    });
    auto *zoomOut = view->addAction(tr("Zoom &Out"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [this]() {
        m_fontSlider->setValue(m_fontSlider->value() - 1);
    });
}

void MainWindow::buildStatusBar()
{
    m_statusDir = new QLabel(this);
    m_statusInfo = new QLabel(this);
    statusBar()->addWidget(m_statusDir, 1);
    statusBar()->addPermanentWidget(m_statusInfo);
}
