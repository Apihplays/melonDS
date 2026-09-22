/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "CloudSyncManager.h"
#include "EmuInstance.h"
#include "Config.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QBuffer>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>

using namespace melonDS;

namespace {

// Google OAuth credentials for a "Desktop app" type OAuth client.
// Paste your own client ID/secret here, or set the CloudSync.ClientID /
// CloudSync.ClientSecret entries in melonDS.toml to override without
// rebuilding. See README.md ("Google Drive save sync") for instructions.
constexpr const char* kDefaultClientID = "";
constexpr const char* kDefaultClientSecret = "";

// Files are stored in a folder named this in the user's Drive root,
// created on first use. We only ask for the drive.file scope: the app
// can only see and touch files it created itself.
constexpr const char* kDriveFolderName = "melonDS";

const QUrl kAuthUrl("https://accounts.google.com/o/oauth2/v2/auth");
const QUrl kTokenUrl("https://oauth2.googleapis.com/token");
const QString kDriveApiBase = "https://www.googleapis.com/drive/v3/files";
const QString kDriveUploadBase = "https://www.googleapis.com/upload/drive/v3/files";
const QString kDriveScope = "https://www.googleapis.com/auth/drive.file";

constexpr const char* kStateFile = "cloudsync.json";
constexpr int kHttpTimeoutMs = 10000;

// RAII guard so syncFile never leaves the re-entrancy flag set.
struct SyncGuard
{
    bool& flag;
    ~SyncGuard() { flag = false; }
};

struct HttpResult
{
    bool ok = false;
    int status = 0;
    QByteArray body;
    QString error;
};

HttpResult performRequest(QNetworkAccessManager* nam, const QByteArray& token,
                          const QString& method, const QUrl& url,
                          const QByteArray& contentType, const QByteArray& data)
{
    QNetworkRequest req(url);
    if (!token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + token);
    if (!contentType.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply;
    if (method == "POST")
        reply = nam->post(req, data);
    else if (method == "PUT")
        reply = nam->put(req, data);
    else if (method == "PATCH")
    {
        QBuffer* buf = new QBuffer();
        buf->setData(data);
        buf->open(QIODevice::ReadOnly);
        reply = nam->sendCustomRequest(req, "PATCH", buf);
        buf->setParent(reply);
    }
    else if (method == "DELETE")
        reply = nam->deleteResource(req);
    else
        reply = nam->get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(kHttpTimeoutMs);
    loop.exec();

    if (!timer.isActive())
    { // timed out
        reply->abort();
    }
    timer.stop();

    HttpResult res;
    res.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    res.body = reply->readAll();
    res.ok = (reply->error() == QNetworkReply::NoError) && (res.status >= 200) && (res.status < 300);
    if (!res.ok)
    {
        res.error = reply->errorString();

        // try to surface Google's own error message, which is far more useful
        QJsonObject errobj = QJsonDocument::fromJson(res.body).object();
        QJsonValue errval = errobj["error"];
        if (errval.isObject())
        {
            QString msg = errval.toObject()["message"].toString();
            if (!msg.isEmpty()) res.error = msg;
        }
        else if (errval.isString())
        {
            res.error = errval.toString();
        }
        else if (!errobj["error_description"].toString().isEmpty())
        {
            res.error = errobj["error_description"].toString();
        }
    }

    reply->deleteLater();
    return res;
}

QJsonObject parseJson(const HttpResult& res)
{
    return QJsonDocument::fromJson(res.body).object();
}

QString fileMd5(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();

    QCryptographicHash hash(QCryptographicHash::Md5);
    char buf[65536];
    qint64 n;
    while ((n = f.read(buf, sizeof(buf))) > 0)
        hash.addData(QByteArrayView(buf, n));

    return QString::fromLatin1(hash.result().toHex());
}

QString normalizePath(const QString& path)
{
    return QDir::fromNativeSeparators(path);
}

}

CloudSyncManager::CloudSyncManager(EmuInstance* inst, QObject* parent)
    : QObject(parent), emuInstance(inst)
{
    nam = new QNetworkAccessManager(this);
    loadState();
}

CloudSyncManager::~CloudSyncManager()
{
    if (authServer)
    {
        authServer->close();
        delete authServer;
    }
}

bool CloudSyncManager::isSignedIn() const
{
    return !refreshToken.isEmpty();
}

bool CloudSyncManager::autoSyncEnabled() const
{
    return emuInstance->getGlobalConfig().GetBool("CloudSync.Enabled");
}

QWidget* CloudSyncManager::parentWindow() const
{
    return emuInstance ? emuInstance->getMainWindow() : nullptr;
}

QString CloudSyncManager::clientID() const
{
    QString id = emuInstance->getGlobalConfig().GetQString("CloudSync.ClientID");
    if (id.isEmpty()) id = QString::fromLatin1(kDefaultClientID);
    return id;
}

QString CloudSyncManager::clientSecret() const
{
    QString secret = emuInstance->getGlobalConfig().GetQString("CloudSync.ClientSecret");
    if (secret.isEmpty()) secret = QString::fromLatin1(kDefaultClientSecret);
    return secret;
}


QString CloudSyncManager::stateFilePath() const
{
    return QString::fromStdString(Platform::GetLocalFilePath(kStateFile));
}

void CloudSyncManager::loadState()
{
    QFile f(stateFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;

    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    QJsonObject tokens = root["tokens"].toObject();
    refreshToken = tokens["refresh_token"].toString();
    accessToken = tokens["access_token"].toString();
    tokenExpiry = (qint64)tokens["expires_at"].toDouble();
    folderID = root["folder_id"].toString();
    fileStates = root["files"].toObject();
}

void CloudSyncManager::saveState()
{
    QJsonObject tokens;
    tokens["refresh_token"] = refreshToken;
    tokens["access_token"] = accessToken;
    tokens["expires_at"] = (double)tokenExpiry;

    QJsonObject root;
    root["tokens"] = tokens;
    root["folder_id"] = folderID;
    root["files"] = fileStates;

    QSaveFile f(stateFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) return;

    // keep the token file readable only by the user where the OS supports it
    QFile::setPermissions(stateFilePath(), QFile::ReadOwner | QFile::WriteOwner);
}


bool CloudSyncManager::ensureAccessToken()
{
    if (!isSignedIn()) return false;

    qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!accessToken.isEmpty() && now < tokenExpiry)
        return true;

    return refreshAccessToken();
}

bool CloudSyncManager::refreshAccessToken()
{
    QUrlQuery body;
    body.addQueryItem("client_id", clientID());
    body.addQueryItem("client_secret", clientSecret());
    body.addQueryItem("refresh_token", refreshToken);
    body.addQueryItem("grant_type", "refresh_token");

    HttpResult res = performRequest(nam, QByteArray(), "POST", kTokenUrl,
                                    "application/x-www-form-urlencoded",
                                    body.toString(QUrl::FullyEncoded).toUtf8());
    QJsonObject resp = parseJson(res);

    if (!res.ok || resp["access_token"].toString().isEmpty())
    {
        reportStatus("Google Drive: token refresh failed. Sign in again. (" + res.error + ")", true);
        return false;
    }

    accessToken = resp["access_token"].toString();
    tokenExpiry = QDateTime::currentSecsSinceEpoch() + resp["expires_in"].toInt(3600) - 60;
    saveState();
    return true;
}

void CloudSyncManager::signIn()
{
    if (isSignedIn())
    {
        reportStatus("Google Drive: already signed in.", false);
        return;
    }
    if (authInProgress) return;

    if (clientID().isEmpty() || clientSecret().isEmpty())
    {
        QMessageBox::warning(parentWindow(), "melonDS",
            "No Google OAuth client ID is configured.\n\n"
            "Create a \"Desktop app\" OAuth client in the Google Cloud Console, "
            "then paste the client ID and secret into the CloudSync.ClientID and "
            "CloudSync.ClientSecret entries in melonDS.toml (or into "
            "CloudSyncManager.cpp). See README.md for details.");
        reportStatus("Google Drive: no OAuth client ID configured. See README.md.", true);
        return;
    }

    startAuthServer();
}

void CloudSyncManager::startAuthServer()
{
    if (authServer)
    {
        authServer->deleteLater();
        authServer = nullptr;
    }

    // PKCE code verifier (RFC 7636)
    QString verifier;
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    for (int i = 0; i < 64; i++)
        verifier += chars[QRandomGenerator::system()->bounded((int)sizeof(chars) - 1)];
    QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    authState = QString::number(QRandomGenerator::system()->generate64(), 16);

    authServer = new QTcpServer(this);
    if (!authServer->listen(QHostAddress::LocalHost))
    {
        delete authServer;
        authServer = nullptr;
        reportStatus("Google Drive: cannot open a local port for sign-in.", true);
        return;
    }

    authVerifier = verifier;
    authRedirectUri = QString("http://127.0.0.1:%1").arg(authServer->serverPort());
    authInProgress = true;
    authHandled = false;

    QObject::connect(authServer, &QTcpServer::newConnection, this, [this]()
    {
        QTcpSocket* sock = authServer->nextPendingConnection();
        if (!sock) return;

        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
        QObject::connect(sock, &QTcpSocket::readyRead, this, [this, sock]()
        {
            if (authHandled)
            { // ignore stray requests (favicon etc.)
                sock->disconnectFromHost();
                return;
            }

            QByteArray req = sock->readAll();
            if (!req.contains("\r\n\r\n")) return; // wait for the full request

            QString path = QString::fromLatin1(req).section(' ', 1, 1);
            QUrlQuery params(QUrl("http://localhost" + path).query());

            QByteArray resp =
                "HTTP/1.0 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "\r\n"
                "<html><body><h2>melonDS</h2>"
                "<p>Sign-in complete. You can close this window and return to melonDS.</p>"
                "</body></html>";
            sock->write(resp);
            sock->disconnectFromHost();
            authHandled = true;
            authServer->close();

            QString error = params.queryItemValue("error");
            QString code = params.queryItemValue("code");
            QString state = params.queryItemValue("state");

            if (code.isEmpty() || state != authState)
            {
                authInProgress = false;
                reportStatus("Google Drive: sign-in failed."
                                 + (error.isEmpty() ? QString() : QString(" (" + error + ")")), true);
                return;
            }

            finishAuth(code);
        });
    });

    QUrlQuery q;
    q.addQueryItem("client_id", clientID());
    q.addQueryItem("redirect_uri", authRedirectUri);
    q.addQueryItem("response_type", "code");
    q.addQueryItem("scope", kDriveScope);
    q.addQueryItem("code_challenge", challenge);
    q.addQueryItem("code_challenge_method", "S256");
    q.addQueryItem("state", authState);
    q.addQueryItem("access_type", "offline");
    q.addQueryItem("prompt", "consent"); // force a refresh token to be issued

    QUrl url(kAuthUrl);
    url.setQuery(q);
    QDesktopServices::openUrl(url);

    QTimer::singleShot(5 * 60 * 1000, this, [this]()
    {
        if (authInProgress && authServer)
        {
            authServer->close();
            authInProgress = false;
            reportStatus("Google Drive: sign-in timed out.", true);
        }
    });

    reportStatus("Google Drive: complete sign-in in your browser...", false);
}

void CloudSyncManager::finishAuth(const QString& code)
{
    QUrlQuery body;
    body.addQueryItem("code", code);
    body.addQueryItem("client_id", clientID());
    body.addQueryItem("client_secret", clientSecret());
    body.addQueryItem("redirect_uri", authRedirectUri);
    body.addQueryItem("grant_type", "authorization_code");
    body.addQueryItem("code_verifier", authVerifier);

    HttpResult res = performRequest(nam, QByteArray(), "POST", kTokenUrl,
                                    "application/x-www-form-urlencoded",
                                    body.toString(QUrl::FullyEncoded).toUtf8());
    QJsonObject resp = parseJson(res);
    authInProgress = false;

    if (!res.ok || resp["access_token"].toString().isEmpty())
    {
        reportStatus("Google Drive: sign-in failed (" + res.error + ")", true);
        return;
    }

    accessToken = resp["access_token"].toString();
    if (!resp["refresh_token"].toString().isEmpty())
        refreshToken = resp["refresh_token"].toString();
    tokenExpiry = QDateTime::currentSecsSinceEpoch() + resp["expires_in"].toInt(3600) - 60;
    saveState();

    reportStatus("Google Drive: signed in.", false);
    emit signInChanged();
}

void CloudSyncManager::signOut()
{
    if (authServer)
    {
        authServer->close();
        delete authServer;
        authServer = nullptr;
    }
    authInProgress = false;

    refreshToken.clear();
    accessToken.clear();
    tokenExpiry = 0;
    folderID.clear();
    fileStates = QJsonObject();
    QFile::remove(stateFilePath());

    reportStatus("Google Drive: signed out.", false);
    emit signInChanged();
}


bool CloudSyncManager::ensureFolder()
{
    if (!folderID.isEmpty()) return true;

    QUrl url(kDriveApiBase);
    QUrlQuery q;
    q.addQueryItem("q", QString("name = '%1' and mimeType = 'application/vnd.google-apps.folder' "
                                "and trashed = false").arg(kDriveFolderName));
    q.addQueryItem("fields", "files(id)");
    q.addQueryItem("pageSize", "1");
    url.setQuery(q);

    HttpResult res = performRequest(nam, accessToken.toUtf8(), "GET", url, {}, {});
    if (!res.ok)
    {
        reportStatus("Google Drive: cannot look up save folder (" + res.error + ")", true);
        return false;
    }

    QJsonArray files = parseJson(res)["files"].toArray();
    if (!files.isEmpty())
    {
        folderID = files[0].toObject()["id"].toString();
        saveState();
        return true;
    }

    // folder doesn't exist yet, create it
    QJsonObject meta;
    meta["name"] = kDriveFolderName;
    meta["mimeType"] = "application/vnd.google-apps.folder";

    QUrl createUrl(kDriveApiBase);
    QUrlQuery createQuery;
    createQuery.addQueryItem("fields", "id");
    createUrl.setQuery(createQuery);

    HttpResult res2 = performRequest(nam, accessToken.toUtf8(), "POST", createUrl,
                                     "application/json; charset=UTF-8",
                                     QJsonDocument(meta).toJson(QJsonDocument::Compact));
    QJsonObject resp = parseJson(res2);
    if (!res2.ok || resp["id"].toString().isEmpty())
    {
        reportStatus("Google Drive: cannot create save folder (" + res2.error + ")", true);
        return false;
    }

    folderID = resp["id"].toString();
    saveState();
    reportStatus("Google Drive: created folder 'melonDS'.", false);
    return true;
}

bool CloudSyncManager::findCloudFile(const QString& name, QJsonObject& result)
{
    result = QJsonObject();
    if (!ensureFolder()) return false;

    QUrl url(kDriveApiBase);
    QUrlQuery q;
    q.addQueryItem("q", QString("name = '%1' and '%2' in parents and trashed = false")
                            .arg(QString(name).replace('\'', "\\'"), folderID));
    q.addQueryItem("fields", "files(id, name, md5Checksum, modifiedTime)");
    q.addQueryItem("pageSize", "1");
    url.setQuery(q);

    HttpResult res = performRequest(nam, accessToken.toUtf8(), "GET", url, {}, {});
    if (!res.ok)
    {
        reportStatus("Google Drive sync failed (" + res.error + ")", true);
        return false;
    }

    QJsonArray files = parseJson(res)["files"].toArray();
    if (!files.isEmpty())
        result = files[0].toObject();

    return true;
}

bool CloudSyncManager::uploadSave(const QString& localPath, const QJsonObject& cloudFile)
{
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly))
    {
        reportStatus("Google Drive: cannot read " + localPath, true);
        return false;
    }
    QByteArray data = f.readAll();
    f.close();

    QString localMd5 = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
    QString fileID = cloudFile["id"].toString();

    HttpResult res;
    if (!fileID.isEmpty())
    { // update the existing cloud file
        QUrl url(kDriveUploadBase + "/" + fileID);
        QUrlQuery q;
        q.addQueryItem("uploadType", "media");
        url.setQuery(q);
        res = performRequest(nam, accessToken.toUtf8(), "PATCH", url,
                             "application/octet-stream", data);
    }
    else
    { // create a new cloud file (metadata + content in one multipart request)
        if (!ensureFolder()) return false;

        QString boundary = QString("melonds%1").arg(QRandomGenerator::system()->generate64(), 16, 16, QChar('0'));
        QByteArray boundaryBytes = boundary.toLatin1();

        QJsonObject meta;
        meta["name"] = QFileInfo(localPath).fileName();
        meta["parents"] = QJsonArray{ folderID };

        QByteArray body;
        body += "--" + boundaryBytes + "\r\n";
        body += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
        body += QJsonDocument(meta).toJson(QJsonDocument::Compact);
        body += "\r\n--" + boundaryBytes + "\r\n";
        body += "Content-Type: application/octet-stream\r\n\r\n";
        body += data;
        body += "\r\n--" + boundaryBytes + "--";

        QUrl url(kDriveUploadBase);
        QUrlQuery q;
        q.addQueryItem("uploadType", "multipart");
        q.addQueryItem("fields", "id");
        url.setQuery(q);
        res = performRequest(nam, accessToken.toUtf8(), "POST", url,
                             "multipart/related; boundary=" + boundaryBytes, body);
    }

    if (!res.ok)
    {
        reportStatus("Google Drive: upload failed (" + res.error + ")", true);
        return false;
    }

    if (fileID.isEmpty())
        fileID = parseJson(res)["id"].toString();

    QJsonObject st;
    st["cloud_id"] = fileID;
    st["cloud_md5"] = localMd5; // the cloud now holds exactly this content
    st["local_md5"] = localMd5;
    fileStates[normalizePath(localPath)] = st;
    saveState();

    reportStatus("Save uploaded to Google Drive.", false);
    return true;
}

