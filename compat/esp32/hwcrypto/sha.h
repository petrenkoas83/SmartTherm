/* Compatibility header for esp32_https_server: вместо hwcrypto/sha.h используем mbedtls SHA-1.
 * SHA в библиотеке нужен только для WebSocket handshake (Sec-WebSocket-Accept).
 * mbedtls уже есть в прошивке для TLS — дублирования кода нет, зависимость от hal/sha убрана.
 */
#pragma once

#include <mbedtls/sha1.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SHA1
#define SHA1 0
#endif

/* Обёртка для esp_sha(SHA1, input, len, output): один вызов хеша. */
static inline void esp_sha(int sha_type, const unsigned char *input, size_t ilen, unsigned char *output) {
  (void)sha_type;
  mbedtls_sha1_ret(input, ilen, output);
}

#ifdef __cplusplus
}
#endif
