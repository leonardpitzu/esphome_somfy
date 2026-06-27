#include "somfy_hub_iohc.h"

#ifdef USE_SOMFY_IOHC

#include "esphome/core/log.h"
#include <cstring>
#include <mbedtls/aes.h>

namespace esphome {
namespace somfy {

static const char *TAG = "somfy.iohc.hub";

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

uint16_t crc16_kermit(const uint8_t *data, size_t len) {
  uint16_t crc = iohc::CRC_INIT;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x0001)
        crc = (crc >> 1) ^ iohc::CRC_POLY;
      else
        crc >>= 1;
    }
  }
  return crc;
}

void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t plaintext[16], uint8_t ciphertext[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, plaintext, ciphertext);
  mbedtls_aes_free(&ctx);
}

void compute_2w_checksum(const uint8_t *data, size_t len, uint8_t &chk1, uint8_t &chk2) {
  chk1 = 0;
  chk2 = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t tmp = data[i] ^ chk2;
    chk2 = ((chk1 & 0x7F) << 1) & 0xFF;
    if (tmp >= 0x80)
      chk2 |= 1;
    if ((chk1 & 0x80) == 0) {
      chk1 = chk2;
      chk2 = (tmp << 1) & 0xFF;
    } else {
      chk1 = chk2 ^ 0x55;
      chk2 = ((tmp << 1) ^ 0x5B) & 0xFF;
    }
  }
}

void compute_2w_response(const uint8_t key[16], const uint8_t *frame_data, size_t frame_len,
                         const uint8_t challenge[6], uint8_t response[6]) {
  // Build initial value (IV) for AES:
  // [0..7]  = first 8 bytes of frame_data (padded with 0x55 if shorter)
  // [8..9]  = rolling checksum over all frame_data bytes
  // [10..15] = 6-byte challenge
  uint8_t iv[16];
  memset(iv, 0x55, 16);

  size_t copy_len = (frame_len > 8) ? 8 : frame_len;
  memcpy(iv, frame_data, copy_len);
  if (frame_len < 8) {
    for (size_t j = frame_len; j < 8; j++)
      iv[j] = 0x55;
  }

  // Compute rolling checksum over full frame_data
  compute_2w_checksum(frame_data, frame_len, iv[8], iv[9]);

  // Copy challenge into iv[10..15]
  memcpy(iv + 10, challenge, 6);

  // AES-128-ECB encrypt IV with the system/device key
  uint8_t encrypted[16];
  aes128_ecb_encrypt(key, iv, encrypted);

  // First 6 bytes of ciphertext = response
  memcpy(response, encrypted, 6);
}

// ---------------------------------------------------------------------------
// Setup / Loop / Dump
// ---------------------------------------------------------------------------

void SomfyIohcHub::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Somfy iohc hub...");
  this->configure_radio_1w();
  this->cc1101_->register_listener(this);
  // Enter RX immediately so we can hear physical io-homecontrol remotes (and 2W
  // feedback) from boot — the CC1101 does not auto-listen, and otherwise RX
  // would only start after the first HA-initiated TX.
  this->cc1101_->begin_rx();
}

void SomfyIohcHub::loop() {
  // 2W frequency hopping
  if (this->listening_2w_) {
    const uint32_t now_us = micros();
    if ((now_us - this->last_hop_us_) >= iohc::CHANNEL_DWELL_US) {
      this->current_2w_channel_ = (this->current_2w_channel_ + 1) % 3;
      this->configure_radio_2w(this->current_2w_channel_);
      this->cc1101_->begin_rx();  // re-enter RX after frequency change
      this->last_hop_us_ = now_us;
    }
  }

  // 2W session state machine
  this->session_loop_();
}

void SomfyIohcHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Somfy iohc Hub:");
  ESP_LOGCONFIG(TAG, "  CC1101: %s", this->cc1101_ != nullptr ? "configured" : "MISSING");
  ESP_LOGCONFIG(TAG, "  RX callbacks: %u", this->rx_callbacks_.size());
}

// ---------------------------------------------------------------------------
// TX (1W)
// ---------------------------------------------------------------------------

