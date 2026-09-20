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

#ifndef CLOUDSYNCMANAGER_H
#define CLOUDSYNCMANAGER_H

#include <QObject>
#include <QString>
#include <QJsonObject>

class QNetworkAccessManager;
class QTcpServer;
class QWidget;

class EmuInstance;

class CloudSyncManager : public QObject
{
    Q_OBJECT

public:
    explicit CloudSyncManager(EmuInstance* inst, QObject* parent = nullptr);
    ~CloudSyncManager();

    bool isSignedIn() const;
    bool autoSyncEnabled() const;

    // Pulls the latest version of the given save file from Google Drive
    // before a game boots. Blocks until done (bounded by timeouts), so the
    // save on disk is up to date when the ROM's save data is read.
    void bootSync(const QString& savPath);

public slots:
    // Called (queued) after SaveManager flushed a save file to disk.
    void onSaveFlushed(const QString& path);

    // Interactive OAuth sign-in (opens the browser, async).
    void signIn();
    void signOut();

    // Manual bidirectional sync of the current game's save.
    void syncNow();

signals:
    void syncStatus(const QString& msg, bool error);
    void signInChanged();

private:
    QWidget* parentWindow() const;
    QString clientID() const;
    QString clientSecret() const;

    // state file (tokens + per-file sync state)
    QString stateFilePath() const;
    void loadState();
    void saveState();

    // OAuth
    bool ensureAccessToken();
    bool refreshAccessToken();
    void startAuthServer();
    void finishAuth(const QString& code);

    // Drive API
    bool ensureFolder();
    bool findCloudFile(const QString& name, QJsonObject& result);
    bool uploadSave(const QString& localPath, const QJsonObject& cloudFile);
    bool downloadSave(const QJsonObject& cloudFile, const QString& localPath);

    // sync logic
    void syncFile(const QString& localPath, bool allowUI);
    void resolveConflict(const QString& localPath, const QJsonObject& cloudFile, bool allowUI);

    void reportStatus(const QString& msg, bool error);

    EmuInstance* emuInstance;
    QNetworkAccessManager* nam;

    // OAuth tokens
    QString accessToken;
    QString refreshToken;
    qint64 tokenExpiry = 0; // epoch seconds

    // OAuth sign-in flow
    QTcpServer* authServer = nullptr;
    QString authVerifier;
    QString authRedirectUri;
    QString authState;
    bool authInProgress = false;
    bool authHandled = false;

    // Drive state
    QString folderID;
    QJsonObject fileStates; // key: normalized local save path

    bool syncing = false;
};

#endif // CLOUDSYNCMANAGER_H
