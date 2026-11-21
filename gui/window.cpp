#include "index.h"
#include "entry_actions.h"
#include "stream.h"
#include "treemap.h"
#include "window.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>
#include <QtEndian>
#include <algorithm>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

using diver::Id;
using diver::none;
using diver::Index;

namespace {

QString entryCount(quint64 count) {
    if (count == 1) return QStringLiteral("1 entry");
    return QStringLiteral("%1 entries").arg(QLocale().toString(count));
}

class SizeTable final : public QTableView {
public:
    void setSizeMetric(bool allocated) {
        allocated_ = allocated;
        fitColumns();
    }
    void fitColumns() {
        const int width = viewport()->width();
        const int amount = fontMetrics().horizontalAdvance(QStringLiteral("≥ 999.9 MiB")) + 16;
        const int minimumName = fontMetrics().horizontalAdvance(QStringLiteral("Downloads")) + 24;
        const bool showBoth = width >= minimumName + 2 * amount;
        const int type = std::clamp(width / 5, 82, 124);
        const bool showType = width >= minimumName + 2 * amount + type;
        setColumnHidden(1, !showBoth && !allocated_);
        setColumnHidden(2, !showBoth && allocated_);
        setColumnHidden(3, !showType);
        setColumnWidth(0, std::max(40, width - (showBoth ? 2 : 1) * amount - (showType ? type : 0)));
        if (!isColumnHidden(1)) setColumnWidth(1, amount);
        if (!isColumnHidden(2)) setColumnWidth(2, amount);
        if (showType) setColumnWidth(3, type);
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QTableView::resizeEvent(event);
        fitColumns();
    }
private:
    bool allocated_ = true;
};

class ResultsSplitter final : public QSplitter {
protected:
    void showEvent(QShowEvent *event) override {
        QSplitter::showEvent(event);
        if (initial_) {
            const int available = width() - handleWidth();
            const int mapWidth = available * 4 / 5;
            setSizes({mapWidth, available - mapWidth});
            initial_ = false;
        }
    }
private:
    bool initial_ = true;
};

class Table final : public QAbstractTableModel {
public:
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(ids.size());
    }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 4; }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        static const QStringList labels{tr("Name"), tr("Allocated"), tr("Logical"), tr("Type")};
        if (o == Qt::Horizontal && role == Qt::DisplayRole) return labels.value(section);
        if (o == Qt::Horizontal && role == Qt::TextAlignmentRole && (section == 1 || section == 2))
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    QVariant data(const QModelIndex &cell, int role) const override {
        if (!index_ || !cell.isValid() || cell.row() >= rowCount()) return {};
        Id id = ids[cell.row()];
        const auto &node = (*index_)[id];
        if (role == Qt::TextAlignmentRole && (cell.column() == 1 || cell.column() == 2))
            return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::ToolTipRole) {
            if (node.alias) {
                const auto &sizes = index_->aliasSizes(id);
                return tr("Hard link · Logical size %1, allocated size %2. Both sizes count toward the first path in name order.")
                        .arg(diver::formatSize(sizes.logical), sizes.allocated == diver::unknown
                             ? tr("unknown") : diver::formatSize(sizes.allocated));
            }
            if (node.errors) return tr("Could not read %1.").arg(entryCount(node.errors));
            if (node.missingSizes) return tr("Allocated size is unknown for %1.").arg(entryCount(node.missingSizes));
            if (cell.column() == 0) return QStringLiteral("<p>%1</p>").arg(index_->name(id).toHtmlEscaped());
            return {};
        }
        if (role != Qt::DisplayRole) return {};
        switch (cell.column()) {
        case 0: {
            const auto name = index_->name(id);
            return name.isEmpty() && node.kind == diver::Error ? tr("(Folder contents inaccessible)") : name;
        }
        case 1:
            if (node.kind == diver::Error || (node.missingSizes && node.kind != diver::Directory))
                return tr("Unknown");
            return (node.missingSizes || node.errors ? QStringLiteral("≥ ") : QString{})
                    + diver::formatSize(node.allocated);
        case 2:
            if (node.kind == diver::Error) return tr("Unknown");
            return (node.errors ? QStringLiteral("≥ ") : QString{}) + diver::formatSize(node.logical);
        case 3:
            if (node.alias) return tr("Hard link");
            if (node.kind == diver::Directory) return node.errors ? tr("Folder (incomplete)") : tr("Folder");
            if (node.kind == diver::Error) return tr("Read error");
            return node.kind == diver::File ? tr("File") : tr("Link / other");
        }
        return {};
    }
    void replace(const Index *index, std::vector<Id> rows) {
        beginResetModel();
        index_ = index;
        ids = std::move(rows);
        endResetModel();
    }
    std::vector<Id> ids;
private:
    const Index *index_ = nullptr;
};

QString workerPath() {
    const auto sibling = QDir(QCoreApplication::applicationDirPath()).filePath("worker.py");
    if (QFileInfo::exists(sibling)) return sibling;
    return QString::fromUtf8(DISK_DIVER_WORKER_PATH);
}

