#pragma once

#include <QListView>

class QStandardItemModel;

/*
 * The colour schemes as a grid of cards, each a small terminal in that
 * scheme's colours: its background, a line of text in its foreground, and
 * its sixteen colours as swatches. Clicking one chooses it.
 */
class SchemeGallery : public QListView
{
    Q_OBJECT

public:
    explicit SchemeGallery(QWidget *parent = nullptr);

    /* Fill (or refill) it with these schemes, by name. */
    void setSchemes(const QStringList &names);
    /* Show `name` as the chosen one, without emitting chosen(). A full
     * path is matched by its basename. */
    void setCurrent(const QString &name);
    QString current() const;

    QSize sizeHint() const override;

signals:
    void chosen(const QString &name);

protected:
    void changeEvent(QEvent *event) override;

private:
    QStandardItemModel *m_model;
    QStringList m_names;
    bool m_quiet = false;
};
