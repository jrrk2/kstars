#include "originmount.h"
#include <QDebug>

namespace Ekos
{

OriginMount::OriginMount(QObject *parent)
    : QObject(parent)
    , m_backend(new OriginBackend(this))
{
    // Forward backend signals
    QObject::connect(m_backend, &OriginBackend::connected,
                    this, &OriginMount::connected);
    QObject::connect(m_backend, &OriginBackend::disconnected,
                    this, &OriginMount::disconnected);
    QObject::connect(m_backend, &OriginBackend::statusUpdated,
                    this, [this]() {
        auto s = m_backend->status();
        emit coordsChanged(s.raPosition, s.decPosition);
        emit statusChanged();
    });
    /*    
    QObject::connect(m_backend, &OriginBackend::imageReady,
                    this, [this]() {
        emit imageReady(m_backend->getLastImage());
    });
    */
    QObject::connect(m_backend, &OriginBackend::tiffImageDownloaded,
                    this, [this](const QString&, const QByteArray& data, double, double, double) {
        emit snapshotReady(data);
    });
}

bool OriginMount::connect(const QString &host, int port)
{
    if (m_backend->connectToTelescope(host, port))
    {
        m_backend->setConnected(true);
        return true;
    }
    return false;
}

void OriginMount::disconnect()
{
    m_backend->disconnectFromTelescope();
}

bool OriginMount::isConnected() const
{
    return m_backend->isLogicallyConnected();
}

bool OriginMount::slew(double ra, double dec)
{
    return m_backend->gotoPosition(ra, dec);
}

bool OriginMount::sync(double ra, double dec)
{
    return m_backend->syncPosition(ra, dec);
}

bool OriginMount::abort()
{
    return m_backend->abortMotion();
}

bool OriginMount::park()
{
    return m_backend->parkMount();
}

bool OriginMount::unpark()
{
    return m_backend->unparkMount();
}

bool OriginMount::track(bool enabled)
{
    return m_backend->setTracking(enabled);
}

bool OriginMount::initialize()
{
    return m_backend->initializeTelescope();
}

double OriginMount::getRA() const
{
    return m_backend->status().raPosition;
}

double OriginMount::getDec() const
{
    return m_backend->status().decPosition;
}

bool OriginMount::isSlewing() const
{
    return m_backend->status().isSlewing;
}

bool OriginMount::isTracking() const
{
    return m_backend->isTracking();
}

bool OriginMount::connectCamera()
{
    m_backend->setCameraConnected(true);
    return true;
}

bool OriginMount::takeSnapshot(double exposure, int iso)
{
    return m_backend->takeSnapshot(exposure, iso);
}

QImage OriginMount::lastImage() const
{
    return m_backend->getLastImage();
}

} // namespace Ekos