QStringList pythonCommand() {
#ifdef Q_OS_WIN
    QString program = QDir(QCoreApplication::applicationDirPath()).filePath("python/python.exe");
    if (QFileInfo::exists(program)) return {program};
    program = QStandardPaths::findExecutable("py");
    if (!program.isEmpty()) return {program, "-3"};
    program = QStandardPaths::findExecutable("python");
#else
    QString program = QStandardPaths::findExecutable("python3");
#endif
    if (!program.isEmpty()) return {program};
    return {};
}

class Window final : public QMainWindow {
public:
    Window(bool useCache, ViewTiming *timing) : timing_(timing) {
        setWindowTitle("Disk-Diver");
        resize(1040, 680);
        auto *central = new QWidget;
        auto *layout = new QVBoxLayout(central);
        auto addControlRow = [layout] {
            auto *row = new QWidget;
            // Let the window shrink to its results area. The row switches to its
            // compact controls as soon as it receives the narrower geometry.
            row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            auto *line = new QHBoxLayout(row);
            line->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(row);
            return line;
        };
        auto *tools = addControlRow();
        sourceLabel_.setBuddy(&roots_);
        tools->addWidget(&sourceLabel_);
        roots_.setObjectName("scanSource");
        roots_.setAccessibleName(tr("Drive or folder to scan"));
        roots_.setEditable(true);
        roots_.setInsertPolicy(QComboBox::NoInsert);
        roots_.setCompleter(nullptr);
        roots_.setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        roots_.setMinimumContentsLength(9);
        roots_.lineEdit()->setClearButtonEnabled(true);
        roots_.lineEdit()->setPlaceholderText(tr("Drive or folder to scan"));
        roots_.setToolTip(tr("Select a drive or enter a folder path (Ctrl+L)."));
        for (const auto &volume : QStorageInfo::mountedVolumes()) {
            if (!volume.isValid() || !volume.isReady() || volume.bytesTotal() <= 0) continue;
            QString root = QDir::cleanPath(volume.rootPath());
            if (roots_.findData(root) >= 0) continue;
            roots_.addItem(QDir::toNativeSeparators(root), root);
        }
        if (roots_.count() == 0) roots_.addItem(QDir::rootPath(), QDir::rootPath());
        tools->addWidget(&roots_, 1);
        auto *focusSource = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
        connect(focusSource, &QShortcut::activated, this, [this] {
            if (!roots_.isEnabled()) return;
            roots_.setFocus(Qt::ShortcutFocusReason);
            roots_.lineEdit()->selectAll();
        });
        browse_.setText(tr("Choose folder…"));
        browse_.setShortcut(QKeySequence::Open);
        browse_.setToolTip(tr("Choose a folder to scan (Ctrl+O)."));
        tools->addWidget(&browse_);
        auto primaryFont = scan_.font();
        primaryFont.setBold(true);
        scan_.setFont(primaryFont);
        scan_.setDefault(true);
        scan_.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
        scan_.setToolTip(tr("Scan this drive or folder (Ctrl+Enter)."));
        tools->addWidget(&scan_);
        stop_.setShortcut(QKeySequence(Qt::Key_Escape));
        stop_.setToolTip(tr("Cancel the current operation (Esc)."));
        tools->addWidget(&stop_);
        auto *settings = new QMenu(&options_);
        settings->setToolTipsVisible(true);
        settings->addAction(&full_);
        settings->addAction(&cache_);
        settings->addSeparator();
        settings->addAction(&clear_);
        full_.setCheckable(true);
        cache_.setCheckable(true);
        cache_.setChecked(useCache);
        full_.setToolTip(tr("Request elevated privileges for this scan."));
        cache_.setToolTip(tr("Save scan results locally and check them for changes on the next scan."));
        clear_.setToolTip(tr("Clear saved results for the selected privilege level."));
        options_.setText(tr("Scan options"));
        options_.setToolTip(tr("Change scan privileges or manage the scan cache."));
        options_.setPopupMode(QToolButton::InstantPopup);
        options_.setMenu(settings);
        tools->addWidget(&options_);
        // The source grows on wider monitors; keep its actions beside the field.
        tools->addStretch(0);
        auto *divider = new QFrame;
        divider->setFrameShape(QFrame::HLine);
        divider->setFrameShadow(QFrame::Sunken);
        layout->addWidget(divider);
        auto *navigation = addControlRow();
        back_.setText(tr("Back"));
        back_.setToolTip(tr("Go back to the previous folder."));
        up_.setText(tr("Up"));
        up_.setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
        up_.setToolTip(tr("Go to the parent folder (Alt+Up)."));
        navigation->addWidget(&back_);
        navigation->addWidget(&up_);
        path_.setReadOnly(true);
        path_.setFrame(false);
        path_.setStyleSheet(QStringLiteral("background: transparent;"));
        path_.setAccessibleName(tr("Current folder"));
        path_.setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        navigation->addWidget(&path_, 1);
        navigation->addWidget(&folderSize_);
        navigation->addStretch(0);
        auto *separator = new QFrame;
        separator->setFrameShape(QFrame::VLine);
        separator->setFrameShadow(QFrame::Sunken);
        navigation->addWidget(separator);
        metric_.addItems({tr("Allocated space"), tr("Logical size")});
        metric_.setAccessibleName(tr("Size metric"));
        metric_.setToolTip(tr("Allocated space is the number of bytes the file system reports. Hard links count once. "
                              "Files that share blocks through reflinks can count the same blocks more than once. "
                              "The space freed by deleting a file depends on which links and shared blocks remain."));
        navigation->addWidget(&metric_);
        reveal_.setText(tr("Show in file manager"));
        reveal_.setToolTip(tr("Show the selected item or current folder in the file manager."));
        navigation->addWidget(&reveal_);
        for (QToolButton *button : {&browse_, &back_, &up_, &reveal_, &options_}) {
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::StrongFocus);
            button->setAccessibleName(button->text());
        }
        updateControlIcons();
        updateControlLayout();
        auto *split = new ResultsSplitter;
        split->addWidget(&map_);
        split->addWidget(&table_);
        split->setChildrenCollapsible(false);
        split->setStretchFactor(0, 1);
        split->setStretchFactor(1, 1);
        layout->addWidget(split, 1);
        table_.setModel(&model_);
        table_.setSelectionBehavior(QAbstractItemView::SelectRows);
        table_.setSelectionMode(QAbstractItemView::SingleSelection);
        table_.setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_.verticalHeader()->hide();
        table_.setMinimumWidth(table_.fontMetrics().horizontalAdvance(QStringLiteral("≥ 999.9 MiB")) + 80);
        table_.horizontalHeader()->setMinimumSectionSize(40);
        table_.horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_.horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table_.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        table_.fitColumns();
        table_.setAlternatingRowColors(true);
        auto *foot = new QHBoxLayout;
        layout->addLayout(foot);
        phase_.setTextFormat(Qt::PlainText);
        status_.setTextFormat(Qt::PlainText);
        phase_.setWordWrap(true);
        status_.setWordWrap(true);
        foot->addWidget(&phase_);
        foot->addWidget(&status_, 1);
        foot->addWidget(&count_);
        setCentralWidget(central);
        connect(&roots_, &QComboBox::editTextChanged, this, [this] { updateActions(); });
        connect(roots_.lineEdit(), &QLineEdit::returnPressed, this, [this] { start(false); });
        connect(&browse_, &QToolButton::clicked, this, [this] {
            QString folder = QFileDialog::getExistingDirectory(this, tr("Choose a folder to scan"),
                    roots_.currentText(), QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
            if (!folder.isEmpty()) { setRoot(folder); start(false); }
        });
        connect(&scan_, &QPushButton::clicked, this, [this] { start(false); });
        connect(&stop_, &QPushButton::clicked, this, [this] { cancel(); });
        connect(&clear_, &QAction::triggered, this, [this] { start(true); });
        connect(&up_, &QToolButton::clicked, this, [this] { navigate((*shown_)[folder_].parent); });
        connect(&back_, &QToolButton::clicked, this, [this] { goBack(); });
        auto *backKey = new QShortcut(QKeySequence::Back, this);
        connect(backKey, &QShortcut::activated, this, [this] { goBack(); });
        connect(&metric_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
        connect(&reveal_, &QToolButton::clicked, this, [this] { revealSelection(); });
        connect(&table_, &QTableView::activated, this, [this](const QModelIndex &cell) {
            activate(model_.ids[cell.row()]);
        });
        connect(table_.selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex &cell) {
            selected_ = cell.isValid() ? model_.ids[cell.row()] : none;
            map_.setSelection(selected_);
            updateActions();
        });
        map_.selected = [this](Id id) { select(id); };
        map_.activated = [this](Id id) { activate(id); };
        map_.setContextMenuPolicy(Qt::CustomContextMenu);
        connect(&map_, &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
            entryMenu(map_.entryAt(pos), map_.mapToGlobal(pos));
        });
        table_.setContextMenuPolicy(Qt::CustomContextMenu);
        connect(&table_, &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
            const auto cell = table_.indexAt(pos);
            if (cell.isValid()) entryMenu(model_.ids.at(cell.row()), table_.viewport()->mapToGlobal(pos));
        });
        connect(&worker_, &QProcess::readyReadStandardOutput, this, [this] { scheduleDrain(); });
        connect(&worker_, &QProcess::readyReadStandardError, this, [this] { readMessage(false); });
        connect(&worker_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus how) {
            exited_ = true;
            exitCode_ = how == QProcess::NormalExit ? code : -1;
            readMessage(true);
            scheduleDrain();
        });
        connect(&worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                running_ = false;
                status_.setText(worker_.errorString());
                if (timing_) timing_->failure = status_.text();
                updateActions();
            }
        });
        repaintTimer_.setInterval(250);
        connect(&repaintTimer_, &QTimer::timeout, this, [this] {
            if (dirty_) refresh();
            updateCount();
        });
        repaintTimer_.start();
        status_.setText(tr("Select a drive or folder to scan."));
        refresh();
    }

    void setRoot(const QString &path) {
        const auto absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        int i = roots_.findData(absolute);
        if (i < 0) {
            roots_.addItem(QDir::toNativeSeparators(absolute), absolute);
            i = roots_.count() - 1;
        }
        roots_.setCurrentIndex(i);
        roots_.setEditText(roots_.itemText(i));
    }
    void scanPath(const QString &path) { setRoot(path); start(false); }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        updateControlLayout();
    }
    void changeEvent(QEvent *event) override {
        QMainWindow::changeEvent(event);
        if (centralWidget() && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
            updateControlIcons();
            updateControlLayout();
        }
    }
    void closeEvent(QCloseEvent *event) override {
        if (worker_.state() != QProcess::NotRunning) {
            cancel();
            worker_.waitForBytesWritten(150);
            if (!worker_.waitForFinished(2000)) { worker_.kill(); worker_.waitForFinished(500); }
        }
        event->accept();
    }

