#include "NotebookSettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

NotebookSettingsDialog::NotebookSettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Notebook Settings");
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    auto *binaryRow = new QHBoxLayout();
    binaryPathEdit = new QLineEdit(this);
    auto *binaryBrowse = new QPushButton("Browse", this);
    binaryRow->addWidget(binaryPathEdit);
    binaryRow->addWidget(binaryBrowse);

    auto *dataRow = new QHBoxLayout();
    dataDirEdit = new QLineEdit(this);
    auto *dataBrowse = new QPushButton("Browse", this);
    dataRow->addWidget(dataDirEdit);
    dataRow->addWidget(dataBrowse);

    autoPortCheck = new QCheckBox("Select free port automatically", this);
    portSpin = new QSpinBox(this);
    portSpin->setRange(1, 65535);

    autoStartCheck = new QCheckBox("Start server automatically", this);

    form->addRow("Notebook binary", binaryRow);
    form->addRow("Notebook data directory", dataRow);
    form->addRow("Port", portSpin);
    form->addRow("", autoPortCheck);
    form->addRow("", autoStartCheck);

    layout->addLayout(form);

    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    connect(binaryBrowse, &QPushButton::clicked, this, &NotebookSettingsDialog::browseNotebookBinary);
    connect(dataBrowse, &QPushButton::clicked, this, &NotebookSettingsDialog::browseNotebookDataDirectory);
    connect(autoPortCheck, &QCheckBox::toggled, this, &NotebookSettingsDialog::updatePortState);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updatePortState();
}

void NotebookSettingsDialog::setSettings(const NotebookPluginSettings &settings) {
    binaryPathEdit->setText(settings.notebookBinaryPath);
    dataDirEdit->setText(settings.notebookDataDirectory);
    autoPortCheck->setChecked(settings.autoPort);
    portSpin->setValue(settings.fixedPort);
    autoStartCheck->setChecked(settings.autoStart);
    updatePortState();
}

NotebookPluginSettings NotebookSettingsDialog::settings() const {
    NotebookPluginSettings output;
    output.notebookBinaryPath = binaryPathEdit->text().trimmed();
    output.notebookDataDirectory = dataDirEdit->text().trimmed();
    output.autoPort = autoPortCheck->isChecked();
    output.fixedPort = portSpin->value();
    output.autoStart = autoStartCheck->isChecked();
    return output;
}

void NotebookSettingsDialog::browseNotebookBinary() {
    const QString path = QFileDialog::getOpenFileName(this, "Select rizin-notebook binary", binaryPathEdit->text());
    if (!path.isEmpty()) {
        binaryPathEdit->setText(path);
    }
}

void NotebookSettingsDialog::browseNotebookDataDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Select notebook data directory", dataDirEdit->text());
    if (!dir.isEmpty()) {
        dataDirEdit->setText(dir);
    }
}

void NotebookSettingsDialog::updatePortState() {
    portSpin->setEnabled(!autoPortCheck->isChecked());
}
