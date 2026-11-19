#include "pkgInstaller.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <cstdlib>

static void Check(bool value, const char *message) {
	if (!value) {
		std::fprintf(stderr, "PkgInstallerTests: %s\n", message);
		std::abort();
	}
}
static void Write(const QString &path, const QByteArray &bytes) {
	Check(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture parent");
	QFile file(path);
	Check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
static void Json(const QJsonObject &object) {
	const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
	std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
	std::fflush(stdout);
}
// The same executable acts as an independent extractor process. No console or game files needed.
static int Mock(const QStringList &args) {
	const auto mode = QFileInfo(args.at(2)).completeBaseName();
	if (args.at(1) == QStringLiteral("info")) {
		const bool unsupported =
			QStringList{"retail", "patch", "metadata", "unknown"}.contains(mode);
		Json({{"type", "done"},
			  {"kind", unsupported ? mode : QStringLiteral("fpkg")},
			  {"unpackable", !unsupported}});
		return 0;
	}
	const auto output = args.at(3);
	Write(QDir(qEnvironmentVariable("TMPDIR")).filePath("decoded-image.tmp"), "cache");
	Write(QDir(output).filePath("eboot.bin"), "ELF fixture");
	Write(QDir(output).filePath("sce_sys/param.json"), "{\"titleId\":\"PPSATEST\"}");
	Write(QDir(QFileInfo(output).absolutePath()).filePath("internal.extracting/partial"),
		  "partial");
	if (mode == "cancel") {
		Json({{"type", "progress"}, {"percent", 10}, {"total", 100}});
		QThread::msleep(30000);
	}
	if (mode == "fail") {
		Json({{"type", "error"}, {"code", "passcode"}, {"message", "Wrong passcode"}});
		return 1;
	}
	if (mode == "flood") {
		const QByteArray output(300000, 'x');
		std::fwrite(output.constData(), 1, output.size(), stdout);
		return 0;
	}
	if (mode == "badjson") {
		std::puts("broken response");
		return 0;
	}
	if (mode == "badmetadata")
		Write(QDir(output).filePath("sce_sys/param.json"), "broken");
	if (mode == "missingdone")
		return 0;
	if (mode == "missinggame")
		QFile::remove(QDir(output).filePath("eboot.bin"));
#ifndef _WIN32
	if (mode == "symlink") {
		QFile::remove(QDir(output).filePath("eboot.bin"));
		Check(QFile::link(args.at(2), QDir(output).filePath("eboot.bin")),
			  "create unsafe symlink fixture");
	}
#endif
	Json({{"type", "done"}, {"dest", output}, {"files", 2}});
	return 0;
}
struct Result {
	QString installed, error;
	bool canceled = false;
};
static Result Run(const QString &helper, const QString &package, const QString &library,
				  const QString &passcode = {}, bool cancel = false) {
	PkgInstaller installer(helper);
	QEventLoop loop;
	Result result;
	bool complete = false;
	QObject::connect(&installer, &PkgInstaller::Finished, &loop,
					 [&](const QString &path, const QString &error) {
						 complete = true;
						 result = {path, error, installer.WasCanceled()};
						 loop.quit();
					 });
	if (cancel)
		QObject::connect(&installer, &PkgInstaller::Progress, &loop,
						 [&](int percent, const QString &) {
							 if (percent >= 0)
								 installer.Cancel();
						 });
	QTimer timer;
	timer.setSingleShot(true);
	QObject::connect(&timer, &QTimer::timeout, &loop, [&] { Check(false, "installer timed out"); });
	timer.start(10000);
	installer.Start(package, library, passcode);
	if (!complete)
		loop.exec();
	Check(complete, "must emit completion exactly once");
	return result;
}
int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	const auto args = app.arguments();
	if (args.size() >= 3 && (args.at(1) == "info" || args.at(1) == "extract"))
		return Mock(args);
	QTemporaryDir temporary;
	Check(temporary.isValid(), "create temporary directory");
	const auto library = temporary.filePath("library with spaces");
	Check(QDir().mkdir(library), "create library");
	const auto package = [&](const QString &name) {
		const auto path = temporary.filePath(name + ".pkg");
		Write(path, "package fixture");
		return path;
	};
	const auto clean = [&] {
		return QDir(library)
			.entryList({".pkg-install-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
			.isEmpty();
	};
	if (args.size() == 3 && args.at(1) == "--real-helper") {
		QProcess version;
		version.start(args.at(2), {"--version"});
		Check(version.waitForFinished(30000) && version.exitCode() == 0 &&
				  QJsonDocument::fromJson(version.readAllStandardOutput()).object().value("type") ==
					  "version",
			  "bundled extractor version smoke test");
		const auto result = Run(args.at(2), package("invalid"), library);
		Check(result.installed.isEmpty() && !result.error.isEmpty() && clean(),
			  "real extractor rejects non-PKG input");
		return 0;
	}
	const auto helper = QCoreApplication::applicationFilePath();
	const auto source = package("success game");
	const auto success = Run(helper, source, library, QString(32, 'A'));
	Check(success.error.isEmpty() &&
			  QFileInfo(QDir(success.installed).filePath("eboot.bin")).isFile(),
		  "install game with spaces and custom passcode");
	Check(QFileInfo(source).size() == 15 && clean(),
		  "preserve original and clean nested staging files");
	const auto quotedLibrary = temporary.filePath("quoted library");
	Check(QDir().mkdir(quotedLibrary), "create quoted-path library fixture");
	const auto quotedSource = package("backup [test]");
	const auto quote = [](const QString &path) {
		return QStringLiteral("\"") + path + QStringLiteral("\"");
	};
	const auto quoted = Run(quote(helper), quote(quotedSource), quote(quotedLibrary));
	Check(quoted.error.isEmpty() &&
			  QFileInfo(QDir(quoted.installed).filePath("eboot.bin")).isFile(),
		  "accept Windows Copy as path quotes in all path fields");
	const auto uriLibrary = temporary.filePath(QString::fromUtf8("URL library 資料"));
	Check(QDir().mkdir(uriLibrary), "create file URL library fixture");
	const auto uriSource = package("file URL game");
	const auto uri = Run(helper, QUrl::fromLocalFile(uriSource).toString(QUrl::FullyEncoded),
						 QUrl::fromLocalFile(uriLibrary).toString(QUrl::FullyEncoded));
	Check(uri.error.isEmpty() && !uri.installed.isEmpty(),
		  "accept local file URLs and Unicode paths");
	Check(PkgInstaller::NormalizePathInput(QString(QChar(0x202a)) + quote(source) +
										   QChar(0x202c)) == QFileInfo(source).absoluteFilePath(),
		  "handle copied direction marks around quoted Windows paths");
#ifndef _WIN32
	const auto literalQuotes = package("literal \"quoted\" filename");
	Check(PkgInstaller::NormalizePathInput(literalQuotes) == literalQuotes,
		  "preserve real filenames containing quotes");
#endif
	const auto missingFile = temporary.filePath("absent.pkg");
	const auto badSource = Run(helper, quote(missingFile), library);
	Check(badSource.error.contains(missingFile) && !badSource.error.contains("games folder"),
		  "identify the inaccessible PKG rather than blaming both fields");
	const auto missingFolder = temporary.filePath("absent folder");
	const auto badFolder = Run(helper, source, quote(missingFolder));
	Check(badFolder.error.contains(missingFolder) && badFolder.error.contains("games folder"),
		  "identify the inaccessible games folder");
	const auto collision = Run(helper, source, library);
	Check(collision.installed.isEmpty() && !collision.error.isEmpty(),
		  "never overwrite installed game");
	for (const auto &mode : {"retail", "patch", "metadata", "unknown", "fail", "badjson", "flood",
							 "badmetadata", "missingdone", "missinggame"}) {
		const auto failed = Run(helper, package(mode), library);
		Check(failed.installed.isEmpty() && !failed.error.isEmpty() && clean(),
			  "reject invalid packages and clean all partial files");
		Check(!QFileInfo::exists(QDir(library).filePath(mode)), "no failed game published");
		if (QStringList{"retail", "patch", "metadata", "unknown"}.contains(
				QString::fromLatin1(mode)))
			Check(failed.error.contains(QString::fromLatin1(mode)),
				  "report the detected package kind instead of a generic unsupported message");
	}
#ifndef _WIN32
	const auto symlink = Run(helper, package("symlink"), library);
	Check(symlink.installed.isEmpty() && !symlink.error.isEmpty() && clean(),
		  "reject extracted symlinks");
#endif
	const auto canceled = Run(helper, package("cancel"), library, {}, true);
	Check(canceled.canceled && canceled.installed.isEmpty() && clean(),
		  "cancel kills extractor and removes partial files");
	{
		PkgInstaller beforeLaunch(helper);
		bool finished = false;
		QObject::connect(&beforeLaunch, &PkgInstaller::Progress, &app,
						 [&](int, const QString &) { beforeLaunch.Cancel(); });
		QObject::connect(&beforeLaunch, &PkgInstaller::Finished, &app,
						 [&](const QString &path, const QString &) {
							 finished = path.isEmpty() && beforeLaunch.WasCanceled();
						 });
		beforeLaunch.Start(package("early-cancel"), library);
		Check(finished && clean(), "cancellation before process launch leaves no partial files");
	}
	const auto invalidPasscode = Run(helper, package("passcode"), library, "short");
	Check(!invalidPasscode.error.isEmpty() && clean(), "validate passcode before extraction");
	const auto missingLibrary = Run(helper, package("no-library"), {});
	Check(!missingLibrary.error.isEmpty() && missingLibrary.installed.isEmpty(),
		  "require an explicit games folder");
	const auto missing = Run(temporary.filePath("missing-helper"), package("missing"), library);
	Check(!missing.error.isEmpty() && clean(), "missing extractor fails cleanly");
	std::puts("PkgInstallerTests: passed");
}
