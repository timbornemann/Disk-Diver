#include "updates.h"

#include <QAction>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr auto checkInterval = 6 * 60 * 60;
enum class Operation { Idle, Fetch, Verify, Install };

class Updates final : public QObject {
public:
    Updates(QMainWindow &window, const QString &helperDirectory, const QString &python, bool enabled)
        : QObject(&window), window_(window), helperDirectory_(helperDirectory), python_(python), enabled_(enabled) {
        auto *menu = window.menuBar()->addMenu(tr("&Updates"));
        auto *version = menu->addAction(tr("Installed version %1").arg(QCoreApplication::applicationVersion()));
        version->setEnabled(false);
        check_ = menu->addAction(tr("Check for Updates…"));
        install_ = menu->addAction(tr("Install Downloaded Update…"));
        auto *automatic = menu->addAction(tr("Download Updates Automatically"));
        automatic->setCheckable(true);
        automatic->setChecked(enabled_ && settings_.value("updates/automatic", true).toBool());
        automatic->setEnabled(enabled_);
        pending_ = settings_.value("updates/package").toString();
        install_->setVisible(enabled_ && QFileInfo(pending_).isFile());

        connect(check_, &QAction::triggered, this, [this] { check(true); });
        connect(install_, &QAction::triggered, this, [this] { verify(); });
        connect(automatic, &QAction::toggled, this, [this](bool enabled) {
            settings_.setValue("updates/automatic", enabled);
            if (enabled) check(false);
            else if (operation_ == Operation::Fetch && !manual_) process_.kill();
        });
        connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit ? code : -1);
        });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(-1);
        });
        if (enabled_) {
            auto *timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [this] { check(false); });
            timer->start(checkInterval * 1000);
            QTimer::singleShot(2500, this, [this] { check(false); });
        }
    }

    ~Updates() override {
        process_.disconnect(this);
        if (process_.state() != QProcess::NotRunning) {
            process_.kill();
            process_.waitForFinished();
        }
#ifdef Q_OS_WIN
        unlockInstaller();
#endif
    }