void SomfyIohcHub::transmit_packet(const std::vector<uint8_t> &frame, uint8_t repeat_count) {
  this->configure_radio_1w();

  for (int i = 0; i < repeat_count; i++) {
    auto err = this->cc1101_->transmit_packet(frame);
    if (err != cc1101::CC1101Error::NONE) {
      ESP_LOGW(TAG, "TX error on repeat %d: %d", i, static_cast<int>(err));
      break;
    }
  }

  this->cc1101_->begin_rx();
  ESP_LOGD(TAG, "TX 1W: %u bytes, %d repeats", frame.size(), repeat_count);
}

void SomfyIohcHub::begin_rx() {
  this->cc1101_->begin_rx();
}

// ---------------------------------------------------------------------------
// Radio configuration
// ---------------------------------------------------------------------------

void SomfyIohcHub::configure_radio_1w() {
  this->cc1101_->set_frequency(iohc::FREQUENCY_1W);
  this->cc1101_->set_modulation_type(cc1101::Modulation::MODULATION_2_FSK);
  this->cc1101_->set_symbol_rate(iohc::SYMBOL_RATE);
  this->cc1101_->set_fsk_deviation(iohc::FSK_DEVIATION);
  this->cc1101_->set_filter_bandwidth(iohc::FILTER_BW);
  this->cc1101_->set_manchester(false);
  this->cc1101_->set_sync1(iohc::SYNC1);
  this->cc1101_->set_sync0(iohc::SYNC0);
  this->listening_2w_ = false;
}

void SomfyIohcHub::configure_radio_2w(uint8_t channel) {
  if (channel >= 3) channel = 0;
  this->cc1101_->set_frequency(iohc::FREQUENCY_2W[channel]);
}

void SomfyIohcHub::start_2w_listen() {
  this->current_2w_channel_ = 0;
  this->configure_radio_2w(0);
  this->cc1101_->begin_rx();
  this->listening_2w_ = true;
  this->last_hop_us_ = micros();
}

void SomfyIohcHub::stop_2w_listen() {
  this->listening_2w_ = false;
  this->configure_radio_1w();
  // Resume 1W RX so passive state-sync keeps working after the 2W session.
  this->cc1101_->begin_rx();
}

// ---------------------------------------------------------------------------
// 2W TX: send command with challenge/response auth
// ---------------------------------------------------------------------------

void SomfyIohcHub::send_2w_command(uint32_t src_node, uint32_t dest_node, uint8_t cmd,
                                    const uint8_t *data, size_t data_len,
                                    const uint8_t key[16], Session2WCallback callback) {
  if (this->session_.state != Session2WState::IDLE &&
      this->session_.state != Session2WState::COMPLETE &&
      this->session_.state != Session2WState::FAILED) {
    ESP_LOGW(TAG, "2W session busy, cannot start new command");
    if (callback) callback(false, nullptr);
    return;
  }

  // Initialize session
  this->session_.state = Session2WState::CMD_SENT;
  this->session_.src_node = src_node;
  this->session_.dest_node = dest_node;
  this->session_.cmd = cmd;
  this->session_.cmd_data.assign(data, data + data_len);
  memcpy(this->session_.key, key, 16);
  this->session_.started_ms = millis();
  this->session_.retries = 0;
  this->session_.callback = std::move(callback);

  // Build and store the frame payload (dest+src+cmd+data) for IV computation
  this->session_.frame_payload.clear();
  this->session_.frame_payload.reserve(7 + data_len);
  this->session_.frame_payload.push_back(static_cast<uint8_t>((dest_node >> 16) & 0xFF));
  this->session_.frame_payload.push_back(static_cast<uint8_t>((dest_node >> 8) & 0xFF));
  this->session_.frame_payload.push_back(static_cast<uint8_t>(dest_node & 0xFF));
  this->session_.frame_payload.push_back(static_cast<uint8_t>((src_node >> 16) & 0xFF));
  this->session_.frame_payload.push_back(static_cast<uint8_t>((src_node >> 8) & 0xFF));
  this->session_.frame_payload.push_back(static_cast<uint8_t>(src_node & 0xFF));
  this->session_.frame_payload.push_back(cmd);
  this->session_.frame_payload.insert(this->session_.frame_payload.end(), data, data + data_len);

  // Switch to 2W mode and send the command frame
  this->start_2w_listen();
  this->send_2w_frame_(src_node, dest_node, cmd, data, data_len);

  ESP_LOGD(TAG, "2W session started: src=0x%06X dst=0x%06X cmd=0x%02X", src_node, dest_node, cmd);
}

