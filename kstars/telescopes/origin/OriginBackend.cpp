#include "OriginBackend.hpp"
#include <QDebug>
#include <QDateTime>
#include <QUrl>
#include <QBuffer>
#include <cmath>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QEventLoop>

OriginBackend::OriginBackend(QObject *parent)
    : QObject(parent)
    , m_cameraManualMode(false)
    , m_currentExposure(0.1)
    , m_currentISO(200)
    , m_snapshotInProgress(false)
    , m_lastImageRa(0)
    , m_lastImageDec(0)
    , m_lastImageExposure(0)
    , m_webSocket(nullptr)
    , m_dataProcessor(nullptr)
    , m_networkManager(nullptr)
    , m_statusTimer(nullptr)
    , m_pingTimer(nullptr)
    , m_connectedPort(80)
    , m_isExposing(false)
    , m_imageReady(false)
    , m_nextSequenceId(2000)
    , m_logFile(nullptr)  // ADD THIS
    , m_logStream(nullptr)  // ADD THIS
    , m_statusRotation(0)  // ADD THIS
    , m_cameraState(CameraState::Idle)
    , m_lastExposureDuration(0.0)
    , m_currentGain(200)
    , m_imageDownloader(new QNetworkAccessManager(this))      
    , m_saveImagesEnabled(true)  // ADD THIS - Enable by default for debugging
{
    m_webSocket = new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    m_dataProcessor = new TelescopeDataProcessor(this);
    m_networkManager = new QNetworkAccessManager(this);
    m_statusTimer = new QTimer(this);
    m_pingTimer = new QTimer(this);

    // Initialize logging - ADD THIS
    initializeLogging();

    // Connect WebSocket signals
    connect(m_webSocket, &QWebSocket::connected, this, &OriginBackend::onWebSocketConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &OriginBackend::onWebSocketDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &OriginBackend::onTextMessageReceived);

    // ADD THESE - WebSocket ping/pong handling:
    connect(m_webSocket, &QWebSocket::pong, this, [this](quint64 elapsedTime, const QByteArray &payload) {
        qDebug() << "Pong received, RTT:" << elapsedTime << "ms";
        logWebSocketMessage("PONG", QString("RTT: %1ms").arg(elapsedTime));
    });
    
    connect(m_webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError error) {
        qWarning() << "WebSocket error:" << error << m_webSocket->errorString();
        logWebSocketMessage("ERROR", QString("Code: %1, Message: %2")
                           .arg(error).arg(m_webSocket->errorString()));
    });

    // Connect data processor signals
    connect(m_dataProcessor, &TelescopeDataProcessor::mountStatusUpdated, 
            this, &OriginBackend::updateStatus);

    // Setup status update timer
    m_statusTimer->setInterval(5000); // Update every 5 seconds
    connect(m_statusTimer, &QTimer::timeout, this, &OriginBackend::updateStatus);

    // ADD THIS - Setup ping timer (keep-alive)
    m_pingTimer->setInterval(15000); // Ping every 10 seconds
    connect(m_pingTimer, &QTimer::timeout, this, [this]() {
        if (m_webSocket && m_webSocket->isValid() && 
            m_webSocket->state() == QAbstractSocket::ConnectedState) {
            qDebug() << "Sending WebSocket ping...";
            m_webSocket->ping();
            logWebSocketMessage("PING", "Keep-alive ping sent");
        }
    });
    // Connect image download signals
    connect(m_imageDownloader, &QNetworkAccessManager::finished,
            this, &OriginBackend::onImageDownloadFinished);

    // Initialize image save path
    m_imageSavePath = createImageSavePath();
    qDebug() << "Image save path:" << m_imageSavePath;

}

OriginBackend::~OriginBackend()
{
    disconnectFromTelescope();
    cleanupLogging();  // ADD THIS LINE
}

bool OriginBackend::connectToTelescope(const QString& host, int port)
{
    if (isConnected()) {
        qDebug() << "Already connected to telescope";
        return true;
    }

    m_connectedHost = host;
    m_connectedPort = port;

    // Construct WebSocket URL for Origin telescope
    QString url = QString("ws://%1:%2/SmartScope-1.0/mountControlEndpoint").arg(host).arg(port);
    
    qDebug() << "Connecting to Origin telescope at:" << url;
    
    m_webSocket->open(QUrl(url));
    
    // Wait for connection with timeout
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(10000); // 10 second timeout
    
    connect(m_webSocket, &QWebSocket::connected, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeoutTimer.start();
    loop.exec();
    
    return isConnected();
}

void OriginBackend::disconnectFromTelescope()
{
    if (m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState) {
        m_webSocket->close();
    }
    
    if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
    }
    
    // ADD THIS
    if (m_pingTimer && m_pingTimer->isActive()) {
        m_pingTimer->stop();
    }

    m_status.isConnected = false;
    m_status.isLogicallyConnected = false;
    m_status.isCameraLogicallyConnected = false;
    qDebug() << "disconnected";
    
    emit disconnected();
}

