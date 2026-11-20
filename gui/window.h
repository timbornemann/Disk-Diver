#pragma once

#include <QMainWindow>
#include <memory>

struct ViewTiming {
    qint64 firstMs = -1, finalMs = -1;
    quint64 entries = 0;
    quint32 errors = 0, unknownSizes = 0;
    QString firstSource, failure;
    bool verifiedPreview = false;
};

std::unique_ptr<QMainWindow> createWindow(const QString &initialPath = {}, bool useCache = true,
                                          ViewTiming *timing = nullptr);