bool CloudSyncManager::downloadSave(const QJsonObject& cloudFile, const QString& localPath)
{
    QUrl url(kDriveApiBase + "/" + cloudFile["id"].toString());
    QUrlQuery q;
    q.addQueryItem("alt", "media");
    url.setQuery(q);

    HttpResult res = performRequest(nam, accessToken.toUtf8(), "GET", url, {}, {});
    if (!res.ok)
    {
        reportStatus("Google Drive: download failed (" + res.error + ")", true);
        return false;
    }

    QString md5 = QString::fromLatin1(
        QCryptographicHash::hash(res.body, QCryptographicHash::Md5).toHex());

    // create a timestamped local backup of the existing save before replacing it
    if (QFile::exists(localPath))
    {
        QString backupDir = QFileInfo(localPath).absolutePath() + "/save_backups";
        QDir().mkpath(backupDir);
        QString timeStamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
        QString backupPath = QString("%1/%2.%3.bak")
                                .arg(backupDir, QFileInfo(localPath).fileName(), timeStamp);
        QFile::copy(localPath, backupPath);
    }

    // write atomically so a failed download can't clobber the local save
    QSaveFile f(localPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || (f.write(res.body) != res.body.size())
            || !f.commit())
    {
        reportStatus("Google Drive: cannot write " + localPath, true);
        return false;
    }

    QJsonObject st;
    st["cloud_id"] = cloudFile["id"].toString();
    st["cloud_md5"] = md5;
    st["local_md5"] = md5;
    fileStates[normalizePath(localPath)] = st;
    saveState();

    reportStatus("Save downloaded from Google Drive.", false);
    return true;
}


