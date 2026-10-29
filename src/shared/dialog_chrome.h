#pragma once

#include <QColorDialog>

class QApplication;
class QDialog;
class QShowEvent;
class QWidget;

// Apply one rounded frame to existing dialogs without replacing their result contracts.
void styleDialog(QDialog *dialog, bool showTitle = true);
void installDialogChrome(QApplication *application);
void showNotice(QWidget *parent, const QString &heading, const QString &detail);
void showAlert(QWidget *parent, const QString &heading, const QString &detail);

class ColorDialog : public QColorDialog
{
    Q_OBJECT
public:
    explicit ColorDialog(const QColor &initial, QWidget *parent = nullptr);
signals:
    void screenPickRequested();
protected:
    void showEvent(QShowEvent *event) override;
private:
    bool m_centered = false;
};