bool OriginBackend::isConnected() const
{
    return m_webSocket->isValid() && m_webSocket->state() == QAbstractSocket::ConnectedState;
    //    return m_status.isConnected;
}

bool OriginBackend::isLogicallyConnected() const
{
  return isConnected() && m_status.isLogicallyConnected;
}

bool OriginBackend::gotoPosition(double ra, double dec)
{
    if (!isConnected()) {
        qWarning() << "Cannot goto position - not connected";
        return false;
    }

    // Convert RA/Dec to radians for Origin telescope
    double raRadians = hoursToRadians(ra);
    double decRadians = degreesToRadians(dec);

    QJsonObject params;
    params["Ra"] = raRadians;
    params["Dec"] = decRadians;

    sendCommand("GotoRaDec", "Mount", params);
    
    m_status.isSlewing = true;
    m_status.currentOperation = "Slewing";
    
    return true;
}

bool OriginBackend::syncPosition(double ra, double dec)
{
    if (!isConnected()) {
        qWarning() << "Cannot sync position - not connected";
        return false;
    }

    // Convert RA/Dec to radians for Origin telescope
    double raRadians = hoursToRadians(ra);
    double decRadians = degreesToRadians(dec);

    QJsonObject params;
    params["Ra"] = raRadians;
    params["Dec"] = decRadians;

    sendCommand("SyncToRaDec", "Mount", params);
    
    return true;
}

// Snapshot Control
bool OriginBackend::takeSingleSnapshot()
{
    return takeSnapshot(m_currentExposure, m_currentISO);
}

bool OriginBackend::takeSnapshot(double exposure, int iso)
{
    qDebug() << "Taking snapshot: Exposure =" << exposure << "ISO =" << iso;
    if (!isConnected()) {
      qDebug() << "ignored due to closed socket";
        return false;
    }

    m_snapshotInProgress = true;
    // RunSampleCapture command triggers a single snapshot
    QJsonObject params;
    params["ExposureTime"] = exposure;
    params["ISO"] = iso;
    
    sendCommand("RunSampleCapture", "TaskController", params);
    return true;
}

// ============================================================================
// MODE CONTROL
// ============================================================================

bool OriginBackend::setCameraManualMode()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;    
    sendCommand("SetEnableManual", "LiveStream", cmd);
    qDebug() << "Setting manual mode";
    return true;
}

bool OriginBackend::setCameraAutoMode()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;
    sendCommand("SetEnableAuto", "LiveStream", cmd);
    qDebug() << "Setting auto mode";
    return true;
}

bool OriginBackend::getCameraMode()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;
    sendCommand("GetEnableManual", "LiveStream", cmd);
    return true;
}

bool OriginBackend::getCaptureParameters()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;    
    sendCommand("GetCaptureParameters", "Camera", cmd);
    return true;
}

bool OriginBackend::setCaptureParameters(double exposure, int iso)
{
    if (!isConnected()) {
        return false;
    }
    
    QJsonObject cmd;
    cmd["Exposure"] = exposure;
    cmd["ISO"] = iso;
    sendCommand("SetCaptureParameters", "Camera", cmd);
    return true;
}

// ============================================================================
// CAMERA INFO
// ============================================================================

bool OriginBackend::getCameraInfo()
{
    if (!isConnected()) {
        return false;
    }

    QJsonObject cmd;
    sendCommand("GetCameraInfo", "Camera", cmd);
    return true;
}

bool OriginBackend::abortMotion()
{
    if (!isConnected()) {
        return false;
    }

    sendCommand("AbortAxisMovement", "Mount");
    
    m_status.isSlewing = false;
    m_status.currentOperation = "Idle";
    
    return true;
}

bool OriginBackend::parkMount()
{
    if (!isConnected()) {
        return false;
    }

    sendCommand("Park", "Mount");
    
    m_status.isParked = true;
    m_status.currentOperation = "Parking";
    
    return true;
}

bool OriginBackend::unparkMount()
{
    if (!isConnected()) {
        return false;
    }

    sendCommand("Unpark", "Mount");
    
    m_status.isParked = false;
    m_status.currentOperation = "Unparking";
    
    return true;
}

