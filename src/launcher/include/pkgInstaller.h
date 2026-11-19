#ifndef PKG_INSTALLER_H
#define PKG_INSTALLER_H

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// Runs the independent PlayOutPs5Pkg CLI; no extractor code is linked into the launcher.
class PkgInstaller : public QObject {
	Q_OBJECT
  public:
	explicit PkgInstaller(QString helper, QObject *parent = nullptr);
	~PkgInstaller() override;
	void Start(const QString &package, const QString &library, const QString &passcode = {});
	void Cancel();
	[[nodiscard]] bool WasCanceled() const { return m_canceled; }
  signals:
	void Progress(int percent, const QString &message);
	void Finished(const QString &installed_directory, const QString &error);

  private:
	void ReadOutput();
	void ProcessFinished(int code, QProcess::ExitStatus status);
	void Complete(const QString &installed, const QString &error);
	QString m_helper, m_package, m_passcode, m_destination, m_error;
	QProcess m_process;
	QTimer m_inspection_timer;
	std::unique_ptr<QTemporaryDir> m_staging;
	QByteArray m_output;
	bool m_started = false, m_finished = false, m_inspecting = true, m_done = false;
	bool m_supported = false, m_canceled = false;
};
#endif
