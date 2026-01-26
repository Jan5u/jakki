#include "auth.hpp"

Auth::Auth() : keypair(nullptr) {}

Auth::~Auth() {
    if (keypair) {
        EVP_PKEY_free(keypair);
        keypair = nullptr;
    }
}

bool Auth::loadOrGenerateKeys() {
    std::cout << "Loading or generating Ed25519 keys..." << std::endl;
    if (loadPrivateKey()) {
        std::cout << "Successfully loaded existing keys from keychain" << std::endl;
        if (derivePublicKey()) {
            std::cout << "Public key: " << getPublicKeyHex().toStdString() << std::endl;
            emit keysReady();
            return true;
        }
    }

    std::cout << "No existing keys found, generating new keypair..." << std::endl;
    if (generateKeys()) {
        std::cout << "Successfully generated new Ed25519 keypair" << std::endl;
        std::cout << "Public key: " << getPublicKeyHex().toStdString() << std::endl;

        if (savePrivateKey()) {
            std::cout << "Successfully saved private key to keychain" << std::endl;
            emit keysReady();
            return true;
        } else {
            std::cerr << "Failed to save private key to keychain" << std::endl;
            emit keyError("Failed to save private key to keychain");
            return false;
        }
    }

    std::cerr << "Failed to generate keys" << std::endl;
    emit keyError("Failed to generate Ed25519 keys");
    return false;
}

bool Auth::generateKeys() {
    std::cout << "Generating Ed25519 keypair..." << std::endl;

    // Generate Ed25519 keypair
    keypair = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    if (!keypair) {
        std::cerr << "Failed to generate Ed25519 keypair" << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }

    return derivePublicKey();
}

bool Auth::derivePublicKey() {
    if (!keypair) {
        std::cerr << "No keypair available to derive public key" << std::endl;
        return false;
    }

    publicKeyBytes.resize(32);
    size_t pubkey_len = publicKeyBytes.size();
    if (EVP_PKEY_get_raw_public_key(keypair, publicKeyBytes.data(), &pubkey_len) != 1) {
        std::cerr << "Failed to extract public key" << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }
    if (pubkey_len != 32) {
        std::cerr << "Invalid public key length: " << pubkey_len << " (expected 32)" << std::endl;
        return false;
    }

    return true;
}

bool Auth::savePrivateKey() {
    if (!keypair) {
        std::cerr << "No keypair to save" << std::endl;
        return false;
    }
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        std::cerr << "Failed to create BIO" << std::endl;
        return false;
    }
    if (PEM_write_bio_PrivateKey(bio, keypair, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        std::cerr << "Failed to write private key to BIO" << std::endl;
        ERR_print_errors_fp(stderr);
        BIO_free(bio);
        return false;
    }
    char *pem_data = nullptr;
    long pem_len = BIO_get_mem_data(bio, &pem_data);
    QString pemString = QString::fromUtf8(pem_data, pem_len);
    BIO_free(bio);

    QEventLoop loop;
    QKeychain::WritePasswordJob job("jakki");
    job.setAutoDelete(false);
    job.setKey("ed25519_private");
    job.setTextData(pemString);

    bool success = false;
    QString errorMsg;

    connect(&job, &QKeychain::WritePasswordJob::finished, [&](QKeychain::Job *j) {
        if (j->error()) {
            errorMsg = j->errorString();
            std::cerr << "QtKeychain write error: " << errorMsg.toStdString() << std::endl;
        } else {
            success = true;
        }
        loop.quit();
    });

    job.start();
    loop.exec();

    return success;
}

bool Auth::loadPrivateKey() {
    std::cout << "Loading private key from keychain..." << std::endl;

    QEventLoop loop;
    QKeychain::ReadPasswordJob job("jakki");
    job.setAutoDelete(false);
    job.setKey("ed25519_private");

    bool success = false;
    QString pemString;

    connect(&job, &QKeychain::ReadPasswordJob::finished, [&](QKeychain::Job *j) {
        if (j->error()) {
            if (j->error() == QKeychain::Error::EntryNotFound) {
                std::cout << "No existing key found in keychain" << std::endl;
            } else {
                std::cerr << "QtKeychain read error: " << j->errorString().toStdString() << std::endl;
            }
        } else {
            QKeychain::ReadPasswordJob *readJob = static_cast<QKeychain::ReadPasswordJob *>(j);
            pemString = readJob->textData();
            success = true;
        }
        loop.quit();
    });

    job.start();
    loop.exec();

    if (!success) {
        return false;
    }

    // Convert PEM string to EVP_PKEY
    QByteArray pemBytes = pemString.toUtf8();
    BIO *bio = BIO_new_mem_buf(pemBytes.constData(), pemBytes.size());
    if (!bio) {
        std::cerr << "Failed to create BIO from PEM data" << std::endl;
        return false;
    }

    keypair = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!keypair) {
        std::cerr << "Failed to parse private key from PEM" << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }

    return true;
}

QString Auth::getPublicKeyHex() const { return bytesToHex(publicKeyBytes); }

QString Auth::getUsername() const { return username; }

void Auth::setUsername(const QString &newUsername) {
    username = newUsername;
    std::cout << "Username set to: " << username.toStdString() << std::endl;
}

std::vector<uint8_t> Auth::signChallenge(const std::vector<uint8_t> &challenge) {
    if (!keypair) {
        std::cerr << "No keypair available for signing" << std::endl;
        return {};
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        std::cerr << "Failed to create EVP_MD_CTX" << std::endl;
        return {};
    }
    if (EVP_DigestSignInit(mdctx, nullptr, nullptr, nullptr, keypair) != 1) {
        std::cerr << "Failed to initialize signing" << std::endl;
        ERR_print_errors_fp(stderr);
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    size_t sig_len = 0;
    if (EVP_DigestSign(mdctx, nullptr, &sig_len, challenge.data(), challenge.size()) != 1) {
        std::cerr << "Failed to determine signature length" << std::endl;
        ERR_print_errors_fp(stderr);
        EVP_MD_CTX_free(mdctx);
        return {};
    }

    std::vector<uint8_t> signature(sig_len);
    if (EVP_DigestSign(mdctx, signature.data(), &sig_len, challenge.data(), challenge.size()) != 1) {
        std::cerr << "Failed to create signature" << std::endl;
        ERR_print_errors_fp(stderr);
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    EVP_MD_CTX_free(mdctx);
    signature.resize(sig_len);
    std::cout << "Created signature of length: " << sig_len << " bytes" << std::endl;

    return signature;
}

QString Auth::bytesToHex(const std::vector<uint8_t> &bytes) const {
    QString hex;
    hex.reserve(bytes.size() * 2);

    for (uint8_t byte : bytes) {
        hex.append(QString("%1").arg(byte, 2, 16, QChar('0')));
    }

    return hex;
}

std::vector<uint8_t> Auth::hexToBytes(const QString &hex) const {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);

    for (int i = 0; i < hex.length(); i += 2) {
        QString byteString = hex.mid(i, 2);
        uint8_t byte = static_cast<uint8_t>(byteString.toUInt(nullptr, 16));
        bytes.push_back(byte);
    }

    return bytes;
}