void CloudSyncManager::resolveConflict(const QString& localPath, const QJsonObject& cloudFile,
                                       bool allowUI)
{
    enum Choice { KeepLocal = 0, KeepCloud, KeepBoth, Cancel };
    Choice choice = KeepBoth;

    if (allowUI && parentWindow())
    {
        // If caller is in background thread, choose KeepBoth safely to prevent GUI deadlock.
        if (QThread::currentThread() != parentWindow()->thread())
        {
            choice = KeepBoth;
        }
        else
        {
            QMessageBox box(parentWindow());
            box.setWindowTitle("melonDS - Google Drive sync");
            box.setIcon(QMessageBox::Warning);
            box.setText(tr("The save file %1 was modified both here and on another device.\n\n"
                           "Which version do you want to keep?")
                            .arg(QFileInfo(localPath).fileName()));
            QPushButton* keepLocal = box.addButton(tr("Keep local"), QMessageBox::AcceptRole);
            QPushButton* keepCloud = box.addButton(tr("Keep cloud"), QMessageBox::DestructiveRole);
            QPushButton* keepBoth = box.addButton(tr("Keep both"), QMessageBox::ActionRole);
            box.addButton(QMessageBox::Cancel);
            box.exec();

            if (box.clickedButton() == keepLocal) choice = KeepLocal;
            else if (box.clickedButton() == keepCloud) choice = KeepCloud;
            else if (box.clickedButton() == keepBoth) choice = KeepBoth;
            else choice = Cancel;
        }
    }

    switch (choice)
    {
        case KeepLocal:
            uploadSave(localPath, cloudFile);
            break;

        case KeepCloud:
            // if a game is running, the downloaded save will apply next boot
            downloadSave(cloudFile, localPath);
            break;

        case KeepBoth:
        {
            // preserve the cloud version next to the save, then upload the local one
            QString conflictPath = localPath + ".conflict";
            if (downloadSave(cloudFile, conflictPath))
                uploadSave(localPath, cloudFile);
            // the .conflict copy is not the synced file, drop its state entry
            fileStates.remove(normalizePath(conflictPath));
            saveState();
            break;
        }

        case Cancel:
            break;
    }
}

