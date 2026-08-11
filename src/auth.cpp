#include "auth.hpp"

Auth::Auth() {}

Auth::~Auth() {}

static std::string pemEncodePrivateKey(EVP_PKEY *key) {
    if (!key) {
        return {};
    }

    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        std::cerr << "Failed to create BIO" << std::endl;
        return {};
    }
    if (PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        std::cerr << "Failed to write private key to BIO" << std::endl;
        ERR_print_errors_fp(stderr);
        BIO_free(bio);
        return {};
    }
    char *pem_data = nullptr;
    long pem_len = BIO_get_mem_data(bio, &pem_data);
    std::string pemString;
    if (pem_len > 0 && pem_data)
        pemString.assign(pem_data, pem_len);
    BIO_free(bio);
    return pemString;
}

static EVP_PKEY *pemDecodePrivateKey(const std::string &pemString) {
    BIO *bio = BIO_new_mem_buf(pemString.data(), static_cast<int>(pemString.size()));
    if (!bio) {
        std::cerr << "Failed to create BIO from PEM data" << std::endl;
        return nullptr;
    }

    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!key) {
        std::cerr << "Failed to parse private key from PEM" << std::endl;
        ERR_print_errors_fp(stderr);
    }

    return key;
}

#ifndef _WIN32

static const SecretSchema *jakki_get_schema() {
    static const SecretSchema schema = {
        "org.jakki.PrivateKey",
        SECRET_SCHEMA_NONE,
        {
        {NULL, static_cast<SecretSchemaAttributeType>(0)},
        }
    };
    return &schema;
}

bool Auth::savePrivateKey() {
    if (!keypair) {
        std::cerr << "No keypair to save" << std::endl;
        return false;
    }

    std::string pemString = pemEncodePrivateKey(keypair);
    if (pemString.empty()) {
        return false;
    }

    GError *error = NULL;
    const SecretSchema *schema = jakki_get_schema();
    const char *label = "jakki ed25519 private key";

    /* store synchronously; no attributes */
    secret_password_store_sync(schema, SECRET_COLLECTION_DEFAULT, label, pemString.c_str(), NULL, &error, NULL);
    if (error) {
        std::cerr << "libsecret store error: " << error->message << std::endl;
        g_error_free(error);
        return false;
    }

    return true;
}

bool Auth::loadPrivateKey() {
    GError *error = NULL;
    const SecretSchema *schema = jakki_get_schema();

    gchar *pem_c = secret_password_lookup_sync(schema, NULL, &error, NULL);
    if (error) {
        std::cerr << "libsecret lookup error: " << error->message << std::endl;
        g_error_free(error);
        return false;
    }
    if (!pem_c) {
        std::cout << "No existing key found in secret service" << std::endl;
        return false;
    }

    std::string pemString(pem_c);
    secret_password_free(pem_c);

    keypair = pemDecodePrivateKey(pemString);
    return keypair != nullptr;
}

#else

bool Auth::savePrivateKey() {
    std::cerr << "Key storage is not implemented on Windows yet" << std::endl;
    return false;
}

bool Auth::loadPrivateKey() {
    std::cerr << "Key storage is not implemented on Windows yet" << std::endl;
    return false;
}

#endif

bool Auth::generateKeys()
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!pctx) {
        std::cerr << "Failed to create EVP_PKEY_CTX" << std::endl;
        return false;
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0) {
        std::cerr << "Failed to init keygen" << std::endl;
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    if (EVP_PKEY_keygen(pctx, &keypair) <= 0) {
        std::cerr << "Failed to generate keypair" << std::endl;
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);
    return derivePublicKey();
}

bool Auth::derivePublicKey()
{
    if (!keypair) return false;
    publicKeyBytes.resize(32);
    size_t len = publicKeyBytes.size();
    if (EVP_PKEY_get_raw_public_key(keypair, publicKeyBytes.data(), &len) != 1) {
        std::cerr << "Failed to extract public key" << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }
    if (len != 32) {
        std::cerr << "Invalid public key length: " << len << std::endl;
        return false;
    }
    return true;
}

static std::string bytesToHexStr(const std::vector<uint8_t> &bytes)
{
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size()*2);
    for (uint8_t b: bytes) {
        out.push_back(hex[b>>4]);
        out.push_back(hex[b&0xf]);
    }
    return out;
}

std::string Auth::getPublicKeyHex() const {
    return bytesToHexStr(publicKeyBytes);
}

std::string Auth::getUsername() const { return username; }

void Auth::setUsername(const std::string &u) { username = u; }

std::vector<uint8_t> Auth::signChallenge(const std::vector<uint8_t> &challenge)
{
    if (!keypair) {
        std::cerr << "No keypair to sign with" << std::endl;
        return {};
    }
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) return {};
    if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, keypair) != 1) {
        ERR_print_errors_fp(stderr);
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    size_t siglen = 0;
    if (EVP_DigestSign(mdctx, NULL, &siglen, challenge.data(), challenge.size()) != 1) {
        ERR_print_errors_fp(stderr);
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    std::vector<uint8_t> sig(siglen);
    if (EVP_DigestSign(mdctx, sig.data(), &siglen, challenge.data(), challenge.size()) != 1) {
        ERR_print_errors_fp(stderr);
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    EVP_MD_CTX_free(mdctx);
    sig.resize(siglen);
    return sig;
}

bool Auth::loadOrGenerateKeys()
{
    if (loadPrivateKey()) {
        if (derivePublicKey()) return true;
    }
    if (generateKeys()) {
        if (savePrivateKey()) return true;
    }
    return false;
}
