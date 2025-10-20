#ifndef ORIGINMOUNT_H
#define ORIGINMOUNT_H

#include "../../telescopes/origin/OriginBackend.hpp"
#include <QObject>

namespace Ekos
{

class OriginMount : public QObject
{
    Q_OBJECT

public:
    explicit OriginMount(QObject *parent = nullptr);
    ~OriginMount() = default;
    
    // Connection
    bool connect(const QString &host, int port = 80);
    void disconnect();
    bool isConnected() const;
    
    // Mount operations
    bool slew(double ra, double dec);
    bool sync(double ra, double dec);
    bool abort();
    bool park();
    bool unpark();
    bool track(bool enabled);
    bool initialize();
    
    // Status
    double getRA() const;
    double getDec() const;
    bool isSlewing() const;
    bool isTracking() const;
    
    // Camera
    bool connectCamera();
    bool takeSnapshot(double exposure, int iso);
    QImage lastImage() const;
    
    // Direct backend access
    OriginBackend* backend() { return m_backend; }

signals:
    void connected();
    void disconnected();
    void coordsChanged(double ra, double dec);
    void statusChanged();
    void imageReady(const QImage &image);
    void snapshotReady(const QByteArray &data);

private:
    OriginBackend *m_backend;
};

} // namespace Ekos

#endif