void CloudSyncManager::syncFile(const QString& localPath, bool allowUI)
{
    if (syncing) return;
    syncing = true;
    SyncGuard guard{syncing};

    if (!ensureAccessToken()) return;

    QFileInfo fi(localPath);
    QString name = fi.fileName();
    QJsonObject st = fileStates[normalizePath(localPath)].toObject();
    QString localMd5 = fi.exists() ? fileMd5(localPath) : QString();

    QJsonObject cloud;
    if (!findCloudFile(name, cloud)) return; // network/auth error already reported

    bool haveLocal = fi.exists();
    bool haveCloud = !cloud.isEmpty();

    if (!haveLocal && !haveCloud) return;

    if (!haveCloud)
    {
        uploadSave(localPath, cloud);
        return;
    }
    if (!haveLocal)
    {
        downloadSave(cloud, localPath);
        return;
    }

    // never synced this file from this device: pick a side by modification time
    if (st.isEmpty())
    {
        QDateTime cloudTime = QDateTime::fromString(cloud["modifiedTime"].toString(), Qt::ISODateWithMs);
        if (cloudTime.isValid() && cloudTime > fi.lastModified())
            downloadSave(cloud, localPath);
        else
            uploadSave(localPath, cloud);
        return;
    }

    bool localChanged = (localMd5 != st["local_md5"].toString());
    bool cloudChanged = (cloud["md5Checksum"].toString() != st["cloud_md5"].toString());

    if (localChanged && cloudChanged)
        resolveConflict(localPath, cloud, allowUI);
    else if (localChanged)
        uploadSave(localPath, cloud);
    else if (cloudChanged)
        downloadSave(cloud, localPath);
}


void CloudSyncManager::bootSync(const QString& savPath)
{
    if (!autoSyncEnabled() || !isSignedIn()) return;

    reportStatus("Syncing save with Google Drive...", false);
    syncFile(savPath, true);
}

void CloudSyncManager::onSaveFlushed(const QString& path)
{
    if (!autoSyncEnabled() || !isSignedIn()) return;

    syncFile(path, true);
}

void CloudSyncManager::syncNow()
{
    if (!isSignedIn())
    {
        reportStatus("Google Drive: not signed in.", true);
        return;
    }

    if (!emuInstance->ndsSave || emuInstance->ndsSave->GetPath().empty())
    {
        reportStatus("Google Drive: no game save to sync.", false);
        return;
    }

    syncFile(QString::fromStdString(emuInstance->ndsSave->GetPath()), true);
}

void CloudSyncManager::reportStatus(const QString& msg, bool error)
{
    emit syncStatus(msg, error);

    if (emuInstance)
        emuInstance->osdAddMessage(error ? 0xFFA0A0 : 0, "%s", msg.toStdString().c_str());
}
