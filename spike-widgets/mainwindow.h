/*
 * superterm — Qt Widgets spike.
 *
 * A deliberate throwaway: the smallest shell that answers one question — does
 * a QWidget-based SuperTerm feel acceptable, and do the settings that
 * QMLTermWidget cannot reach actually work from a real UI?
 *
 * Deliberately NOT a port of the QML chrome. It is QMainWindow + QTabWidget +
 * QTermWidget, with the four previously-unreachable settings wired to real
 * controls. If this feels right, the rest is mechanical.
 */

#pragma once

#include <QMainWindow>
#include <QVector>

class QTabWidget;
class QTermWidget;
class QLabel;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QSlider;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void newTab();
    void closeCurrentTab();
    void applyHistorySize(int lines);
    void applyColorScheme(const QString &name);
    void applyCursorShape(int index);
    void applyLineChars(bool enabled);
    void applyFontSize(int points);
    void updateStatus();

private:
    QTermWidget *currentTerminal() const;
    QTermWidget *createTerminal();
    void buildMenus();
    void buildStatusBar();

    QTabWidget *m_tabs = nullptr;
    QLabel *m_statusDir = nullptr;
    QLabel *m_statusInfo = nullptr;

    // Settings controls live in a dock so they are visible while testing.
    QSpinBox *m_historyBox = nullptr;
    QComboBox *m_schemeBox = nullptr;
    QComboBox *m_cursorBox = nullptr;
    QCheckBox *m_lineCharsBox = nullptr;
    QSlider *m_fontSlider = nullptr;

    int m_nextTabId = 1;
};