bool OriginBackend::initializeTelescope()
{
    if (!isConnected()) {
        return false;
    }

    // Use current date/time and location for initialization
    QDateTime now = QDateTime::currentDateTime();
    
    QJsonObject params;
    params["Date"] = now.toString("dd MM yyyy");
    params["Time"] = now.toString("HH:mm:ss");
    params["TimeZone"] = "UTC"; // Or get system timezone
    params["Latitude"] = degreesToRadians(52.2); // Default latitude
    params["Longitude"] = degreesToRadians(0.0); // Default longitude
    params["FakeInitialize"] = false;

    sendCommand("RunInitialize", "TaskController", params);
    
    m_status.currentOperation = "Initializing";
    
    return true;
}

bool OriginBackend::moveDirection(int direction, int speed)
{
    if (!isConnected()) {
        return false;
    }

    // Map direction codes to Origin telescope directions
    // 0 = North (Dec+), 1 = South (Dec-), 2 = East (RA+), 3 = West (RA-)
    
    QString axisName;
    QString directionName;
    
    switch(direction) {
        case 0: // North
            axisName = "Dec";
            directionName = "Positive";
            break;
        case 1: // South
            axisName = "Dec";
            directionName = "Negative";
            break;
        case 2: // East
            axisName = "Ra";
            directionName = "Positive";
            break;
        case 3: // West
            axisName = "Ra";
            directionName = "Negative";
            break;
        default:
            return false;
    }

    QJsonObject params;
    params["Axis"] = axisName;
    params["Direction"] = directionName;
    params["Speed"] = speed; // 0-100

    sendCommand("MoveAxis", "Mount", params);
    
    return true;
}

bool OriginBackend::setTracking(bool enabled)
{
    if (!isConnected()) {
        return false;
    }

    QString command = enabled ? "StartTracking" : "StopTracking";
    sendCommand(command, "Mount");
    
    m_status.isTracking = enabled;
    
    return true;
}

bool OriginBackend::isTracking() const
{
    return m_status.isTracking;
}

OriginBackend::TelescopeStatus OriginBackend::status() const
{
    return m_status;
}

double OriginBackend::temperature() const
{
    return m_status.temperature;
}

bool OriginBackend::isExposing() const
{
    return m_isExposing;
}

bool OriginBackend::isImageReady() const
{
    return m_imageReady;
}

QImage OriginBackend::getLastImage() const
{
    return m_lastImage;
}

void OriginBackend::setLastImage(const QImage& image)
{
    m_lastImage = image;
}

void OriginBackend::setImageReady(bool ready)
{
    m_imageReady = ready;
}

// Exposure and ISO Control
bool OriginBackend::setCameraExposure(double seconds)
{
    return setCaptureParameters(seconds, m_currentISO);
}

bool OriginBackend::setCameraISO(int iso)
{
    return setCaptureParameters(m_currentExposure, iso);
}

