#include "computermodel.h"

#include <QThreadPool>
#include <QUdpSocket>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);

    m_Computers = m_ComputerManager->getComputers();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    Q_ASSERT(index.row() < m_Computers.count());

    NvComputer* computer = m_Computers[index.row()];
    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case PairedRole:
        return computer->pairState == NvComputer::PS_PAIRED;
    case BusyRole:
        return computer->currentGameId != 0;
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case ServerSupportedRole:
        return computer->isSupportedServerVersion;
    case DetailsRole: {
        QString state, pairState;

        switch (computer->state) {
        case NvComputer::CS_ONLINE:
            state = tr("Online");
            break;
        case NvComputer::CS_OFFLINE:
            state = tr("Offline");
            break;
        default:
            state = tr("Unknown");
            break;
        }

        switch (computer->pairState) {
        case NvComputer::PS_PAIRED:
            pairState = tr("Paired");
            break;
        case NvComputer::PS_NOT_PAIRED:
            pairState = tr("Unpaired");
            break;
        default:
            pairState = tr("Unknown");
            break;
        }

        return tr("Name: %1").arg(computer->name) + '\n' +
               tr("Status: %1").arg(state) + '\n' +
               tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
               tr("UUID: %1").arg(computer->uuid) + '\n' +
               tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
               tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
               tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
               tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
               tr("MAC Address: %1").arg(computer->macAddress.isEmpty() ? tr("Unknown") : QString(computer->macAddress.toHex(':'))) + '\n' +
               tr("Pair State: %1").arg(pairState) + '\n' +
               tr("Running Game ID: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->currentGameId) : tr("Unknown")) + '\n' +
               tr("HTTPS Port: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->activeHttpsPort) : tr("Unknown"));
    }
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[PairedRole] = "paired";
    names[BusyRole] = "busy";
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[ServerSupportedRole] = "serverSupported";
    names[DetailsRole] = "details";

    return names;
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // We must currently be streaming a game to use this function
    Q_ASSERT(computer->currentGameId != 0);

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app);
        }
    }

    // We have a current running app but it's not in our app list
    Q_ASSERT(false);
    return nullptr;
}

void ComputerModel::deleteComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);

    // m_Computer[computerIndex] will be deleted by this call
    m_ComputerManager->deleteHost(m_Computers[computerIndex]);

    // Remove the now invalid item
    m_Computers.removeAt(computerIndex);

    endRemoveRows();
}

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer)
        : m_Computer(computer) {}

    void run()
    {
        m_Computer->wake();
    }

private:
    NvComputer* m_Computer;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(m_Computers[computerIndex]);
    QThreadPool::globalInstance()->start(wakeTask);
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

QString ComputerModel::generatePinString()
{
    return m_ComputerManager->generatePinString();
}

class DeferredTestConnectionTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    void run()
    {
        unsigned int portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443, ML_PORT_FLAG_ALL);
        if (portTestResult == ML_TEST_RESULT_INCONCLUSIVE) {
            emit connectionTestCompleted(-1, QString());
        }
        else {
            char blockedPorts[512];
            LiStringifyPortFlags(portTestResult, "\n", blockedPorts, sizeof(blockedPorts));
            emit connectionTestCompleted(portTestResult, QString(blockedPorts));
        }
    }

signals:
    void connectionTestCompleted(int result, QString blockedPorts);
};

void ComputerModel::testConnectionForComputer(int)
{
    DeferredTestConnectionTask* testConnectionTask = new DeferredTestConnectionTask();
    QObject::connect(testConnectionTask, &DeferredTestConnectionTask::connectionTestCompleted,
                     this, &ComputerModel::connectionTestCompleted);
    QThreadPool::globalInstance()->start(testConnectionTask);
}

void ComputerModel::pairComputer(int computerIndex, QString pin)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->pairHost(m_Computers[computerIndex], pin);
}

void ComputerModel::handlePairingCompleted(NvComputer*, QString error)
{
    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = m_ComputerManager->getComputers();

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;
        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        emit dataChanged(createIndex(index, 0), createIndex(index, 0));
    }
}


class DeferredHostUdpTestTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    DeferredHostUdpTestTask(QString host, uint16_t httpPort)
        : m_Host(std::move(host)), m_HttpPort(httpPort) {}

    void run()
    {
        // GameStream UDP offsets from HTTP base B: video B+9, control B+10, audio B+11
        struct PortSpec { const char* name; uint16_t port; };
        const PortSpec ports[] = {
            {"video", static_cast<uint16_t>(m_HttpPort + 9)},
            {"control", static_cast<uint16_t>(m_HttpPort + 10)},
            {"audio", static_cast<uint16_t>(m_HttpPort + 11)},
        };

        QString report;
        report += QStringLiteral("Test Host UDP (THIS PC host — not public qt.conntest)\n");
        report += QStringLiteral("Target: %1  HTTP base: %2\n\n").arg(m_Host).arg(m_HttpPort);

        const QByteArray probePrefix = QByteArrayLiteral("SPOTCO_UDP_PROBE_V1");
        const QByteArray ackPrefix = QByteArrayLiteral("SPOTCO_UDP_PROBE_ACK");

        for (const auto& spec : ports) {
            QUdpSocket sock;
            if (!sock.bind(QHostAddress::Any, 0)) {
                report += QStringLiteral("%1 UDP %2: BIND_FAIL\n").arg(spec.name).arg(spec.port);
                continue;
            }

            quint64 nonceVal = QRandomGenerator::global()->generate64();
            QByteArray nonce(reinterpret_cast<const char*>(&nonceVal), 8);
            QByteArray payload = probePrefix + nonce;

            QElapsedTimer timer;
            timer.start();
            bool echoOk = false;
            qint64 rttMs = -1;

            for (int i = 0; i < 3; ++i) {
                sock.writeDatagram(payload, QHostAddress(m_Host), spec.port);
            }

            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

            auto tryRead = [&]() {
                while (sock.hasPendingDatagrams()) {
                    QByteArray datagram;
                    datagram.resize(int(sock.pendingDatagramSize()));
                    QHostAddress sender;
                    quint16 senderPort = 0;
                    sock.readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
                    if (datagram.startsWith(ackPrefix)) {
                        // Prefer matching nonce when present
                        if (datagram.size() >= ackPrefix.size() + 8) {
                            if (datagram.mid(ackPrefix.size(), 8) != nonce) {
                                continue;
                            }
                        }
                        echoOk = true;
                        rttMs = timer.elapsed();
                        loop.quit();
                        return;
                    }
                }
            };

            QObject::connect(&sock, &QUdpSocket::readyRead, &loop, tryRead);
            timeout.start(1500);
            tryRead();
            if (!echoOk) {
                loop.exec();
            }

            if (echoOk) {
                report += QStringLiteral("%1 UDP %2: SENT, ECHO_OK, rtt_ms=%3\n")
                              .arg(spec.name).arg(spec.port).arg(rttMs);
            } else {
                report += QStringLiteral("%1 UDP %2: SENT, ECHO_TIMEOUT\n")
                              .arg(spec.name).arg(spec.port);
            }
        }

        report += QStringLiteral("\nOK = ECHO_OK on video+audio (+control if idle probe). "
                                 "ECHO_TIMEOUT on video while audio OK suggests video UDP path blocked/asymmetric.\n");
        emit hostUdpTestCompleted(report);
    }

signals:
    void hostUdpTestCompleted(QString report);

private:
    QString m_Host;
    uint16_t m_HttpPort;
};

void ComputerModel::testHostUdpForComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QString host;
    uint16_t httpPort = DEFAULT_HTTP_PORT;
    {
        QReadLocker lock(&computer->lock);
        if (!computer->activeAddress.isNull()) {
            host = computer->activeAddress.address();
            httpPort = computer->activeAddress.port() ? computer->activeAddress.port() : DEFAULT_HTTP_PORT;
        } else if (!computer->localAddress.isNull()) {
            host = computer->localAddress.address();
            httpPort = computer->localAddress.port() ? computer->localAddress.port() : DEFAULT_HTTP_PORT;
        } else if (!computer->manualAddress.isNull()) {
            host = computer->manualAddress.address();
            httpPort = computer->manualAddress.port() ? computer->manualAddress.port() : DEFAULT_HTTP_PORT;
        } else if (!computer->remoteAddress.isNull()) {
            host = computer->remoteAddress.address();
            httpPort = computer->remoteAddress.port() ? computer->remoteAddress.port() : DEFAULT_HTTP_PORT;
        }
    }

    if (host.isEmpty()) {
        emit hostUdpTestCompleted(QStringLiteral("Test Host UDP failed: no address for this PC."));
        return;
    }

    DeferredHostUdpTestTask* task = new DeferredHostUdpTestTask(host, httpPort);
    QObject::connect(task, &DeferredHostUdpTestTask::hostUdpTestCompleted,
                     this, &ComputerModel::hostUdpTestCompleted);
    QThreadPool::globalInstance()->start(task);
}


#include "computermodel.moc"
