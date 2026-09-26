#ifndef XECRYPT_RC4_H_
#define XECRYPT_RC4_H_

#include <cstdint>

namespace xecrypt {
namespace rc4 {

// RC4 key schedule over all key_size bytes of the key, as XeCryptRc4Key does.
// An empty key schedules as a zero key.
static inline void KeySchedule(uint8_t (&s)[256], uint8_t& i, uint8_t& j,
                               const uint8_t* key, uint32_t key_size) {
  i = j = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    s[x] = static_cast<uint8_t>(x);
  }
  uint8_t idx = 0;
  for (uint32_t x = 0; x < 0x100; x++) {
    idx += s[x] + (key_size ? key[x % key_size] : 0);
    uint8_t temp = s[idx];
    s[idx] = s[x];
    s[x] = temp;
  }
}

// XORs data in place with the next size bytes of the key stream.
static inline void Crypt(uint8_t (&s)[256], uint8_t& i, uint8_t& j,
                         uint8_t* data, uint32_t size) {
  for (uint32_t idx = 0; idx < size; idx++) {
    i++;
    j += s[i];
    uint8_t temp = s[i];
    s[i] = s[j];
    s[j] = temp;
    data[idx] ^= s[static_cast<uint8_t>(s[i] + s[j])];
  }
}

}  // namespace rc4
}  // namespace xecrypt

#endif  // XECRYPT_RC4_H_