private:
    void updateControlIcons() {
        browse_.setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
        scan_.setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        back_.setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
        up_.setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
        reveal_.setIcon(style()->standardIcon(QStyle::SP_DirLinkIcon));
    }
    void updateControlLayout() {
        const int em = fontMetrics().horizontalAdvance(QLatin1Char('M'));
        const bool compact = width() < 68 * em;
        sourceLabel_.setVisible(!compact);
        roots_.setMaximumWidth(62 * em);
        path_.setMaximumWidth(std::min(80 * em, path_.fontMetrics().horizontalAdvance(path_.text()) + em));
        for (QToolButton *button : {&browse_, &back_, &up_, &reveal_}) {
            button->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        }
    }
    void updateActions() {
        const bool focusCancel = running_ && !scan_.isHidden() &&
                                 (!focusWidget() || scan_.hasFocus() || roots_.hasFocus() || browse_.hasFocus());
        const bool focusScan = !running_ && stop_.hasFocus();
        scan_.setEnabled(!running_ && !roots_.currentText().isEmpty());
        scan_.setVisible(!running_);
        stop_.setVisible(running_);
        stop_.setEnabled(running_ && !cancelling_);
        options_.setEnabled(!running_);
        roots_.setEnabled(!running_);
        browse_.setEnabled(!running_);
        if (focusCancel) stop_.setFocus(Qt::OtherFocusReason);
        if (focusScan) scan_.setFocus(Qt::OtherFocusReason);
        back_.setEnabled(!history_.empty());
        up_.setEnabled(shown_ && folder_ != none && (*shown_)[folder_].parent != none);
        reveal_.setEnabled(shown_ && folder_ != none);
    }
    void updateCount() {
        const auto *active = incoming_ ? incoming_ : shown_;
        if (active) count_.setText(entryCount(active->size()));
        else count_.clear();
    }
    void select(Id id) {
        if (id != none && shown_ && (*shown_)[id].parent == folder_) {
            const auto it = std::find(model_.ids.begin(), model_.ids.end(), id);
            if (it != model_.ids.end()) table_.selectRow(int(it - model_.ids.begin()));
        } else {
            table_.selectionModel()->clear();
        }
        selected_ = id;
        map_.setSelection(id);
        updateActions();
    }
    void activate(Id id) {
        if ((*shown_)[id].kind == diver::Directory) navigate(id);
        else { select(id); revealSelection(); }
    }
    void navigate(Id id) {
        if (id == none || id == folder_) return;
        restoreFocus_.clear();
        history_.push_back(folder_);
        folder_ = id;
        selected_ = none;
        refresh();
    }
    void goBack() {
        if (history_.empty()) return;
        restoreFocus_.clear();
        folder_ = history_.back();
        history_.pop_back();
        selected_ = none;
        refresh();
    }
    QString displayPath(Id id) const {
        QStringList components;
        while (id != none && (*shown_)[id].parent != none) {
            components.prepend(shown_->name(id));
            id = (*shown_)[id].parent;
        }
        return QDir::toNativeSeparators(QDir(rootPath_).filePath(components.join('/')));
    }
    void refresh() {
        dirty_ = false;
        if (shown_ && shown_->size() && folder_ == none) folder_ = 0;
        const bool allocated = metric_.currentIndex() == 0;
        if (shown_ && folder_ != none && (*shown_)[folder_].childCount > 8192) {
            if (renderedEpoch_ != epoch_ || renderedFolder_ != folder_ || renderedAllocated_ != allocated)
                renderRows({}, allocated);
            if (sorting_) resortRequested_ = true;
            else {
                // Only immutable IDs and weights cross the thread boundary. The live
                // tree can grow and additional navigation requests stay responsive.
                sorting_ = true;
                auto weights = shown_->weights(folder_, allocated);
                sortPool_.start([this, weights = std::move(weights), epoch = epoch_,
                                 parent = folder_, allocated]() mutable {
                    std::sort(weights.begin(), weights.end(), [](auto &a, auto &b) {
                        return a.size == b.size ? a.id < b.id : a.size > b.size;
                    });
                    std::vector<Id> ordered;
                    ordered.reserve(weights.size());
                    Treemap::Remainder remainder;
                    for (const auto &item : weights) {
                        if (ordered.size() >= Treemap::limit && item.size) {
                            ++remainder.count;
                            remainder.bytes += item.size;
                        }
                        ordered.push_back(item.id);
                    }
                    QMetaObject::invokeMethod(this, [this, ordered = std::move(ordered), remainder, epoch,
                                                      parent, allocated]() mutable {
                        sorting_ = false;
                        if (resortRequested_) {
                            dirty_ = true;
                            resortRequested_ = false;
                        }
                        if (epoch == epoch_ && parent == folder_ && allocated == (metric_.currentIndex() == 0))
                            renderRows(std::move(ordered), allocated, remainder);
                    }, Qt::QueuedConnection);
                });
            }
        } else renderRows(shown_ ? shown_->children(folder_, allocated) : std::vector<Id>{}, allocated);
        if (shown_ && folder_ != none) {
            const auto &n = (*shown_)[folder_];
            const auto folderPath = displayPath(folder_);
            if (path_.text() != folderPath) {
                path_.setText(folderPath);
                path_.setToolTip(folderPath);
                updateControlLayout();
            }
            const auto prefix = n.errors || (allocated && n.missingSizes) ? QStringLiteral("≥ ") : QString{};
            folderSize_.setText(prefix + diver::formatSize(allocated ? n.allocated : n.logical));
        } else {
            path_.clear();
            path_.setToolTip({});
            folderSize_.clear();
        }
        updateActions();
        updateCount();
    }
    void renderRows(std::vector<Id> rows, bool allocated, std::optional<Treemap::Remainder> remainder = {}) {
        renderedEpoch_ = epoch_;
        renderedFolder_ = folder_;
        renderedAllocated_ = allocated;
        const auto previous = selected_;
        model_.replace(shown_, std::move(rows));
        table_.setSizeMetric(allocated);
        map_.setItems(shown_, model_.ids, allocated, remainder);
        Id ancestor = shown_ && previous < shown_->size() ? (*shown_)[previous].parent : none;
        while (ancestor != none && ancestor != folder_) ancestor = (*shown_)[ancestor].parent;
        select(ancestor != none ? previous : none);
        if (timing_ && shown_ && folder_ != none) {
            const auto &node = (*shown_)[folder_];
            if (timing_->firstMs < 0 && (!model_.ids.empty() || (!incoming_ && !node.childCount))) {
                timing_->firstMs = scanClock_.elapsed();
                timing_->firstSource = showingPreview_ ? "cache" : "live";
            }
            // A very wide folder is sorted asynchronously. Only publish the final
            // measurement once the verified generation's actual rows are available.
            if (timing_->finalMs < 0 && timing_->failure.isEmpty() && !running_ && completedLive_ && !cancelling_
                    && exitCode_ == 0 && !protocolError_ && model_.ids.size() == node.childCount) {
                timing_->finalMs = scanClock_.elapsed();
                timing_->entries = shown_->size();
                timing_->errors = (*shown_)[0].errors;
                timing_->unknownSizes = (*shown_)[0].missingSizes;
            }
        }
    }

    void revealSelection() {
        if (!shown_ || folder_ == none) return;
        reveal(selected_ == none ? folder_ : selected_);
    }
    void reveal(Id id) {
#ifdef Q_OS_WIN
        const QString path = displayPath(id);
        PIDLIST_ABSOLUTE item = nullptr;
        HRESULT hr = SHParseDisplayName(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                        &item, 0, nullptr);
        if (SUCCEEDED(hr)) {
            hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            CoTaskMemFree(item);
        }
        if (FAILED(hr) && !QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())))
            status_.setText(tr("File Explorer could not show this path."));
