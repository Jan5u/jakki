#pragma once

#include <QEventLoop>
#include <QObject>
#include <QString>
#include <iostream>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <qt6keychain/keychain.h>
#include <vector>

class Auth : public QObject {
    Q_OBJECT

  public:
    Auth();
    ~Auth();
    bool loadOrGenerateKeys();
    QString getPublicKeyHex() const;
    QString getUsername() const;
    void setUsername(const QString &username);
    std::vector<uint8_t> signChallenge(const std::vector<uint8_t> &challenge);
    bool hasKeys() const { return keypair != nullptr; }

  signals:
    void keysReady();
    void keyError(const QString &error);

  private:
    EVP_PKEY *keypair;
    std::vector<uint8_t> publicKeyBytes;
    QString username;
    bool generateKeys();
    bool savePrivateKey();
    bool loadPrivateKey();
    bool derivePublicKey();
    QString bytesToHex(const std::vector<uint8_t> &bytes) const;
    std::vector<uint8_t> hexToBytes(const QString &hex) const;
};
