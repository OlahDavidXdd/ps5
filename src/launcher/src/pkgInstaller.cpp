#include "pkgInstaller.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QUrl>
#include <utility>

PkgInstaller::PkgInstaller(QString helper, QObject *parent)
	: QObject(parent), m_helper(std::move(helper)) {
	m_inspection_timer.setSingleShot(true);
	connect(&m_inspection_timer, &QTimer::timeout, this, [this] {
		m_error = tr("Package inspection timed out.");
		m_process.kill();
	});
	connect(&m_process, &QProcess::started, this, [this] {
		if (m_canceled || !m_error.isEmpty())
			m_process.kill();
	});
	connect(&m_process, &QProcess::readyReadStandardOutput, this, &PkgInstaller::ReadOutput);
	connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
		// Drain diagnostics without retaining package passcodes or unbounded logs.
		m_process.readAllStandardError();
	});
	connect(&m_process, &QProcess::finished, this, &PkgInstaller::ProcessFinished);
	connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart)
			Complete({},
					 tr("Could not start the package extractor: %1").arg(m_process.errorString()));
	});
}

PkgInstaller::~PkgInstaller() {
	m_process.disconnect(this);
	if (m_process.state() != QProcess::NotRunning) {
		m_process.kill();
		m_process.waitForFinished();
	}
}

QString PkgInstaller::NormalizePathInput(const QString &input) {
	if (input.isEmpty())
		return {};
	QString path = input;
	// Preserve real filenames containing whitespace or quotes; unwrap only copied input.
	if (!QFileInfo::exists(path)) {
		path = path.trimmed();
		const auto stripFormatting = [&path] {
			while (!path.isEmpty() && path.front().category() == QChar::Other_Format)
				path.remove(0, 1);
			while (!path.isEmpty() && path.back().category() == QChar::Other_Format)
				path.chop(1);
		};
		stripFormatting();
		path = path.trimmed();
		if (path.size() >= 2 && ((path.front() == '"' && path.back() == '"') ||
								 (path.front() == '\'' && path.back() == '\'')))
			path = path.mid(1, path.size() - 2);
		if (!QFileInfo::exists(path)) {
			stripFormatting();
			path = path.trimmed();
			const QUrl url(path);
			if (url.isLocalFile())
				path = url.toLocalFile();
		}
	}
	return path.isEmpty() ? QString()
						  : QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath();
}

