#include "tlsstress.h"

#include "nvhttp.h"

#include <QReadLocker>
#include <QThread>

#include <utility>

namespace {

struct StressTarget
{
    QString name;
    NvAddress address;
    uint16_t httpsPort;
    QSslCertificate serverCert;
};

int envInt(const char* name, int defaultValue, int minimum)
{
    bool ok = false;
    int value = QString::fromLocal8Bit(qgetenv(name)).toInt(&ok);
    return ok ? qMax(value, minimum) : defaultValue;
}

StressTarget targetFromComputer(NvComputer* computer)
{
    const QVector<NvAddress> addresses = computer->uniqueAddresses();

    QReadLocker lock(&computer->lock);
    NvAddress address = computer->activeAddress;
    if (address.isNull()) {
        for (const NvAddress& candidate : addresses) {
            if (!candidate.isNull()) {
                address = candidate;
                break;
            }
        }
    }
    int configuredPort = envInt("MOONLIGHT_TLS_STRESS_HTTPS_PORT", 0, 1);
    uint16_t httpsPort = configuredPort != 0 ? static_cast<uint16_t>(configuredPort) : computer->activeHttpsPort;
    if (httpsPort == 0) {
        httpsPort = 47984;
    }

    return {computer->name, address, httpsPort, computer->serverCert};
}

} // namespace

class TlsStressRunner::Worker : public QThread
{
public:
    Worker(StressTarget target, int workerNumber, int rounds, QElapsedTimer* clock)
        : m_Target(std::move(target)),
          m_WorkerNumber(workerNumber),
          m_Rounds(rounds),
          m_Clock(clock)
    {
        setObjectName(QString("TLS stress worker %1").arg(workerNumber));
    }

    int failedRequests() const
    {
        return m_FailedRequests;
    }

protected:
    void run() override
    {
        const int interRequestDelay = envInt("MOONLIGHT_TLS_STRESS_DELAY_MS", 0, 0);

        qInfo().nospace() << "TLSSTRESS WORKER START worker=" << m_WorkerNumber
                          << " host=" << m_Target.name
                          << " address=" << m_Target.address.toString()
                          << " httpsPort=" << m_Target.httpsPort
                          << " rounds=" << m_Rounds;

        for (int round = 0; round < m_Rounds && !isInterruptionRequested(); round++) {
            const qint64 startMs = m_Clock->elapsed();
            qInfo().nospace() << "TLSSTRESS REQUEST START worker=" << m_WorkerNumber
                              << " round=" << round
                              << " t_ms=" << startMs
                              << " host=" << m_Target.name;

            bool success = false;
            QString result;
            {
                // Keep NvHTTP local to this thread. This matches the existing
                // polling implementation and makes destruction part of the
                // request timing recorded below.
                NvHTTP http(m_Target.address, m_Target.httpsPort, m_Target.serverCert);
                try {
                    const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR, true);
                    success = !serverInfo.isEmpty();
                    result = success ? QStringLiteral("ok") : QStringLiteral("empty response");
                }
                catch (const QtNetworkReplyException& exception) {
                    result = QStringLiteral("network error=%1 text=%2")
                            .arg(exception.getError())
                            .arg(exception.toQString());
                }
                catch (const GfeHttpResponseException& exception) {
                    result = QStringLiteral("GFE error=%1 text=%2")
                            .arg(exception.getStatusCode())
                            .arg(exception.toQString());
                }
                catch (...) {
                    result = QStringLiteral("unknown exception");
                }
            }

            if (!success) {
                m_FailedRequests++;
            }

            qInfo().nospace() << "TLSSTRESS REQUEST END worker=" << m_WorkerNumber
                              << " round=" << round
                              << " t_ms=" << m_Clock->elapsed()
                              << " duration_ms=" << (m_Clock->elapsed() - startMs)
                              << " result=" << result;

            if (interRequestDelay != 0) {
                QThread::msleep(static_cast<unsigned long>(interRequestDelay));
            }
        }

        qInfo().nospace() << "TLSSTRESS WORKER END worker=" << m_WorkerNumber
                          << " t_ms=" << m_Clock->elapsed()
                          << " failed=" << m_FailedRequests;
    }

private:
    StressTarget m_Target;
    int m_WorkerNumber;
    int m_Rounds;
    QElapsedTimer* m_Clock;
    int m_FailedRequests = 0;
};

TlsStressRunner::TlsStressRunner(QObject* parent)
    : QObject(parent)
{
    m_Clock.start();
}

bool TlsStressRunner::start(const QVector<NvComputer*>& computers)
{
    const QString targetFilter = QString::fromLocal8Bit(qgetenv("MOONLIGHT_TLS_STRESS_TARGET"));
    QVector<StressTarget> targets;
    for (NvComputer* computer : computers) {
        StressTarget target = targetFromComputer(computer);
        if (!targetFilter.isEmpty() &&
                target.name.compare(targetFilter, Qt::CaseInsensitive) != 0 &&
                target.address.toString().compare(targetFilter, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (target.address.isNull()) {
            qWarning() << "TLSSTRESS skipping host with no known address:" << target.name;
            continue;
        }
        if (target.serverCert.isNull()) {
            qWarning() << "TLSSTRESS skipping host without a pinned server certificate:" << target.name;
            continue;
        }
        targets.append(std::move(target));
    }

    if (targets.isEmpty()) {
        qCritical() << "TLSSTRESS found no configured paired hosts with an address and certificate";
        return false;
    }

    const int defaultWorkerCount = targets.size();
    const int workerCount = envInt("MOONLIGHT_TLS_STRESS_WORKERS", defaultWorkerCount, 1);
    const int rounds = envInt("MOONLIGHT_TLS_STRESS_ROUNDS", 20, 1);

    qInfo().nospace() << "TLSSTRESS CONFIG hosts=" << targets.size()
                      << " workers=" << workerCount
                      << " rounds=" << rounds
                      << " targetFilter=" << (targetFilter.isEmpty() ? QStringLiteral("<all>") : targetFilter)
                      << " clearAccessCacheWhilePending="
                      << qEnvironmentVariableIsSet("MOONLIGHT_TLS_STRESS_CLEAR_ACCESS_CACHE");

    for (int i = 0; i < workerCount; i++) {
        Worker* worker = new Worker(targets.at(i % targets.size()), i, rounds, &m_Clock);
        worker->setParent(this);
        m_Workers.append(worker);
        connect(worker, &QThread::finished, this, [this, worker]() {
            handleWorkerFinished(worker);
        });
    }

    for (Worker* worker : m_Workers) {
        worker->start();
    }

    return true;
}

void TlsStressRunner::handleWorkerFinished(Worker* worker)
{
    m_CompletedWorkers++;
    m_FailedRequests += worker->failedRequests();

    if (m_CompletedWorkers == m_Workers.size()) {
        qInfo().nospace() << "TLSSTRESS COMPLETE t_ms=" << m_Clock.elapsed()
                          << " workers=" << m_Workers.size()
                          << " failedRequests=" << m_FailedRequests;
        emit finished(m_FailedRequests == 0 ? 0 : 1);
    }
}
