#pragma once

#include <iostream>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <vector>

#include <libsecret/secret.h>

class Auth {
  public:
    Auth();
    ~Auth();
    bool loadOrGenerateKeys();
    std::string getPublicKeyHex() const;
    std::string getUsername() const;
    void setUsername(const std::string &username);
    std::vector<uint8_t> signChallenge(const std::vector<uint8_t> &challenge);
    bool hasKeys() const { return keypair != nullptr; }

  // signals:
  //   void keysReady();
  //   void keyError(const QString &error);

  private:
    EVP_PKEY *keypair;
    std::vector<uint8_t> publicKeyBytes;
    std::string username;
    bool generateKeys();
    bool savePrivateKey();
    bool loadPrivateKey();
    bool derivePublicKey();
    // QString bytesToHex(const std::vector<uint8_t> &bytes) const;
    // std::vector<uint8_t> hexToBytes(const QString &hex) const;
};