// ---------------------------------------------------------------------------
// 2W session loop (timeout management)
// ---------------------------------------------------------------------------

void SomfyIohcHub::session_loop_() {
  if (this->session_.state == Session2WState::IDLE ||
      this->session_.state == Session2WState::COMPLETE ||
      this->session_.state == Session2WState::FAILED)
    return;

  uint32_t elapsed = millis() - this->session_.started_ms;
  if (elapsed > iohc::SESSION_TIMEOUT_MS) {
    if (this->session_.retries < iohc::SESSION_MAX_RETRIES) {
      this->session_.retries++;
      this->session_.started_ms = millis();
      this->session_.state = Session2WState::CMD_SENT;
      ESP_LOGW(TAG, "2W session timeout, retry %d", this->session_.retries);
      this->send_2w_frame_(this->session_.src_node, this->session_.dest_node,
                           this->session_.cmd, this->session_.cmd_data.data(),
                           this->session_.cmd_data.size());
    } else {
      ESP_LOGW(TAG, "2W session failed after %d retries", this->session_.retries);
      this->session_.state = Session2WState::FAILED;
      this->stop_2w_listen();
      if (this->session_.callback) this->session_.callback(false, nullptr);
    }
  }
}

// ---------------------------------------------------------------------------
// 2W frame building and sending
// ---------------------------------------------------------------------------