QImage OriginBackend::singleShot(int gain, int binning, int exposureTimeMicroseconds)
{
    if (!isConnected()) {
        qWarning() << "Cannot take image - not connected";
        return QImage();
    }

    // Generate a unique session UUID
    QUuid uuid = QUuid::createUuid();
    m_currentImagingSession = uuid.toString(QUuid::WithoutBraces);

    // Set camera parameters first
    QJsonObject cameraParams;
    cameraParams["ISO"] = gain;
    cameraParams["Binning"] = binning;
    cameraParams["Exposure"] = exposureTimeMicroseconds / 1000000.0; // Convert to seconds

    sendCommand("SetCaptureParameters", "Camera", cameraParams);

    // Wait a moment for parameters to be set
    QEventLoop loop500;
    QTimer::singleShot(500, &loop500, &QEventLoop::quit);
    loop500.exec();
 
    // Start imaging
    QJsonObject imagingParams;
    imagingParams["Name"] = QString("AlpacaCapture_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    imagingParams["Uuid"] = m_currentImagingSession;
    imagingParams["SaveRawImage"] = true;

    sendCommand("RunImaging", "TaskController", imagingParams);
    
    m_isExposing = true;
    m_imageReady = false;

    // Wait for exposure to complete with timeout
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval((exposureTimeMicroseconds / 1000) + 30000); // Exposure time + 30 seconds

    //    connect(this, &OriginBackend::imageReady, &loop, &QEventLoop::quit);
    //    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    m_isExposing = false;

    if (m_imageReady) {
        return m_lastImage;
    } else {
        qWarning() << "Image capture timed out or failed";
        return QImage();
    }
}

// Also log connection events by modifying these methods:
void OriginBackend::onWebSocketConnected()
{
    qDebug() << "Connected to Origin telescope";
    
    // LOG CONNECTION EVENT - ADD THIS
    logWebSocketMessage("SYSTEM", QString("Connected to %1:%2").arg(m_connectedHost).arg(m_connectedPort));
    
    m_status.isConnected = true;
    qDebug() << "connected";
    
    // Start status updates
    m_statusTimer->start();

    // ADD THIS - Start keep-alive pings
    m_pingTimer->start();

    // Request initial status
    sendCommand("GetStatus", "Mount");
    
    emit connected();
}

void OriginBackend::onWebSocketDisconnected()
{
    qDebug() << "Disconnected from Origin telescope";
    
    // LOG DISCONNECTION EVENT - ADD THIS
    logWebSocketMessage("SYSTEM", "Disconnected from telescope");
    
    m_status.isConnected = false;
    m_status.isLogicallyConnected = false;
    qDebug() << "disconnected";
    
    if (m_statusTimer->isActive()) {
        m_statusTimer->stop();
    }
    if (m_pingTimer->isActive()) {
        m_pingTimer->stop();
    }
    
    emit disconnected();
}

void OriginBackend::onTextMessageReceived(const QString &message)
{
    logWebSocketMessage("RECV", message);
    
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Still emit for other listeners
//    emit messageReceived(obj);
    
    // Process through data processor
    bool processed = m_dataProcessor->processJsonPacket(message.toUtf8());
    if (processed) {
        updateStatusFromProcessor();
    }
    
    QString command = obj["Command"].toString();
    QString type = obj["Type"].toString();
    QString source = obj["Source"].toString();

    // Handle NewImageReady notification from Origin
    if (type == "Notification" && command == "NewImageReady") {
        handleNewImageReady(obj);
    }
    
    // Handle responses to our commands
    if (type == "Response") {
        if (command == "RunSampleCapture") {
            // Exposure started successfully
            qDebug() << "Exposure command acknowledged";
        }
    }
    
    // Handle camera-specific responses
    if (type == "Response") {
        int errorCode = obj["ErrorCode"].toInt();
        
        if (errorCode != 0) {
            QString errorMsg = obj["ErrorMessage"].toString();
            qWarning() << "Command error:" << errorCode << errorMsg;
            return;
        }
        
        // Process camera responses
        if (command == "GetCaptureParameters") {
            m_currentExposure = obj["Exposure"].toDouble();
            m_currentISO = obj["ISO"].toInt();
            
	    // qDebug() << "Capture parameters: Exposure =" << m_currentExposure << "ISO =" << m_currentISO;
            
            emit captureParametersChanged(m_currentExposure, m_currentISO);
        }
        else if (command == "GetEnableManual" || 
                 command == "SetEnableManual" || 
                 command == "SetEnableAuto") {
            if (obj.contains("IsManual")) {
                m_cameraManualMode = obj["IsManual"].toBool();
                qDebug() << "Camera mode:" << (m_cameraManualMode ? "Manual" : "Auto");
                emit cameraModeChanged(m_cameraManualMode);
            }
        }
        else if (command == "GetCameraInfo") {
            QString cameraID = obj["CameraID"].toString();
            QString model = obj["CameraModel"].toString();
            qDebug() << "Camera info: ID =" << cameraID << "Model =" << model;
            emit cameraInfoReceived(cameraID, model);
        }
    }

    // Handle image notifications
    if (source == "ImageServer" && 
        command == "NewImageReady" &&
        type == "Notification") {
        
        QString filePath = obj["FileLocation"].toString();
        
        if (!filePath.isEmpty()) {
            // Store metadata
            m_lastImageRa = obj["Ra"].toDouble();
            m_lastImageDec = obj["Dec"].toDouble();
            m_lastImageExposure = obj["ExposureTime"].toDouble();
            m_lastImageFilePath = filePath;
            
            // Determine type by extension
            bool isTiff = filePath.endsWith(".tiff", Qt::CaseInsensitive) || 
                         filePath.endsWith(".tif", Qt::CaseInsensitive);
            
	    // qDebug() << "Image ready:" << filePath << (isTiff ? "(TIFF snapshot)" : "(JPEG live stream)");

	    // ADD THIS CHECK - Skip live JPEGs during snapshot capture
	    if (!isTiff && m_snapshotInProgress) {
	      qDebug() << "Skipping live JPEG - snapshot in progress";
	      return;  // Don't download live images during snapshot
	    }

            // Download the image
            requestImage(filePath);
        }
    }
}

void OriginBackend::updateStatus()
{
    if (!isConnected()) {
        return;
    }
    switch (m_statusRotation % 3) {
        case 0:
            sendCommand("GetStatus", "Mount");
            break;
        case 1:
            sendCommand("GetStatus", "Environment");
            break;
        case 2:
            sendCommand("GetCaptureParameters", "Camera");
            break;
    }
    
    m_statusRotation++;
}

QJsonObject OriginBackend::createCommand(const QString& command, const QString& destination, const QJsonObject& params)
{
    QJsonObject jsonCommand;
    jsonCommand["Command"] = command;
    jsonCommand["Destination"] = destination;
    jsonCommand["SequenceID"] = m_nextSequenceId++;
    jsonCommand["Source"] = "AlpacaServer";
    jsonCommand["Type"] = "Command";
    
    // Add any parameters
    for (auto it = params.begin(); it != params.end(); ++it) {
        jsonCommand[it.key()] = it.value();
    }
    
    return jsonCommand;
}

// Modify the sendCommand method in OriginBackend.cpp:
void OriginBackend::sendCommand(const QString& command, const QString& destination, const QJsonObject& params)
{
    QJsonObject jsonCommand = createCommand(command, destination, params);
    
    QJsonDocument doc(jsonCommand);
    QString message = doc.toJson(QJsonDocument::Compact);  // Use Compact for cleaner logs
    
    if (isConnected()) {
        // LOG THE OUTGOING MESSAGE - ADD THIS
        logWebSocketMessage("SEND", message);
        
        m_webSocket->sendTextMessage(message);
        
        // Store the command for potential response tracking
        int sequenceId = jsonCommand["SequenceID"].toInt();
        m_pendingCommands[sequenceId] = command;
        
        // qDebug() << "Sent command:" << command << "to" << destination;
    } else {
        qWarning() << "Cannot send command - WebSocket not connected";
    }
}

void OriginBackend::updateStatusFromProcessor()
{
    const TelescopeData& data = m_dataProcessor->getData();
    
    // Update mount status
    m_status.isTracking = data.mount.isTracking;
    m_status.isSlewing = !data.mount.isGotoOver;
    m_status.isAligned = data.mount.isAligned;
    
    // Convert coordinates from radians
    m_status.raPosition = radiansToHours(data.mount.enc0); // Assuming enc0 is RA
    m_status.decPosition = radiansToDegrees(data.mount.enc1); // Assuming enc1 is Dec
    
    // Calculate Alt/Az from current mount data if available
    // This would require proper coordinate conversion
    m_status.altPosition = 45.0; // Placeholder
    m_status.azPosition = 180.0; // Placeholder
    
    // Update temperature from environment data
    m_status.temperature = data.environment.ambientTemperature;
    
    // Update operation status
    if (m_status.isSlewing) {
        m_status.currentOperation = "Slewing";
    } else if (m_status.isTracking) {
        m_status.currentOperation = "Tracking";
    } else {
        m_status.currentOperation = "Idle";
    }
    
    emit statusUpdated();
}

void OriginBackend::requestImage(const QString &filePath) {
    if (m_connectedHost.isEmpty()) return;
    
    QString fullPath = QString("http://%1/SmartScope-1.0/dev2/%2")
                       .arg(m_connectedHost, filePath);
    QUrl url(fullPath);
    QNetworkRequest request(url);
    
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Accept", "*/*");
    
    bool isTiff = filePath.endsWith(".tiff", Qt::CaseInsensitive) || 
                  filePath.endsWith(".tif", Qt::CaseInsensitive);
    
    qDebug() << "Downloading:" << fullPath;
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->get(request);
    
    // Store metadata in reply
    reply->setProperty("filePath", filePath);
    reply->setProperty("isTiff", isTiff);
    reply->setProperty("ra", m_lastImageRa);
    reply->setProperty("dec", m_lastImageDec);
    reply->setProperty("exposure", m_lastImageExposure);
    
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager]() {
        if (reply->error() == QNetworkReply::NoError) {
            QByteArray imageData = reply->readAll();
            QString filePath = reply->property("filePath").toString();
            bool isTiff = reply->property("isTiff").toBool();
            double ra = reply->property("ra").toDouble();
            double dec = reply->property("dec").toDouble();
            double exposure = reply->property("exposure").toDouble();
            
            qDebug() << "Downloaded:" << imageData.size() << "bytes" << (isTiff ? "(TIFF)" : "(JPEG)");
            
            // **ADD THIS** - Save image to file for debugging
            saveImageToFile(imageData, filePath, ra, dec, exposure);
            
            if (isTiff) {
                // Snapshot - emit special signal
                m_snapshotInProgress = false;
                qDebug() << "Snapshot complete - resuming live stream";
                emit tiffImageDownloaded(filePath, imageData, ra, dec, exposure);
            } else {
                // Live stream
                QImage image;
                if (image.loadFromData(imageData)) {
                    m_lastImage = image;
                    m_imageReady = true;
                    qDebug() << "Image downloaded successfully, size:" << imageData.size() << "bytes";
                    emit liveImageDownloaded(imageData, ra, dec, exposure);
                } else {
                    qWarning() << "Failed to load image from downloaded data";
                }
            }
        } else {
            qWarning() << "Download error:" << reply->errorString();
            m_snapshotInProgress = false;
        }
        
        reply->deleteLater();
        manager->deleteLater();
    });
}

double OriginBackend::radiansToHours(double radians)
{
    return radians * 12.0 / M_PI; // 12 hours = π radians
}

double OriginBackend::radiansToDegrees(double radians)
{
    return radians * 180.0 / M_PI;
}

double OriginBackend::hoursToRadians(double hours)
{
    return hours * M_PI / 12.0;
}

double OriginBackend::degreesToRadians(double degrees)
{
    return degrees * M_PI / 180.0;
}

// Add this method to OriginBackend.cpp:
void OriginBackend::initializeLogging() {
    // Create logs directory in user's Documents
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString logDir = documentsPath + "/CelestronOriginLogs";
    QDir().mkpath(logDir);
    
    // Create log file with timestamp
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
    QString logFileName = QString("%1/websocket_log_%2.txt").arg(logDir, timestamp);
    
    m_logFile = new QFile(logFileName, this);
    if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_logStream = new QTextStream(m_logFile);
        m_logStream->setCodec("UTF-8");  // Change this line
        
        qDebug() << "WebSocket logging initialized:" << logFileName;
        logWebSocketMessage("SYSTEM", "=== WebSocket Logging Started ===");
    } else {
        qWarning() << "Failed to open log file:" << logFileName;
        delete m_logFile;
        m_logFile = nullptr;
    }
}

// Add this method to OriginBackend.cpp:
void OriginBackend::logWebSocketMessage(const QString& direction, const QString& message) {
    if (!m_logStream) return;
    
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString logEntry = QString("[%1] %2: %3").arg(timestamp, direction, message);
    
    *m_logStream << logEntry << Qt::endl;
    m_logStream->flush();
    
    // Also output to console for immediate viewing
    //    qDebug() << "WS" << direction << ":" << message;
}

// Add this method to OriginBackend.cpp:
void OriginBackend::cleanupLogging() {
    if (m_logStream) {
        logWebSocketMessage("SYSTEM", "=== WebSocket Logging Ended ===");
        delete m_logStream;
        m_logStream = nullptr;
    }
    
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
}

QString OriginBackend::getConnectedHost()
{
  return m_connectedHost;
}

// In OriginBackend.cpp:
void OriginBackend::setCameraConnected(bool connected)
{
    qDebug() << "setCameraConnected called with:" << connected;
    qDebug() << "  Physical connection:" << m_status.isConnected;
    qDebug() << "  Camera logical before:" << m_status.isCameraLogicallyConnected;
    
    if (connected && !m_status.isConnected) {
        qWarning() << "Cannot logically connect camera - no physical connection";
        return;
    }
    
    m_status.isCameraLogicallyConnected = connected;
    qDebug() << "  Camera logical after:" << m_status.isCameraLogicallyConnected;
}

bool OriginBackend::startExposure(double duration, int iso)
{
    if (!isLogicallyConnected()) {
        qWarning() << "Cannot start exposure - not connected";
        return false;
    }
    
    if (m_cameraState != CameraState::Idle) {
        qWarning() << "Cannot start exposure - camera busy";
        return false;
    }
    
    // Store exposure parameters
    m_lastExposureDuration = duration;
    m_currentGain = iso;
    m_lastExposureStartTime = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m_imageReady = false;
    m_lastImageData.clear();
    
    // Send command to Origin via WebSocket
    QJsonObject command;
    command["Command"] = "RunSampleCapture";
    command["Destination"] = "TaskController";
    command["Source"] = "AlpacaServer";
    command["SequenceID"] = m_nextSequenceId++;
    command["Type"] = "Command";
    command["ExposureTime"] = duration;
    command["ISO"] = iso;
    
    QString message = QJsonDocument(command).toJson(QJsonDocument::Compact);
    
    if (m_webSocket && m_webSocket->isValid()) {
        m_webSocket->sendTextMessage(message);
        m_cameraState = CameraState::Exposing;
        
        qDebug() << "Started exposure:" << duration << "sec, ISO:" << iso;
        emit exposureStarted();
        emit cameraStateChanged(static_cast<int>(m_cameraState));
        
        return true;
    }
    
    return false;
}

bool OriginBackend::abortExposure()
{
    if (m_cameraState != CameraState::Exposing) {
        return false;
    }
    
    // Send abort command to Origin
    QJsonObject command;
    command["Command"] = "AbortExposure";
    command["Destination"] = "Camera";
    command["Source"] = "AlpacaServer";
    command["SequenceID"] = m_nextSequenceId++;
    command["Type"] = "Command";
    
    QString message = QJsonDocument(command).toJson(QJsonDocument::Compact);
    
    if (m_webSocket && m_webSocket->isValid()) {
        m_webSocket->sendTextMessage(message);
        m_cameraState = CameraState::Idle;
        
        qDebug() << "Aborted exposure";
        emit cameraStateChanged(static_cast<int>(m_cameraState));
        
        return true;
    }
    
    return false;
}

bool OriginBackend::setGain(int gain)
{
    if (!isLogicallyConnected()) {
        return false;
    }
    
    // Origin uses ISO, which maps to gain
    m_currentGain = gain;
    
    // If you need to set gain for subsequent exposures, update camera parameters
    QJsonObject command;
    command["Command"] = "SetCaptureParameters";
    command["Destination"] = "Camera";
    command["Source"] = "AlpacaServer";
    command["SequenceID"] = m_nextSequenceId++;
    command["Type"] = "Command";
    command["ISO"] = gain;
    command["Exposure"] = m_lastExposureDuration;  // Keep current exposure
    
    QString message = QJsonDocument(command).toJson(QJsonDocument::Compact);
    
    if (m_webSocket && m_webSocket->isValid()) {
        m_webSocket->sendTextMessage(message);
        qDebug() << "Set gain/ISO to:" << gain;
        return true;
    }
    
    return false;
}

void OriginBackend::handleNewImageReady(const QJsonObject& data)
{
    QString fileLocation = data["FileLocation"].toString();
    double ra = data["Ra"].toDouble();
    double dec = data["Dec"].toDouble();
    
    qDebug() << "New image ready:" << fileLocation;
    qDebug() << "  Position: RA=" << ra*180.0/15.0/M_PI << "Dec=" << dec*180.0/M_PI;
    
    // Update camera state
    m_cameraState = CameraState::Reading;
    emit cameraStateChanged(static_cast<int>(m_cameraState));
    emit exposureComplete();
    
    // Store the path for download
    m_lastImagePath = fileLocation;
    
    // Automatically download the image
    downloadImage(fileLocation);
}

void OriginBackend::downloadImage(const QString& remotePath)
{
    if (!m_telescopeIP.isEmpty()) {
        // Construct URL to download image from Origin
        // Format: http://<telescope-ip>/SmartScope-1.0/dev2/<file-path>
      QUrl url = QUrl(QString("http://%1/SmartScope-1.0/dev2/%2")
                      .arg(m_telescopeIP, remotePath));
        
        qDebug() << "Downloading image from:" << url;
        
        QNetworkRequest request(url);
        request.setRawHeader("Cache-Control", "no-cache");
        
        QNetworkReply* reply = m_imageDownloader->get(request);
        
        // Store metadata in reply for later use
        reply->setProperty("remotePath", remotePath);
    }
}


void OriginBackend::onImageDownloadFinished(QNetworkReply* reply)
{
    reply->deleteLater();
    
    if (reply->error() == QNetworkReply::NoError) {
        // Successfully downloaded image
        m_lastImageData = reply->readAll();
        
        // Determine format from file extension
        QString remotePath = reply->property("remotePath").toString();
        if (remotePath.endsWith(".jpg", Qt::CaseInsensitive)) {
            m_lastImageFormat = "JPEG";
        } else if (remotePath.endsWith(".tiff", Qt::CaseInsensitive) || 
                   remotePath.endsWith(".tif", Qt::CaseInsensitive)) {
            m_lastImageFormat = "TIFF";
        } else {
            m_lastImageFormat = "RAW";
        }
        
        qDebug() << "Image downloaded:" << m_lastImageData.size() << "bytes";
        qDebug() << "Format:" << m_lastImageFormat;

        // **ADD THIS** - Save Alpaca-initiated images too
        saveImageToFile(m_lastImageData, remotePath, 
                       m_lastImageRa, m_lastImageDec, m_lastExposureDuration);

        // Mark image as ready for Alpaca client to fetch
        m_imageReady = true;
        m_cameraState = CameraState::Idle;
        
        emit imageReady(remotePath);
        emit cameraStateChanged(static_cast<int>(m_cameraState));
        
    } else {
        qWarning() << "Failed to download image:" << reply->errorString();
        m_cameraState = CameraState::Error;
        emit cameraStateChanged(static_cast<int>(m_cameraState));
    }
}

QString OriginBackend::createImageSavePath()
{
    // Create a directory in user's Documents for downloaded images
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString imageDir = documentsPath + "/CelestronOriginImages";
    
    // Create timestamped subdirectory for this session
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString sessionDir = imageDir + "/session_" + timestamp;
    
    QDir dir;
    if (dir.mkpath(sessionDir)) {
        qDebug() << "Created image save directory:" << sessionDir;
        return sessionDir;
    } else {
        qWarning() << "Failed to create image save directory:" << sessionDir;
        return imageDir;  // Fallback to parent directory
    }
}

void OriginBackend::setImageSavePath(const QString& path)
{
    QDir dir;
    if (dir.mkpath(path)) {
        m_imageSavePath = path;
        qDebug() << "Image save path set to:" << m_imageSavePath;
    } else {
        qWarning() << "Failed to create image save path:" << path;
    }
}

void OriginBackend::enableImageSaving(bool enable)
{
    m_saveImagesEnabled = enable;
    qDebug() << "Image saving" << (enable ? "enabled" : "disabled");
}

void OriginBackend::saveImageToFile(const QByteArray& imageData, 
                                    const QString& originalPath,
                                    double ra, double dec, double exposure)
{
    if (!m_saveImagesEnabled || imageData.isEmpty()) {
        return;
    }
    
    // Determine file extension from original path
    QString extension = "jpg";  // Default
    if (originalPath.endsWith(".tiff", Qt::CaseInsensitive) || 
        originalPath.endsWith(".tif", Qt::CaseInsensitive)) {
        extension = "tiff";
    } else if (originalPath.endsWith(".jpg", Qt::CaseInsensitive)) {
        extension = "jpg";
    } else if (originalPath.endsWith(".jpeg", Qt::CaseInsensitive)) {
        extension = "jpeg";
    }
    
    // Create filename with timestamp and metadata
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    QString raStr = QString::number(ra * 180.0 / M_PI / 15.0, 'f', 4);  // Convert to hours
    QString decStr = QString::number(dec * 180.0 / M_PI, 'f', 4);  // Convert to degrees
    
    QString filename = QString("image_%1_ra%2_dec%3_exp%4s.%5")
                       .arg(timestamp)
                       .arg(raStr)
                       .arg(decStr)
                       .arg(exposure, 0, 'f', 2)
                       .arg(extension);
    
    QString fullPath = m_imageSavePath + "/" + filename;
    
    // Save the image data
    QFile file(fullPath);
    if (file.open(QIODevice::WriteOnly)) {
        qint64 bytesWritten = file.write(imageData);
        file.close();
        
        if (bytesWritten == imageData.size()) {
            qDebug() << "Saved image:" << fullPath;
            qDebug() << "  Size:" << imageData.size() << "bytes";
            qDebug() << "  RA:" << raStr << "hours";
            qDebug() << "  Dec:" << decStr << "degrees";
            qDebug() << "  Exposure:" << exposure << "seconds";
            
            // Also save metadata to a text file
            QString metadataFile = fullPath + ".txt";
            QFile meta(metadataFile);
            if (meta.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&meta);
                out << "Image: " << filename << "\n";
                out << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
                out << "RA (hours): " << raStr << "\n";
                out << "Dec (degrees): " << decStr << "\n";
                out << "RA (radians): " << ra << "\n";
                out << "Dec (radians): " << dec << "\n";
                out << "Exposure (seconds): " << exposure << "\n";
                out << "Size (bytes): " << imageData.size() << "\n";
                out << "Format: " << extension.toUpper() << "\n";
                out << "Original path: " << originalPath << "\n";
                meta.close();
                qDebug() << "  Metadata saved:" << metadataFile;
            }
        } else {
            qWarning() << "Failed to write complete image data to:" << fullPath;
        }
    } else {
        qWarning() << "Failed to open file for writing:" << fullPath;
        qWarning() << "  Error:" << file.errorString();
    }
}