void PkgInstaller::Start(const QString &package, const QString &library, const QString &passcode) {
	if (m_started || m_finished)
		return;
	m_started = true;
	m_helper = NormalizePathInput(m_helper);
	const auto package_path = NormalizePathInput(package);
	const auto library_path = NormalizePathInput(library);
	if (!QFileInfo(m_helper).isAbsolute() || !QFileInfo(m_helper).isFile() ||
		!QFileInfo(m_helper).isExecutable()) {
		Complete({}, tr("Select a PlayOutPs5Pkg extractor executable first."));
		return;
	}
	if (package_path.isEmpty()) {
		Complete({}, tr("Choose a PKG file using Browse."));
		return;
	}
	if (!QFileInfo(package_path).isFile()) {
		Complete({}, tr("Cannot access the PKG file at:\n%1\n\nSelect the actual .pkg file using "
						"Browse. If it is inside an archive, extract it first.")
						 .arg(package_path));
		return;
	}
	QFile source(package_path);
	if (!source.open(QIODevice::ReadOnly)) {
		Complete({},
				 tr("Cannot read the PKG file:\n%1\n\n%2").arg(package_path, source.errorString()));
		return;
	}
	source.close();
	if (library_path.isEmpty() || !QDir(library_path).exists()) {
		Complete({}, tr("Cannot access the games folder at:\n%1\n\nSelect an existing folder using "
						"Games folder → Browse.")
						 .arg(library_path.isEmpty() ? tr("(no folder selected)") : library_path));
		return;
	}
	if (!passcode.isEmpty() && (passcode.size() != 32 || passcode.contains(QRegularExpression(
															 QStringLiteral("[^\\x20-\\x7e]"))))) {
		Complete({}, tr("The package passcode must contain 32 printable ASCII characters."));
		return;
	}
	m_package = package_path;
	m_passcode = passcode;
	auto name = QFileInfo(package_path).completeBaseName();
	name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("_"));
	// Trailing dots/spaces are aliases on Windows; keep one portable directory name.
	while (name.endsWith('.') || name.endsWith(' '))
		name.chop(1);
	if (name.isEmpty() || name == QStringLiteral(".."))
		name = QStringLiteral("game");
	m_destination = QDir(library_path).absoluteFilePath(name);
	if (QFileInfo::exists(m_destination) || QFileInfo(m_destination).isSymLink()) {
		Complete({}, tr("The destination already exists: %1. Choose another games folder or rename "
						"the PKG file.")
						 .arg(m_destination));
		return;
	}
	m_staging = std::make_unique<QTemporaryDir>(
		QDir(library_path).absoluteFilePath(QStringLiteral(".pkg-install-XXXXXX")));
	if (!m_staging->isValid()) {
		Complete(
			{},
			tr("Cannot create an installation folder. Check free space and folder permissions."));
		return;
	}
	// Keep extractor scratch images and .NET self-extraction inside the owned transaction.
	auto environment = QProcessEnvironment::systemEnvironment();
	for (const auto &name : {"TEMP", "TMP", "TMPDIR"})
		environment.insert(QString::fromLatin1(name), m_staging->path());
	environment.insert(QStringLiteral("DOTNET_BUNDLE_EXTRACT_BASE_DIR"),
					   m_staging->filePath(QStringLiteral("runtime")));
	m_process.setProcessEnvironment(environment);
	m_process.setWorkingDirectory(m_staging->path());
	m_process.setProgram(m_helper);
	m_process.setArguments({QStringLiteral("info"), m_package});
	emit Progress(-1, tr("Inspecting package…"));
	if (m_finished)
		return;
	m_inspection_timer.start(60000);
	m_process.start();
}

void PkgInstaller::Cancel() {
	if (m_finished)
		return;
	m_canceled = true;
	if (m_process.state() == QProcess::NotRunning)
		Complete({}, tr("Installation canceled."));
	else
		m_process.kill();
}

void PkgInstaller::ReadOutput() {
	m_output += m_process.readAllStandardOutput();
	if (m_output.size() > 262144) {
		m_error = tr("The package extractor returned too much output.");
		m_process.kill();
		return;
	}
	while (m_output.contains('\n')) {
		const auto end = m_output.indexOf('\n');
		const auto line = m_output.left(end).trimmed();
		m_output.remove(0, end + 1);
		if (line.isEmpty())
			continue;
		QJsonParseError error;
		const auto document = QJsonDocument::fromJson(line, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			m_error = tr("The package extractor returned an invalid response.");
			m_process.kill();
			return;
		}
		const auto object = document.object();
		const auto type = object.value(QStringLiteral("type")).toString();
		if (type == QStringLiteral("error")) {
			m_error = object.value(QStringLiteral("message")).toString().left(2048);
			if (m_error.isEmpty())
				m_error = tr("Package extraction failed.");
		} else if (type == QStringLiteral("done")) {
			m_done = true;
			if (m_inspecting) {
				const auto kind = object.value(QStringLiteral("kind")).toString();
				m_supported = object.value(QStringLiteral("unpackable")).toBool() &&
							  kind == QStringLiteral("fpkg");
				if (!m_supported) {
					if (kind == QStringLiteral("retail"))
						m_error =
							tr("Detected type: retail-encrypted PS5 package. This extractor cannot "
							   "decrypt retail packages. Use a supported PS5 debug/fake base "
							   "package or an already-decrypted game folder.");
					else if (kind == QStringLiteral("patch"))
						m_error = tr("Detected type: update/patch package. Standalone update "
									 "installation is unsupported. Select the game's PS5 "
									 "debug/fake base package instead.");
					else if (kind == QStringLiteral("metadata"))
						m_error = tr("Detected type: metadata/CNT container. This extractor only "
									 "installs finalized PS5 debug/fake base packages; it cannot "
									 "install this container format.");
					else if (kind == QStringLiteral("unknown") || kind.isEmpty())
						m_error = tr("Detected type: unknown package format. The extractor does "
									 "not recognize a supported PS5 debug/fake base package. The "
									 "file may use another format or be incomplete.");
					else
						m_error =
							tr("The extractor cannot install this package. Detected type: %1.")
								.arg(kind.left(64));
				}
			}
		} else if (type == QStringLiteral("progress") && !m_inspecting) {
			const auto total = object.value(QStringLiteral("total")).toDouble();
			const auto percent =
				total > 0 ? qBound(0, object.value(QStringLiteral("percent")).toInt(), 100) : -1;
			emit Progress(percent, total > 0 ? tr("Installing package… %1%").arg(percent)
											 : tr("Reading package…"));
		} else {
			m_error = tr("The package extractor returned an unexpected response.");
			m_process.kill();
			return;
		}
	}
}

