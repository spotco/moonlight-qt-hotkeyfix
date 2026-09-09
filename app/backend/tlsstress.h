#pragma once

#include "nvcomputer.h"

#include <QElapsedTimer>
#include <QThread>

class TlsStressRunner : public QObject
{
    Q_OBJECT

public:
    explicit TlsStressRunner(QObject* parent = nullptr);

    // Starts one worker per configured host by default. The environment variables
    // documented in tlsstress.cpp control the number of workers and rounds.
    bool start(const QVector<NvComputer*>& computers);

signals:
    void finished(int exitCode);

private:
    class Worker;

    void handleWorkerFinished(Worker* worker);

    QElapsedTimer m_Clock;
    QVector<Worker*> m_Workers;
    int m_CompletedWorkers = 0;
    int m_FailedRequests = 0;
};