std::vector<uint8_t> SomfyIohcHub::build_2w_frame_(uint32_t src, uint32_t dest, uint8_t cmd,
                                                     const uint8_t *data, size_t data_len) {
  std::vector<uint8_t> frame;
  frame.reserve(9 + data_len + 2);  // header + data + CRC

  // CtrlByte0: 2W mode (isOneWay=0), order=1, size filled later
  uint8_t ctrl0 = 0x00;
  // Size field = total bytes after ctrl0+ctrl1 minus 1 (protocol convention)
  uint8_t size_field = static_cast<uint8_t>(6 + data_len);  // dest(3)+src(3)+cmd(1)+data(n) - 1
  ctrl0 |= (size_field & 0x1F);
  ctrl0 |= (1 << 5);  // order bit (S=1 for controller-originated)
  frame.push_back(ctrl0);

  // CtrlByte1: StartFrame=1, EndFrame=1, Protocol=0
  frame.push_back(iohc::CTRL1_START_END);

  // Destination (3 bytes big-endian)
  frame.push_back(static_cast<uint8_t>((dest >> 16) & 0xFF));
  frame.push_back(static_cast<uint8_t>((dest >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(dest & 0xFF));

  // Source (3 bytes big-endian)
  frame.push_back(static_cast<uint8_t>((src >> 16) & 0xFF));
  frame.push_back(static_cast<uint8_t>((src >> 8) & 0xFF));
  frame.push_back(static_cast<uint8_t>(src & 0xFF));

  // Command
  frame.push_back(cmd);

  // Data
  for (size_t i = 0; i < data_len; i++) {
    frame.push_back(data[i]);
  }

  // CRC-16-KERMIT
  uint16_t crc = crc16_kermit(frame.data(), frame.size());
  frame.push_back(static_cast<uint8_t>(crc & 0xFF));
  frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

  return frame;
}

void SomfyIohcHub::send_2w_frame_(uint32_t src, uint32_t dest, uint8_t cmd,
                                   const uint8_t *data, size_t data_len) {
  auto frame = this->build_2w_frame_(src, dest, cmd, data, data_len);
  // 2W frames are sent once on the current channel (868.95 MHz = ch1)
  this->configure_radio_2w(1);
  auto err = this->cc1101_->transmit_packet(frame);
  if (err != cc1101::CC1101Error::NONE) {
    ESP_LOGW(TAG, "2W TX error: %d", static_cast<int>(err));
  }
  this->cc1101_->begin_rx();
  ESP_LOGD(TAG, "TX 2W: cmd=0x%02X %u bytes", cmd, frame.size());
}

// ---------------------------------------------------------------------------
// RX callback
// ---------------------------------------------------------------------------

void SomfyIohcHub::on_packet(const std::vector<uint8_t> &packet, float freq_offset,
                              float rssi, uint8_t lqi) {
  if (packet.size() < 11) return;  // minimum frame size

  // Verify CRC
  uint16_t received_crc = static_cast<uint16_t>(packet[packet.size() - 2]) |
                          (static_cast<uint16_t>(packet[packet.size() - 1]) << 8);
  uint16_t calc_crc = crc16_kermit(packet.data(), packet.size() - 2);
  if (received_crc != calc_crc) {
    ESP_LOGV(TAG, "RX: CRC mismatch (got 0x%04X, calc 0x%04X)", received_crc, calc_crc);
    return;
  }

  // Parse header
  IohcDecodedPacket pkt;
  pkt.dest_node = (static_cast<uint32_t>(packet[2]) << 16) |
                  (static_cast<uint32_t>(packet[3]) << 8) |
                  static_cast<uint32_t>(packet[4]);
  pkt.src_node = (static_cast<uint32_t>(packet[5]) << 16) |
                 (static_cast<uint32_t>(packet[6]) << 8) |
                 static_cast<uint32_t>(packet[7]);
  pkt.cmd = packet[8];
  pkt.data = (packet.size() > 11) ? &packet[9] : nullptr;
  pkt.data_len = (packet.size() > 11) ? packet.size() - 11 : 0;  // 9 header + 2 CRC
  pkt.rssi = rssi;
  pkt.lqi = lqi;

  ESP_LOGD(TAG, "RX: src=0x%06X dst=0x%06X cmd=0x%02X rssi=%.1f len=%u",
           pkt.src_node, pkt.dest_node, pkt.cmd, rssi, packet.size());

  // Handle 2W session packets
  this->handle_2w_packet_(pkt);

  // Dispatch to all registered callbacks
  for (auto &cb : this->rx_callbacks_) {
    cb(pkt);
  }
}

// ---------------------------------------------------------------------------
// 2W session packet handler (challenge/response state machine)
// ---------------------------------------------------------------------------

void SomfyIohcHub::handle_2w_packet_(const IohcDecodedPacket &pkt) {
  // Only process if we have an active session and packet is from our target
  if (this->session_.state == Session2WState::IDLE ||
      this->session_.state == Session2WState::COMPLETE ||
      this->session_.state == Session2WState::FAILED)
    return;

  if (pkt.src_node != this->session_.dest_node)
    return;
  if (pkt.dest_node != this->session_.src_node && pkt.dest_node != iohc::BROADCAST_ADDR)
    return;

  switch (this->session_.state) {
    case Session2WState::CMD_SENT:
      // Expecting 0x3C (Challenge Request) from actuator
      if (pkt.cmd == iohc::CMD_CHALLENGE_REQUEST && pkt.data_len >= 6) {
        memcpy(this->session_.challenge, pkt.data, 6);
        ESP_LOGD(TAG, "2W: Challenge received from 0x%06X", pkt.src_node);

        // Compute response from the original frame payload (dest+src+cmd+data)
        uint8_t response[6];
        compute_2w_response(this->session_.key, this->session_.frame_payload.data(),
                           this->session_.frame_payload.size(), this->session_.challenge, response);

        // Send 0x3D (Challenge Response)
        this->session_.state = Session2WState::WAIT_STATUS;
        this->send_2w_frame_(this->session_.src_node, this->session_.dest_node,
                            iohc::CMD_CHALLENGE_RESPONSE, response, 6);
        ESP_LOGD(TAG, "2W: Challenge response sent");
      }
      break;

    case Session2WState::WAIT_STATUS:
      // Expecting status response (0xFE) or private ACK (0x21)
      if (pkt.cmd == iohc::CMD_STATUS || pkt.cmd == iohc::CMD_PRIVATE_ACK) {
        ESP_LOGD(TAG, "2W: Session complete, got cmd=0x%02X", pkt.cmd);
        this->session_.state = Session2WState::COMPLETE;
        this->stop_2w_listen();
        if (this->session_.callback) this->session_.callback(true, &pkt);
      }
      break;

    default:
      break;
  }
}

}  // namespace somfy
}  // namespace esphome

#endif  // USE_SOMFY_IOHC
