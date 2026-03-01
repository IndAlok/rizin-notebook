#pragma once

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;

struct NotebookPluginSettings {
    QString notebookBinaryPath;
    QString notebookDataDirectory;
    bool autoPort = true;
    int fixedPort = 8000;
    bool autoStart = true;
};

class NotebookSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit NotebookSettingsDialog(QWidget *parent = nullptr);

    void setSettings(const NotebookPluginSettings &settings);
    NotebookPluginSettings settings() const;

private slots:
    void browseNotebookBinary();
    void browseNotebookDataDirectory();
    void updatePortState();

private:
    QLineEdit *binaryPathEdit = nullptr;
    QLineEdit *dataDirEdit = nullptr;
    QCheckBox *autoPortCheck = nullptr;
    QSpinBox *portSpin = nullptr;
    QCheckBox *autoStartCheck = nullptr;
    QDialogButtonBox *buttons = nullptr;
};
