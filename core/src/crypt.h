#include <stddef.h>
#include <stdint.h>

struct EncryptionContext;

struct EncryptionContext *encryption_context_new(uint8_t iv[static 4]);
void encryption_context_destroy(struct EncryptionContext *context);
void encryption_context_encrypt(struct EncryptionContext *context, uint16_t length, uint8_t *data);
void encryption_context_header(struct EncryptionContext *context, uint16_t size, uint8_t *out);
const uint8_t *encryption_context_get_iv(struct EncryptionContext *context);

struct DecryptionContext;

struct DecryptionContext *decryption_context_new(uint8_t iv[static 4]);
void decryption_context_destroy(struct DecryptionContext *context);
void decryption_context_decrypt(struct DecryptionContext *context, uint16_t length, uint8_t *data);
const uint8_t *decryption_context_get_iv(struct DecryptionContext *context);

