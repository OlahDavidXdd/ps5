#ifndef PKG_INSTALL_DIALOG_H
#define PKG_INSTALL_DIALOG_H
#include <QDialog>
class PkgInstaller;
class QLineEdit;
class QLabel;
class QProgressBar;
class QPushButton;
class PkgInstallDialog : public QDialog {
	Q_OBJECT
  public:
	explicit PkgInstallDialog(const QString &library, QWidget *parent = nullptr);
	[[nodiscard]] QString LibraryDirectory() const;
	void reject() override;

  private:
	void Install();
	QLineEdit *m_package, *m_library, *m_passcode, *m_helper;
	QWidget *m_inputs;
	QLabel *m_status;
	QProgressBar *m_progress;
	QPushButton *m_install;
	PkgInstaller *m_installer = nullptr;
	bool m_running = false;
};
#endif