#else
        auto raw = QFile::encodeName(rootPath_);
        const auto suffix = shown_->relativeName(id);
        if (!suffix.isEmpty()) {
            if (!raw.endsWith('/')) raw += '/';
            raw += suffix;
        }
        const auto uri = QString::fromLatin1("file://" + raw.toPercentEncoding("/"));
        const auto parent = raw.left(std::max<qsizetype>(1, raw.lastIndexOf('/')));
        const auto parentUrl = QUrl::fromEncoded("file://" + parent.toPercentEncoding("/"));
        QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1",
                "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems");
        call << QStringList{uri} << QString{};
        auto *watch = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
        connect(watch, &QDBusPendingCallWatcher::finished, this, [this, parentUrl, watch] {
            QDBusPendingReply<> reply = *watch;
            if (reply.isError() && !QDesktopServices::openUrl(parentUrl))
                status_.setText(tr("The file manager could not show this folder."));
            watch->deleteLater();
        });
#endif
    }

    bool canChange(Id id) const {
        return !running_ && !showingPreview_ && completedLive_ && !cancelling_ && !protocolError_
                && exitCode_ == 0 && !(*shown_)[id].errors;
    }
    void entryMenu(Id id, QPoint position) {
        if (!shown_) return;
        const auto target = diver::entryTarget(*shown_, rootPath_, id);
        if (target.names.isEmpty()) return;
        select(id);
        const auto epoch = epoch_;
        const auto stillCurrent = [&] {
            if (shown_ && epoch == epoch_) return true;
            status_.setText(tr("The scan results changed. Select the entry again."));
            return false;
        };
        const auto fullPath = target.path();
        QMenu menu(this);
        auto *open = menu.addAction(tr("Open folder in treemap"));
        open->setEnabled(target.kind == diver::Directory);
        auto *show = menu.addAction(tr("Show in file manager"));
        auto *copy = menu.addAction(tr("Copy full path"));
        copy->setEnabled(target.exactName);
        menu.addSeparator();
        auto *rename = menu.addAction(tr("Rename…"));
#ifdef Q_OS_WIN
        const QString trashLabel = tr("Move to Recycle Bin");
        const QString trashPrompt = tr("Move this entry and its contents to the Recycle Bin?");
#else
        const QString trashLabel = tr("Move to Trash");
        const QString trashPrompt = tr("Move this entry and its contents to the Trash?");
#endif
        auto *trash = menu.addAction(trashLabel + QStringLiteral("…"));
        const bool writable = canChange(id) && target.mutableEntry();
        std::optional<diver::EntryPin> pin;
        if (writable) pin.emplace(target);
        rename->setEnabled(writable);
        trash->setEnabled(writable);
        const auto action = menu.exec(position);
        if (!action || !stillCurrent()) return;
        if (action == open) { navigate(id); return; }
        if (action == show) { reveal(id); return; }
        if (action == copy) { QApplication::clipboard()->setText(fullPath); return; }
        if (!canChange(id) || !writable) return;
        QString error = pin->error;
        if (error.isEmpty() && action == rename) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(tr("Rename"));
            dialog.setLabelText(tr("Enter a new name for\n%1").arg(fullPath));
            for (auto *label : dialog.findChildren<QLabel *>()) label->setTextFormat(Qt::PlainText);
            dialog.setTextValue(target.names.last());
            dialog.setOkButtonText(tr("Rename"));
            if (dialog.exec() != QDialog::Accepted || !stillCurrent() || !canChange(id)) return;
            if (dialog.textValue() == target.names.last()) return;
            error = diver::renameEntry(target, dialog.textValue());
        } else if (error.isEmpty() && action == trash) {
            QMessageBox confirm(QMessageBox::Question, trashLabel, fullPath, QMessageBox::Cancel, this);
            confirm.setTextFormat(Qt::PlainText);
            confirm.setInformativeText(trashPrompt);
            auto *accept = confirm.addButton(trashLabel, QMessageBox::DestructiveRole);
            confirm.setDefaultButton(QMessageBox::Cancel);
            confirm.exec();
            if (confirm.clickedButton() != accept || !stillCurrent() || !canChange(id)) return;
            error = diver::trashEntry(target);
        }
        if (!error.isEmpty()) {
            QMessageBox message(QMessageBox::Warning, tr("File operation failed"), fullPath + "\n\n" + error,
                                QMessageBox::Ok, this);
            message.setTextFormat(Qt::PlainText);
            message.exec();
            return;
        }
        setRoot(target.root);
        start(false);
    }

    void resetView() {
        ++epoch_;
        phase_.clear();
        showingPreview_ = false;
        folder_ = selected_ = none;
        history_.clear();
        shown_ = incoming_ = nullptr;
        refresh();
        stored_.reset();
        next_.reset();
    }
    void start(bool clear) {
        if (running_ || (!clear && roots_.currentText().isEmpty())) return;
        scanClock_.start();
        if (timing_) *timing_ = {};
        auto command = pythonCommand();
        if (command.isEmpty() || !QFileInfo::exists(workerPath())) {
            status_.setText(tr("Could not find Python 3 or worker.py."));
            if (timing_) timing_->failure = status_.text();
            return;
        }
        const auto executable = command.takeFirst();
        command << workerPath() << (clear ? "clear" : "scan");
        if (!clear) {
            setRoot(QDir::fromNativeSeparators(roots_.currentText()));
            restoreFocus_.clear();
            if (shown_ && rootPath_ == roots_.currentData().toString()) {
                for (Id current = folder_; current != none && current != 0; current = (*shown_)[current].parent)
                    restoreFocus_.push_back(shown_->relativeName(current));
            }
            resetView();
            rootPath_ = roots_.currentData().toString();
            command << rootPath_;
        }
        if (full_.isChecked()) command << "--full";
        if (!clear && !cache_.isChecked()) command << "--no-cache";
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("DISK_DIVER_BIN_DIR", QCoreApplication::applicationDirPath());
        env.insert("PYTHONIOENCODING", "utf-8");
        worker_.setProcessEnvironment(env);
        clearing_ = clear;
        exited_ = cancelling_ = protocolError_ = completedLive_ = false;
        message_.clear();
        journalChecked_ = false;
        status_.setToolTip({});
        lastMessage_.clear();
        cacheMessage_.clear();
        buffer_.clear();
        offset_ = 0;
        atHeader_ = true;
        running_ = true;
        status_.setText(clear ? tr("Clearing the cache…") : tr("Starting scan…"));
        updateActions();
        worker_.start(executable, command);
    }
    void cancel() {
        if (!running_ || cancelling_) return;
        cancelling_ = true;
        status_.setText(tr("Canceling scan…"));
        worker_.write("cancel\n");
        auto pid = worker_.processId();
        QTimer::singleShot(2000, this, [this, pid] {
            if (cancelling_ && worker_.state() != QProcess::NotRunning && worker_.processId() == pid)
                worker_.kill();
        });
        updateActions();
    }
    void readMessage(bool final) {
        message_ += worker_.readAllStandardError();
        for (;;) {
            qsizetype end = message_.indexOf('\n');
            if (end < 0) {
                if (!final || message_.isEmpty()) break;
                end = message_.size();
            }
            const auto line = QString::fromUtf8(message_.left(end)).trimmed();
            message_.remove(0, std::min(message_.size(), end + 1));
            if (line.isEmpty() || line == "PREVIEW" || line == "LIVE") continue;
            if (!cancelling_ && !protocolError_) {
                lastMessage_ = line.startsWith("STATUS ") ? line.mid(7) : line;
                status_.setText(lastMessage_);
                if (line.startsWith("STATUS NTFS journal checked")) journalChecked_ = true;
                if (line.startsWith("STATUS Refreshing the selected tree")) journalChecked_ = false;
                if (line.startsWith("STATUS Saved scan")) {
                    cacheMessage_ = line.section(QChar(0x00b7), 1).trimmed();
                    if (showingPreview_) phase_.setText(tr("Saved scan · %1").arg(cacheMessage_));
                }
            }
        }
    }
    void scheduleDrain() {
        if (drainScheduled_) return;
        drainScheduled_ = true;
        QTimer::singleShot(0, this, [this] { drainScheduled_ = false; drain(); });
    }
    void badStream() {
        protocolError_ = true;
        status_.setText(tr("Invalid scan data. The scan is incomplete."));
        worker_.kill();
    }
    bool parseOne() {
        const qsizetype left = buffer_.size() - offset_;
        const char *p = buffer_.constData() + offset_;
        if (atHeader_) {
            const auto frame = diver::frameHeader({p, left}, stored_ && showingPreview_, completedLive_);
            if (frame == diver::Frame::More) return false;
            if (frame == diver::Frame::Invalid) { badStream(); return false; }
            if (frame == diver::Frame::VerifiedPreview) {
                offset_ += 6;
                showingPreview_ = false;
                completedLive_ = true;
                if (timing_) timing_->verifiedPreview = true;
                phase_.clear();
                return true;
            }
            preview_ = frame == diver::Frame::Preview;
            offset_ += 5;
            atHeader_ = false;
            next_ = std::make_unique<Index>();
            incoming_ = next_.get();
            if (!stored_) {
                shown_ = incoming_;
                folder_ = none;
                showingPreview_ = preview_;
                phase_.setText(preview_ ? tr("Saved scan · %1").arg(cacheMessage_) : tr("Scanning…"));
            }
            return true;
        }
        if (left < 1) return false;
        const quint8 type = quint8(p[0]);
        if (type == 5) {
            if (!incoming_->size()) { badStream(); return false; }
            ++offset_;
            atHeader_ = true;
            incoming_->finish();
            if (shown_ && shown_ != incoming_) {
                // A refreshed directory may have new ordinal IDs after a rename
                // or deletion. Keep the navigation history for surviving paths.
                auto remap = [this](Id old) -> Id {
                    return old == none ? none : incoming_->findRelative(shown_->relativeName(old));
                };
                folder_ = remap(folder_);
                selected_ = remap(selected_);
                for (Id &entry : history_) entry = remap(entry);
                history_.erase(std::remove_if(history_.begin(), history_.end(), [this](Id id) {
                    return id == none || (*incoming_)[id].kind != diver::Directory;
                }), history_.end());
            }
            auto previous = std::move(stored_);
            stored_ = std::move(next_);
            shown_ = stored_.get();
            ++epoch_;
            showingPreview_ = preview_;
            phase_.setText(preview_ ? tr("Saved scan · %1").arg(cacheMessage_) : QString{});
            incoming_ = nullptr;
            if (folder_ == none || (*shown_)[folder_].kind != diver::Directory) folder_ = 0;
            if (preview_) status_.setText(tr("Checking for changes…"));
            else completedLive_ = true;
            refresh();
            return true;
        }
        if (type < 1 || type > 4) { badStream(); return false; }
        constexpr qsizetype header = 1 + 5 * 8 + 4;
        if (left < header) return false;
        auto length = qFromLittleEndian<quint32>(p + 41);
        if (length > 1024 * 1024) { badStream(); return false; }
        if (left < header + length) return false;
        auto field = [&](int i) { return qFromLittleEndian<quint64>(p + 1 + 8 * i); };
        if (!incoming_->append(diver::Kind(type), field(0), field(1), field(2),
                               {field(3), field(4)}, p + header, length)) {
            badStream(); return false;
        }
        offset_ += header + length;
        if (incoming_ == shown_) dirty_ = true;
        return true;
    }
    void drain() {
        if (!running_) return;
        QElapsedTimer budget;
        budget.start();
        while (!clearing_ && !protocolError_ && !cancelling_) {
            if (!parseOne()) {
                if (protocolError_) break;
                if (offset_) { buffer_.remove(0, offset_); offset_ = 0; }
                const auto chunk = worker_.read(256 * 1024);
                if (chunk.isEmpty()) break;
                buffer_ += chunk;
            }
            if (budget.elapsed() >= 10) { scheduleDrain(); break; }
        }
        if (incoming_) incoming_->flush();
        if (cancelling_ || clearing_ || protocolError_) { worker_.readAllStandardOutput(); buffer_.clear(); offset_ = 0; }
        if (!exited_ || drainScheduled_) return;
        running_ = false;
        if (!showingPreview_) phase_.clear();
        const bool incomplete = !clearing_ && (!atHeader_ || offset_ != buffer_.size() || !completedLive_);
        if (cancelling_) status_.setText(tr("Scan canceled. The results shown may be incomplete."));
        else if (protocolError_) status_.setText(tr("Invalid scan data. The scan is incomplete."));
        else if (exitCode_ != 0 || incomplete)
            status_.setText(tr("Scan incomplete. ") + lastMessage_);
        else if (clearing_) status_.setText(tr("Scan cache cleared."));
        else {
            const auto &root = (*shown_)[0];
            QString text = root.errors || root.missingSizes ? tr("Scan finished with partial results.") : tr("Scan complete.");
            if (journalChecked_) {
                text += tr(" NTFS change journal checked.");
                status_.setToolTip(tr("If a file was already open for writing before the earliest available journal record, "
                                      "the scan may miss size changes. Turn off “Use scan cache” in Scan options and scan again "
                                      "to check all file metadata."));
            }
            if (root.errors) text += tr(" Could not read %1.").arg(entryCount(root.errors));
            if (root.missingSizes) text += tr(" Allocated size is unknown for %1.").arg(entryCount(root.missingSizes));
            status_.setText(text);
            for (const auto &path : restoreFocus_) {
                const Id match = shown_->findRelative(path);
                if (match != none && (*shown_)[match].kind == diver::Directory) { folder_ = match; break; }
            }
            restoreFocus_.clear();
        }
        if (timing_ && (cancelling_ || protocolError_ || exitCode_ || incomplete))
            timing_->failure = status_.text();
        refresh();
    }

    QPushButton scan_{tr("Scan")}, stop_{tr("Cancel")};
    QToolButton browse_, back_, up_, reveal_, options_;
    QAction full_{tr("Use elevated privileges")}, cache_{tr("Use scan cache")}, clear_{tr("Clear cache")};
    QComboBox roots_, metric_;
    QLabel sourceLabel_{tr("Scan &location")}, folderSize_, phase_, status_, count_;
    QLineEdit path_;
    Table model_;
    SizeTable table_;
    Treemap map_;
    QProcess worker_;
    QTimer repaintTimer_;
    std::unique_ptr<Index> stored_, next_;
    Index *shown_ = nullptr, *incoming_ = nullptr;
    Id folder_ = none, selected_ = none;
    std::vector<Id> history_;
    std::vector<QByteArray> restoreFocus_;
    QString rootPath_, lastMessage_, cacheMessage_;
    QByteArray buffer_, message_;
    qsizetype offset_ = 0;
    int exitCode_ = 0;
    bool running_ = false, clearing_ = false, cancelling_ = false, exited_ = false;
    bool preview_ = false, showingPreview_ = false, completedLive_ = false, atHeader_ = true, journalChecked_ = false;
    bool dirty_ = false, drainScheduled_ = false, protocolError_ = false;
    quint64 epoch_ = 0, renderedEpoch_ = 0;
    Id renderedFolder_ = none;
    bool sorting_ = false, resortRequested_ = false, renderedAllocated_ = true;
    QThreadPool sortPool_;
    ViewTiming *timing_ = nullptr;
    QElapsedTimer scanClock_;
};

}

std::unique_ptr<QMainWindow> createWindow(const QString &initialPath, bool useCache, ViewTiming *timing) {
    auto window = std::make_unique<Window>(useCache, timing);
    if (!initialPath.isEmpty()) window->scanPath(initialPath);
    return window;
}