void PkgInstaller::ProcessFinished(int code, QProcess::ExitStatus status) {
	if (m_finished)
		return;
	m_inspection_timer.stop();
	ReadOutput();
	if (m_canceled) {
		Complete({}, tr("Installation canceled."));
		return;
	}
	if (!m_output.trimmed().isEmpty() && m_error.isEmpty())
		m_error = tr("The package extractor returned an incomplete response.");
	if (code != 0 || status != QProcess::NormalExit || !m_error.isEmpty() || !m_done) {
		Complete({}, m_error.isEmpty() ? tr("Package extraction failed. The package may be damaged "
											"or its passcode incorrect.")
									   : m_error);
		return;
	}
	if (m_inspecting) {
		m_inspecting = false;
		m_done = false;
		m_output.clear();
		QStringList arguments{QStringLiteral("extract"), m_package,
							  m_staging->filePath(QStringLiteral("game"))};
		if (!m_passcode.isEmpty())
			arguments << QStringLiteral("--passcode") << m_passcode;
		m_process.setArguments(arguments);
		m_passcode.clear();
		emit Progress(-1, tr("Reading package…"));
		if (m_finished)
			return;
		m_process.start();
		return;
	}
	const auto game = m_staging->filePath(QStringLiteral("game"));
	const QFileInfo eboot(QDir(game).filePath(QStringLiteral("eboot.bin")));
	bool valid = !QFileInfo(game).isSymLink() && eboot.isFile() && eboot.size() > 0;
	QDirIterator entries(game,
						 QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
						 QDirIterator::Subdirectories);
	while (valid && entries.hasNext()) {
		entries.next();
		const auto info = entries.fileInfo();
		valid = !info.isSymLink() && (info.isFile() || info.isDir());
	}
	QFile metadata(QDir(game).filePath(QStringLiteral("sce_sys/param.json")));
	valid = valid && metadata.open(QIODevice::ReadOnly) && metadata.size() <= 1048576 &&
			QJsonDocument::fromJson(metadata.readAll()).isObject();
	metadata.close();
	if (!valid) {
		Complete({}, tr("The extracted package is not a valid PS5 game folder (eboot.bin and "
						"sce_sys/param.json are required)."));
		return;
	}
	// The owned staging folder and destination are on the same filesystem. Never merge or
	// overwrite.
	if (QFileInfo::exists(m_destination) || QFileInfo(m_destination).isSymLink() ||
		!QDir().rename(game, m_destination)) {
		Complete({}, tr("Could not finish installation. The destination may exist or the games "
						"folder is not writable."));
		return;
	}
	Complete(m_destination, {});
}

void PkgInstaller::Complete(const QString &installed, const QString &error) {
	if (m_finished)
		return;
	m_finished = true;
	m_inspection_timer.stop();
	m_passcode.clear();
	m_process.setArguments({});
	m_staging.reset();
	emit Finished(installed, error);
}