private:
    void message(const QString &text, QMessageBox::Icon icon = QMessageBox::Information) {
        auto *box = new QMessageBox(icon, tr("Disk-Diver · Updates"), {}, QMessageBox::Ok, &window_);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setTextFormat(Qt::PlainText);
        box->setText(text);
        box->open();
    }

    void check(bool manual) {
        if (operation_ != Operation::Idle) return;
        if (!enabled_) {
            if (manual) message(tr("This build has no configured source for signed updates."));
            return;
        }
        const auto now = QDateTime::currentSecsSinceEpoch();
        const auto last = settings_.value("updates/lastCheck", 0).toLongLong();
        if (!manual && (!settings_.value("updates/automatic", true).toBool() ||
                        (last <= now && now - last < checkInterval))) return;
        manual_ = manual;
        settings_.setValue("updates/lastCheck", now);
        run(Operation::Fetch, "updater.py", {"fetch"});
    }

    void verify() {
        if (operation_ != Operation::Idle || pending_.isEmpty()) return;
        manual_ = true;
#ifdef Q_OS_WIN
        // Keep the verified executable open without write/delete sharing through ShellExecute.
        installerLock_ = CreateFileW(reinterpret_cast<LPCWSTR>(pending_.utf16()), GENERIC_READ,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (installerLock_ == INVALID_HANDLE_VALUE) {
            message(tr("Could not open the installer. Check for updates again."), QMessageBox::Warning);
            return;
        }
#endif
        run(Operation::Verify, "updater.py", {"verify", pending_});
    }

    void run(Operation operation, const QString &script, const QStringList &arguments) {
        operation_ = operation;
        check_->setEnabled(false);
        install_->setEnabled(false);
        check_->setText(operation == Operation::Install ? tr("Installing update…") : tr("Checking update…"));
        QStringList args = {"-I", "-B", helperDirectory_.filePath(script)};
        args.append(arguments);
        process_.setWorkingDirectory(helperDirectory_.absolutePath());
        process_.start(python_, args);
    }

    void finish(int code) {
        auto operation = operation_;
        if (operation == Operation::Idle) return;
        operation_ = Operation::Idle;
        check_->setEnabled(true);
        install_->setEnabled(true);
        check_->setText(tr("Check for Updates…"));
        const auto response = QJsonDocument::fromJson(process_.readAllStandardOutput()).object();
        const auto detail = QString::fromUtf8(process_.readAllStandardError()).trimmed().right(1000);
        const auto state = response.value("status").toString();
        const auto path = response.value("path").toString();
        if (operation == Operation::Install) {
            if (code == 0) {
                forgetPackage();
                message(tr("Update installed. Restart Disk-Diver to use the new version."));
            } else if (code != 126) {
                message(tr("The package manager could not install the update.\n%1").arg(detail), QMessageBox::Warning);
            }
            return;
        }
        if (code == 0 && state == "ready" && QFileInfo(path).isAbsolute() && QFileInfo(path).isFile()) {
            if (operation == Operation::Fetch) {
                pending_ = path;
                settings_.setValue("updates/package", pending_);
                install_->setVisible(true);
                offer(response.value("version").toString());
                return;
            }
            if (path == pending_) {
                launchInstaller();
                return;
            }
        }
#ifdef Q_OS_WIN
        unlockInstaller();
#endif
        if (operation == Operation::Verify || state == "current") forgetPackage();
        if (!manual_) return;
        if (code == 0 && state == "current") {
            message(tr("You have the latest version (%1).").arg(QCoreApplication::applicationVersion()));
        } else if (code == 0 && state == "disabled") {
            message(tr("This build has no configured source for signed updates."));
        } else {
            auto reason = response.value("message").toString(detail);
            if (reason.isEmpty()) reason = process_.errorString();
            message(tr("Could not check the update.\n%1").arg(reason), QMessageBox::Warning);
        }
    }

    void forgetPackage() {
        pending_.clear();
        settings_.remove("updates/package");
        install_->setVisible(false);
    }

    void offer(const QString &version) {
        if (!manual_ && version == offered_) return;
        offered_ = version;
        auto *box = new QMessageBox(QMessageBox::Information, tr("Disk-Diver · Update Ready"), {},
                                    QMessageBox::NoButton, &window_);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setTextFormat(Qt::PlainText);
        box->setText(tr("Disk-Diver downloaded and verified version %1.").arg(version));
#ifdef Q_OS_WIN
        box->setInformativeText(tr("Finish any active scan before continuing. The Windows installer will guide you through the update."));
#else
        box->setInformativeText(tr("Your package manager will ask for permission to install the update."));
#endif
        auto *install = box->addButton(tr("Install Now"), QMessageBox::AcceptRole);
        box->addButton(tr("Later"), QMessageBox::RejectRole);
        connect(box, &QMessageBox::buttonClicked, this, [this, install, path = pending_](QAbstractButton *button) {
            if (button == install && path == pending_) verify();
        });
        box->open();
    }

    void launchInstaller() {
#ifdef Q_OS_WIN
        const std::wstring file = QDir::toNativeSeparators(pending_).toStdWString();
        const std::wstring directory = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString();
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOASYNC;
        launch.hwnd = reinterpret_cast<HWND>(window_.winId());
        launch.lpVerb = L"open";
        launch.lpFile = file.c_str();
        launch.lpDirectory = directory.c_str();
        launch.nShow = SW_SHOWNORMAL;
        const bool started = ShellExecuteExW(&launch);
        unlockInstaller();
        if (!started) {
            message(tr("Could not start the Windows installer. Try again from the Updates menu."),
                    QMessageBox::Warning);
        } else {
            window_.close();
        }
#else
        run(Operation::Install, "install_update.py", {pending_});
#endif
    }

#ifdef Q_OS_WIN
    void unlockInstaller() {
        if (installerLock_ != INVALID_HANDLE_VALUE) CloseHandle(installerLock_);
        installerLock_ = INVALID_HANDLE_VALUE;
    }
    HANDLE installerLock_ = INVALID_HANDLE_VALUE;
#endif
    QMainWindow &window_;
    QDir helperDirectory_;
    QString python_;
    bool enabled_;
    QSettings settings_;
    QProcess process_;
    QAction *check_, *install_;
    Operation operation_ = Operation::Idle;
    QString pending_, offered_;
    bool manual_ = false;
};
}

void setupUpdates(QMainWindow &window, const QString &helperDirectory, const QString &python, bool enabled) {
    new Updates(window, helperDirectory, python, enabled);
}
