#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QImage>
#include <QUuid>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QWebSocket>
#include "TelescopeDataProcessor.hpp"

/**
 * @brief Backend adapter to connect Alpaca server to Celestron Origin telescope
 * 
 * This class implements the OpenStellinaBackend interface but communicates
 * with a Celestron Origin telescope using its WebSocket JSON protocol.
 */
class OriginBackend : public QObject
{
    Q_OBJECT

public:
    struct TelescopeStatus {
        double altPosition = 0.0;     // Altitude in degrees
        double azPosition = 0.0;      // Azimuth in degrees
        double raPosition = 0.0;      // RA in hours
        double decPosition = 0.0;     // Dec in degrees
        bool isConnected = false;
        bool isLogicallyConnected = false;
        bool isCameraLogicallyConnected = false;
        bool isSlewing = false;
        bool isTracking = false;
        bool isParked = false;
        bool isAligned = false;
        QString currentOperation = "Idle";
        double temperature = 20.0;
    };

    explicit OriginBackend(QObject *parent = nullptr);
    ~OriginBackend();

    // Connection management
    bool connectToTelescope(const QString& host, int port = 80);
    void disconnectFromTelescope();
    bool isConnected() const;
    bool isLogicallyConnected() const;
    QString getConnectedHost();

    // Set the logical connection state (fast, no network activity)
    void setConnected(bool connected) {
        if (connected && !m_status.isConnected) {
            qWarning() << "Cannot set connected - no physical connection to Origin";
            return;
        }
        m_status.isLogicallyConnected = connected;
        qDebug() << "Logical connection state:" << m_status.isLogicallyConnected;
    }
  
    // Mount operations
    bool gotoPosition(double ra, double dec);
    bool syncPosition(double ra, double dec);
    bool abortMotion();
    bool parkMount();
    bool unparkMount();
    bool initializeTelescope();
    bool moveDirection(int direction, int speed);

    // Tracking
    bool setTracking(bool enabled);
    bool isTracking() const;

    // Status access
    TelescopeStatus status() const;
    double temperature() const;
    int m_nextSequenceId;

    // Camera operations
    bool startExposure(double duration, int iso = 200);
    bool abortExposure();
    bool isCameraExposing() const { return m_cameraState == CameraState::Exposing; }
    QByteArray getLastImageData() const { return m_lastImageData; }
    QString getLastImageFormat() const { return m_lastImageFormat; }
    
    // Camera properties
    double getLastExposureDuration() const { return m_lastExposureDuration; }
    QString getLastExposureStartTime() const { return m_lastExposureStartTime; }
    int getCurrentGain() const { return m_currentGain; }
    bool setGain(int gain);
  
    bool isCameraLogicallyConnected() const { return m_status.isCameraLogicallyConnected; }
    void setCameraConnected(bool connected);
    bool isExposing() const;
    bool isImageReady() const;
    QImage getLastImage() const;
    void setLastImage(const QImage& image);
    void setImageReady(bool ready);
    QImage singleShot(int gain, int binning, int exposureTimeMicroseconds);
    void sendCommand(const QString& command, const QString& destination, 
                    const QJsonObject& params = QJsonObject());
    bool takeSnapshot(double exposure, int iso);
    bool setManualMode();
    bool setAutoMode();
    bool getCameraMode();
    bool getCaptureParameters();
    bool setCaptureParameters(double exposure, int iso);
    bool getCameraInfo();
    // Camera mode control
    bool setCameraManualMode();
    bool setCameraAutoMode();
     
    // Camera exposure/ISO control
    bool setCameraExposure(double seconds);
    bool setCameraISO(int iso);
    
    // Snapshot control
    bool takeSingleSnapshot();
    double radiansToHours(double radians);
    double radiansToDegrees(double radians);
    // Image saving for debugging
    void enableImageSaving(bool enable = true);
    void setImageSavePath(const QString& path);
    QString getImageSavePath() const { return m_imageSavePath; }

signals:
    void connected();
    void disconnected();
    void statusUpdated();
    void imageReady();
    // Camera-related signals
    void exposureStarted();
    void exposureComplete();
    void imageReady(const QString& fileLocation);
    void cameraStateChanged(int state); // 0=idle, 1=exposing, 2=reading
    void cameraModeChanged(bool isManual);
    void captureParametersChanged(double exposure, int iso);
    void cameraInfoReceived(const QString& cameraID, const QString& model);
    void snapshotRequested();  // Emitted when snapshot command sent
    void tiffImageDownloaded(const QString& filePath, const QByteArray& imageData,
                            double ra, double dec, double exposure);
    void liveImageDownloaded(const QByteArray& imageData, 
                            double ra, double dec, double exposure);
private slots:
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onImageDownloadFinished(QNetworkReply* reply);
    void onTextMessageReceived(const QString &message);
    void updateStatus();

private:
    enum class CameraState {
        Idle = 0,
        Exposing = 1,
        Reading = 2,
        Error = 3
    };
    
    CameraState m_cameraState;
    QByteArray m_lastImageData;
    QString m_lastImageFormat;
    double m_lastExposureDuration;
    QString m_lastExposureStartTime;
    int m_currentGain;
    QString m_lastImagePath;
    
    QNetworkAccessManager* m_imageDownloader;
    
    void handleNewImageReady(const QJsonObject& data);
    void downloadImage(const QString& remotePath);
  
    QWebSocket *m_webSocket;
    TelescopeDataProcessor *m_dataProcessor;
    QNetworkAccessManager *m_networkManager;
    QTimer *m_statusTimer;
    QTimer *m_pingTimer;  // ADD THIS - for WebSocket keep-alive
  
    // State variables
    QString m_connectedHost;
    int m_connectedPort;
    bool m_isExposing;
    bool m_imageReady;
    QImage m_lastImage;
    
    // Current telescope status
    TelescopeStatus m_status;
    
    // Pending operations
    QMap<int, QString> m_pendingCommands;
    QString m_currentImagingSession;
    QFile* m_logFile;
    QTextStream* m_logStream;
    
    void initializeLogging();
    void logWebSocketMessage(const QString& direction, const QString& message);
    void cleanupLogging();

    // Helper methods
    QJsonObject createCommand(const QString& command, const QString& destination, 
                             const QJsonObject& params = QJsonObject());
    void updateStatusFromProcessor();
    void requestImage(const QString& filePath);
    double hoursToRadians(double hours);
    double degreesToRadians(double degrees);
    // Camera state tracking
    bool m_cameraManualMode;
    double m_currentExposure;
    int m_currentISO;
    bool m_snapshotInProgress;
    
    // Image metadata from last NewImageReady
    double m_lastImageRa;
    double m_lastImageDec;
    double m_lastImageExposure;
    QString m_lastImageFilePath;
    int m_statusRotation;
    QString m_telescopeIP;
    QString m_imageSavePath;  // Path to save downloaded images
    bool m_saveImagesEnabled;  // Flag to enable/disable saving
    
    void saveImageToFile(const QByteArray& imageData, const QString& originalPath,
                        double ra, double dec, double exposure);
    QString createImageSavePath();
  
};
