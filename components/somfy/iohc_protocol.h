#pragma once

// io-homecontrol protocol core — pure, dependency-free helpers.
//
// This file intentionally pulls in nothing from ESPHome or mbedTLS so the
// framing/CRC/checksum/IV/MAC logic can be unit-tested on a host machine
// against captured golden vectors (see tests/cpp/test_iohc_protocol.cpp).
// AES is injected by the caller: firmware supplies the mbedTLS wrapper, the
// host test supplies a known-correct AES-128 oracle.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace somfy {
namespace iohc_proto {

// AES-128 ECB single-block encrypt: out = AES_enc(key, in).
using Aes128EcbFn = void (*)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

// CRC-16 used by io-homecontrol (poly 0x8408, init 0x0000, reflected, LSB-first
// on the wire). Computed over [data, data+len). Append as two bytes,
// low byte first; a frame including its own CRC then checksums to 0x0000.
uint16_t crc16(const uint8_t *data, size_t len);

// io-homecontrol rolling checksum over [data, data+len). Returns the two
// accumulator bytes (chk1 = high, chk2 = low) that feed the MAC IV.
void rolling_checksum(const uint8_t *data, size_t len, uint8_t &chk1, uint8_t &chk2);

// Build the 16-byte AES IV for a 1W MAC.
//   payload = cmd || data (the command payload, excluding node addresses)
//   iv[0..7]   = first 8 payload bytes, 0x55-padded if payload shorter than 8
//   iv[8..9]   = rolling checksum over the full payload
//   iv[10..11] = sequence number (big-endian)
//   iv[12..15] = 0x55 padding
void build_iv_1w(const uint8_t *payload, size_t payload_len, uint16_t sequence, uint8_t iv[16]);

// Build the 16-byte AES IV for a 2W challenge response.
//   payload = cmd || data (the command payload, excluding node addresses)
//   iv[0..7]   = first 8 payload bytes, 0x55-padded if payload shorter than 8
//   iv[8..9]   = rolling checksum over the full payload
//   iv[10..15] = 6-byte challenge supplied by the actuator
void build_iv_2w(const uint8_t *payload, size_t payload_len, const uint8_t challenge[6], uint8_t iv[16]);

// MAC = AES(key, iv) truncated to the leading 6 bytes.
void compute_mac(Aes128EcbFn aes, const uint8_t key[16], const uint8_t iv[16], uint8_t mac_out[6]);

// 1W controller-key obfuscation for the 0x30 key-transfer frame.
//   keystream = AES(transfer_key, IV) where IV is the 3-byte node address
//   (big-endian) repeated to fill all 16 bytes.
//   enc_out[i] = plain_key[i] XOR keystream[i].
void obfuscate_key_1w(Aes128EcbFn aes, const uint8_t transfer_key[16], uint32_t node_addr,
                      const uint8_t plain_key[16], uint8_t enc_out[16]);

// --- Physical layer (UART-8N1) codec -------------------------------------
//
// io-homecontrol modulates 2-FSK at 38400 baud, but the bit stream is *not*
// raw NRZ: every logical byte is wrapped in a UART 8N1 frame — a 0 start bit,
// 8 data bits transmitted LSB-first, then a 1 stop bit — and bytes are sent in
// order. The on-air message is:
//
//   preamble (256 bits of 0101…, i.e. UART-framed 0x55 bytes)
//     + sync   (UART-framed 0xFF 0x33  ->  bits 0111111111 0110011001)
//     + payload (each logical frame byte UART-framed)
//
// The CC1101 hardware can generate the alternating preamble and match a 16-bit
// sync word, but it cannot UART-frame the payload. We therefore co-opt the
// hardware: program the 16-bit sync word to the FIRST 16 bits of the encoded
// sync (0x7FD9), and let software handle everything after it. The 4 remaining
// encoded-sync bits (0b1001, the tail of the UART-framed 0x33) sit at the head
// of the FIFO payload, immediately before the first logical byte's UART frame.
//
// build_iv_1w / crc16 / etc. operate on the *logical* bytes (start/stop bits
// stripped, bit order restored) — exactly what the documented captures show.

// 16-bit hardware sync word: the first 16 bits of the UART-encoded 0xFF 0x33
// sync sequence. Program SYNC1=0x7F, SYNC0=0xD9, sync_mode 16/16 at runtime.
static constexpr uint8_t PHY_HW_SYNC1 = 0x7F;
static constexpr uint8_t PHY_HW_SYNC0 = 0xD9;

// The 4 encoded-sync bits (0b1001) that follow the 16-bit hardware sync match
// and precede the first logical byte inside the FIFO payload.
static constexpr uint8_t PHY_SYNC_TAIL_BITS = 4;

// Encode a logical frame (ctrl0 .. crc) into the CC1101 FIFO payload that
// follows the 16-bit hardware sync word. The payload is the 4-bit encoded-sync
// tail (0b1001) followed by each logical byte UART-framed (start 0, 8 data bits
// LSB-first, stop 1), bit-packed MSB-first. Any partial trailing byte is padded
// with idle (1) bits. The CC1101 must be in fixed-length packet mode with the
// length set to out.size() and hardware CRC disabled.
void uart_encode(const uint8_t *logical, size_t len, std::vector<uint8_t> &out);

// Decode a CC1101 FIFO payload (captured after a 0x7FD9 sync match) back into
// logical frame bytes. Skips the 4-bit encoded-sync tail, then UART-decodes
// each following 10-bit group. Stops at the first framing error (bad start/stop
// bit) or when fewer than 10 bits remain. Returns the number of bytes decoded.
size_t uart_decode(const uint8_t *payload, size_t len, std::vector<uint8_t> &out);

}  // namespace iohc_proto
}  // namespace somfy
}  // namespace esphome
