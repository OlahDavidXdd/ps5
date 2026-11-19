#include "pkgInstallDialog.h"
#include "pkgInstaller.h"
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

PkgInstallDialog::PkgInstallDialog(const QString &library, QWidget *parent) : QDialog(parent) {
	setWindowTitle(tr("Install PKG"));
	resize(600, 300);
	auto *layout = new QVBoxLayout(this);
	auto *description = new QLabel(tr("Install a PS5 debug/fake base package into your game "
									  "library. Retail and update packages are unsupported."),
								   this);
	description->setWordWrap(true);
	layout->addWidget(description);
	m_inputs = new QWidget(this);
	auto *form = new QFormLayout(m_inputs);
	m_package = new QLineEdit(m_inputs);
	m_library = new QLineEdit(library, m_inputs);
	m_passcode = new QLineEdit(m_inputs);
	m_passcode->setEchoMode(QLineEdit::Password);
	m_passcode->setMaxLength(32);
	m_passcode->setPlaceholderText(tr("Optional; leave blank for the default package passcode"));
	const auto bundled = QDir(QCoreApplication::applicationDirPath())
							 .filePath(
#ifdef _WIN32
								 QStringLiteral("helpers/pkg/PlayOutPs5Pkg.exe")
#else
								 QStringLiteral("helpers/pkg/PlayOutPs5Pkg")
#endif
							 );
	m_helper = new QLineEdit(QFileInfo::exists(bundled)
								 ? bundled
								 : QSettings().value(QStringLiteral("pkg_extractor")).toString(),
							 m_inputs);
	const auto addBrowse = [this, form](const QString &title, QLineEdit *edit, bool directory,
										const QString &filter) {
		auto *row = new QWidget(m_inputs);
		auto *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		auto *button = new QPushButton(tr("Browse…"), row);
		rowLayout->addWidget(edit);
		rowLayout->addWidget(button);
		form->addRow(title, row);
		connect(button, &QPushButton::clicked, this, [this, edit, title, directory, filter] {
			const auto path = directory
								  ? QFileDialog::getExistingDirectory(this, title, edit->text())
								  : QFileDialog::getOpenFileName(this, title, edit->text(), filter);
			if (!path.isEmpty())
				edit->setText(path);
		});
	};
	addBrowse(tr("PKG file"), m_package, false,
			  tr("PlayStation packages (*.pkg *.PKG);;All files (*)"));
	addBrowse(tr("Games folder"), m_library, true, {});
	form->addRow(tr("Package passcode"), m_passcode);
	if (!QFileInfo::exists(bundled)) {
		addBrowse(tr("PlayOutPs5Pkg extractor"), m_helper, false, tr("Executables (*)"));
		m_helper->setToolTip(
			tr("Select a compatible PlayOutPs5Pkg command-line extractor. Windows release packages "
			   "include it; other platforms require a separately built helper."));
	}
	layout->addWidget(m_inputs);
	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	layout->addWidget(m_progress);
	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setTextFormat(Qt::PlainText);
	layout->addWidget(m_status);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	m_install = buttons->addButton(tr("Install"), QDialogButtonBox::ActionRole);
	connect(m_install, &QPushButton::clicked, this, &PkgInstallDialog::Install);
	connect(buttons, &QDialogButtonBox::rejected, this, &PkgInstallDialog::reject);
	layout->addWidget(buttons);
}

QString PkgInstallDialog::LibraryDirectory() const {
	return QFileInfo(m_library->text()).absoluteFilePath();
}

void PkgInstallDialog::Install() {
	if (m_running)
		return;
	m_running = true;
	m_inputs->setEnabled(false);
	m_install->setEnabled(false);
	m_status->clear();
	m_progress->setRange(0, 0);
	m_installer = new PkgInstaller(QFileInfo(m_helper->text()).absoluteFilePath(), this);
	connect(m_installer, &PkgInstaller::Progress, this,
			[this](int percent, const QString &message) {
				m_progress->setRange(0, percent < 0 ? 0 : 100);
				if (percent >= 0)
					m_progress->setValue(percent);
				m_status->setText(message);
			});
	connect(m_installer, &PkgInstaller::Finished, this,
			[this](const QString &installed, const QString &error) {
				m_running = false;
				m_inputs->setEnabled(true);
				m_install->setEnabled(true);
				m_progress->setRange(0, 100);
				m_progress->setValue(installed.isEmpty() ? 0 : 100);
				m_status->setText(error);
				if (!installed.isEmpty()) {
					QSettings().setValue(QStringLiteral("pkg_extractor"), m_helper->text());
					accept();
				} else if (m_installer->WasCanceled())
					QDialog::reject();
				m_installer->deleteLater();
				m_installer = nullptr;
			});
	m_installer->Start(m_package->text(), m_library->text(), m_passcode->text());
	m_passcode->clear();
}

void PkgInstallDialog::reject() {
	if (m_running) {
		m_status->setText(tr("Canceling installation…"));
		m_installer->Cancel();
	} else
		QDialog::reject();
}
