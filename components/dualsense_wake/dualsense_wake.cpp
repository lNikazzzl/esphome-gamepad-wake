#include "dualsense_wake.h"

#include <cmath>
#include <cstring>

#include "esp_bt.h"
#include "esp_mac.h"
#include "ping/ping_sock.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace dualsense_wake {

static const char *const TAG = "dualsense_wake";

static const uint32_t PING_PERIOD_MS = 3000;
static const uint32_t PC_OFFLINE_AFTER_MS = 10000;  // no ping reply this long -> PC is off
static const uint32_t PRESS_COOLDOWN_MS = 5000;     // one press pages several times
static const uint32_t HCI_SEND_TIMEOUT_MS = 500;

// HCI opcodes
static const uint16_t HCI_REJECT_CONNECTION_REQUEST = 0x040A;
static const uint16_t HCI_RESET = 0x0C03;
static const uint16_t HCI_WRITE_SCAN_ENABLE = 0x0C1A;
static const uint16_t HCI_WRITE_PAGE_SCAN_ACTIVITY = 0x0C1C;
static const uint16_t HCI_WRITE_PAGE_SCAN_TYPE = 0x0C47;
static const uint16_t HCI_READ_BD_ADDR = 0x1009;

SemaphoreHandle_t DualsenseWake::send_ready_ = nullptr;
QueueHandle_t DualsenseWake::caller_queue_ = nullptr;
std::atomic<bool> DualsenseWake::addr_read_{false};
uint8_t DualsenseWake::controller_addr_[6] = {};
std::atomic<uint32_t> DualsenseWake::last_ping_ok_{0};
std::atomic<bool> DualsenseWake::ping_done_{true};

// "AA:BB:CC:DD:EE:FF" (':' or '-', any case) -> 6 bytes, display order
static bool parse_mac(const std::string &s, uint8_t *out) {
  if (s.size() != 17) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    if (i > 0 && s[i * 3 - 1] != ':' && s[i * 3 - 1] != '-') {
      return false;
    }
    char hex[3] = {s[i * 3], s[i * 3 + 1], 0};
    if (!isxdigit((unsigned char) hex[0]) || !isxdigit((unsigned char) hex[1])) {
      return false;
    }
    out[i] = strtol(hex, nullptr, 16);
  }
  return true;
}

static std::string format_hci_addr(const uint8_t *hci_addr) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", hci_addr[5], hci_addr[4], hci_addr[3], hci_addr[2],
           hci_addr[1], hci_addr[0]);
  return buf;
}

// Major class Peripheral, minor type joystick or gamepad (DualSense: 0x002508)
static bool is_gamepad(uint32_t cod) {
  uint8_t major = (cod >> 8) & 0x1F;
  uint8_t minor_type = (cod >> 2) & 0x0F;
  return major == 0x05 && (minor_type == 0x01 || minor_type == 0x02);
}

void DualsenseWake::on_send_available_() { xSemaphoreGive(send_ready_); }

// Runs in the controller task: only queue, never send HCI from here.
int DualsenseWake::on_hci_packet_(uint8_t *data, uint16_t len) {
  if (len < 3 || data[0] != 0x04) {
    return 0;
  }
  // HCI_Connection_Request: bdaddr[6], class[3], link_type[1]
  if (data[1] == 0x04 && len >= 13) {
    Caller caller;
    memcpy(caller.addr, data + 3, 6);
    caller.cod = data[9] | (data[10] << 8) | (data[11] << 16);
    xQueueSend(caller_queue_, &caller, 0);
  }
  // HCI_Command_Complete for HCI_Read_BD_ADDR: ncmd, opcode[2], status, bdaddr[6]
  if (data[1] == 0x0E && len >= 13 && data[4] == (HCI_READ_BD_ADDR & 0xFF) && data[5] == (HCI_READ_BD_ADDR >> 8) &&
      data[6] == 0x00) {
    memcpy(controller_addr_, data + 7, 6);
    addr_read_.store(true, std::memory_order_release);
  }
  return 0;
}

bool DualsenseWake::hci_send_(uint16_t opcode, const uint8_t *params, uint8_t len) {
  uint8_t buf[4 + 255];
  buf[0] = 0x01;  // HCI command packet
  buf[1] = opcode & 0xFF;
  buf[2] = opcode >> 8;
  buf[3] = len;
  if (len > 0) {
    memcpy(buf + 4, params, len);
  }
  uint32_t start = millis();
  while (!esp_vhci_host_check_send_available()) {
    if (millis() - start > HCI_SEND_TIMEOUT_MS) {
      ESP_LOGE(TAG, "HCI command 0x%04X dropped: controller busy", opcode);
      return false;
    }
    xSemaphoreTake(send_ready_, pdMS_TO_TICKS(20));
  }
  esp_vhci_host_send_packet(buf, 4 + len);
  return true;
}

void DualsenseWake::set_page_scan_(bool on) {
  uint8_t mode = on ? 0x02 : 0x00;  // 0x02 = page scan only, never discoverable
  if (!this->hci_send_(HCI_WRITE_SCAN_ENABLE, &mode, 1)) {
    return;  // retried on the next loop
  }
  this->scan_enabled_ = on;
  this->listening_sensor_->publish_state(on);
  ESP_LOGI(TAG, "Page scan %s", on ? "ON" : "OFF");
}

bool DualsenseWake::bt_start_() {
  if (esp_iface_mac_addr_set(this->host_mac_, ESP_MAC_BT) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set BT address");
    return false;
  }
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&cfg) != ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth controller init failed");
    return false;
  }
  static const esp_vhci_host_callback_t callbacks = {on_send_available_, on_hci_packet_};
  esp_vhci_host_register_callback(&callbacks);

  // One command at a time: the controller accepts a single outstanding command
  this->hci_send_(HCI_RESET, nullptr, 0);
  delay(100);
  uint8_t interlaced = 0x01;
  this->hci_send_(HCI_WRITE_PAGE_SCAN_TYPE, &interlaced, 1);
  delay(10);
  // interval 0x0400 (640 ms), window 0x0024 (22.5 ms), in 0.625 ms slots
  uint8_t activity[4] = {0x00, 0x04, 0x24, 0x00};
  this->hci_send_(HCI_WRITE_PAGE_SCAN_ACTIVITY, activity, 4);
  delay(10);
  this->hci_send_(HCI_READ_BD_ADDR, nullptr, 0);
  delay(10);
  this->set_page_scan_(false);
  return true;
}

void DualsenseWake::setup() {
  send_ready_ = xSemaphoreCreateBinary();
  caller_queue_ = xQueueCreate(8, sizeof(Caller));
  this->listening_sensor_->publish_state(false);
  this->pc_online_sensor_->publish_state(false);

  this->checked_mac_ = this->host_mac_text_->state;
  if (!parse_mac(this->checked_mac_, this->host_mac_)) {
    ESP_LOGW(TAG, "PC Bluetooth MAC is not set: enter it in Home Assistant (format AA:BB:CC:DD:EE:FF)");
    this->chip_address_sensor_->publish_state(this->checked_mac_.empty() ? "PC MAC not set" : "Invalid PC MAC");
    return;
  }
  if (!this->bt_start_()) {
    this->bt_failed_ = true;
    this->status_set_error("Bluetooth init failed");
    this->chip_address_sensor_->publish_state("Bluetooth init failed");
    return;
  }
  this->bt_ready_ = true;
}

void DualsenseWake::dump_config() {
  ESP_LOGCONFIG(TAG, "DualSense Wake:");
  if (!this->bt_ready_) {
    ESP_LOGCONFIG(TAG, "  Bluetooth not started (%s)", this->bt_failed_ ? "init failed" : "PC MAC not set or invalid");
    return;
  }
  ESP_LOGCONFIG(TAG, "  PC address: %02X:%02X:%02X:%02X:%02X:%02X", this->host_mac_[0], this->host_mac_[1],
                this->host_mac_[2], this->host_mac_[3], this->host_mac_[4], this->host_mac_[5]);
  if (addr_read_.load(std::memory_order_acquire)) {
    std::string actual = format_hci_addr(controller_addr_);
    ESP_LOGCONFIG(TAG, "  Controller address: %s", actual.c_str());
  } else {
    ESP_LOGW(TAG, "  Controller address not read");
  }
}

// Holds only ever extend, so the guard window can't cut a running pause short
void DualsenseWake::hold_for_(float seconds) {
  if (std::isnan(seconds) || seconds <= 0) {
    return;
  }
  uint32_t until = millis() + (uint32_t) (seconds * 1000);
  if (!this->hold_active_ || (int32_t) (until - this->hold_until_) > 0) {
    this->hold_until_ = until;
  }
  this->hold_active_ = true;
}

bool DualsenseWake::holding_() const {
  return this->hold_active_ && (int32_t) (millis() - this->hold_until_) < 0;
}

void DualsenseWake::loop() {
  // A new PC MAC takes effect after a reboot: the BT address is fixed at controller init
  const std::string &mac_text = this->host_mac_text_->state;
  if (mac_text != this->checked_mac_) {
    this->checked_mac_ = mac_text;
    uint8_t mac[6];
    if (!parse_mac(mac_text, mac)) {
      ESP_LOGW(TAG, "Invalid PC MAC '%s', keeping the current one", mac_text.c_str());
      this->chip_address_sensor_->publish_state("Invalid PC MAC");
      this->chip_address_published_ = false;
    } else if (!this->reboot_pending_ && (!this->bt_ready_ || memcmp(mac, this->host_mac_, 6) != 0)) {
      ESP_LOGI(TAG, "PC MAC changed, rebooting to apply it");
      this->reboot_pending_ = true;
      this->set_timeout(1000, []() { App.safe_reboot(); });
    }
  }
  if (!this->bt_ready_) {
    return;
  }

  if (!this->chip_address_published_ && addr_read_.load(std::memory_order_acquire)) {
    this->chip_address_sensor_->publish_state(format_hci_addr(controller_addr_));
    this->chip_address_published_ = true;
  }

  this->update_pc_online_();
  if (this->hold_active_ && !this->holding_()) {
    this->hold_active_ = false;
  }

  bool listen = this->enabled_switch_->state && this->pc_known_ && !this->pc_online_ && !this->holding_();
  if (listen != this->scan_enabled_) {
    this->set_page_scan_(listen);
  }

  Caller caller;
  while (xQueueReceive(caller_queue_, &caller, 0) == pdTRUE) {
    this->handle_caller_(caller);
  }
}

void DualsenseWake::handle_caller_(const Caller &caller) {
  uint8_t params[7];
  memcpy(params, caller.addr, 6);
  params[6] = 0x0F;  // Connection Rejected due to Unacceptable BD_ADDR
  this->hci_send_(HCI_REJECT_CONNECTION_REQUEST, params, sizeof(params));

  std::string address = format_hci_addr(caller.addr);
  char label[40];
  snprintf(label, sizeof(label), "%s (CoD 0x%06X)", address.c_str(), (unsigned) caller.cod);
  if (this->last_caller_ != label) {
    this->last_caller_ = label;
    this->last_caller_sensor_->publish_state(label);
  }

  // Queued just before page scan went off: reject, but stay silent as promised
  if (!this->scan_enabled_) {
    ESP_LOGD(TAG, "Page from %s while not listening, ignored", label);
    return;
  }
  if (!is_gamepad(caller.cod) && !this->any_device_switch_->state) {
    ESP_LOGI(TAG, "Page from %s, not a gamepad, ignored", label);
    return;
  }
  uint32_t now = millis();
  if (this->last_press_ != 0 && now - this->last_press_ < PRESS_COOLDOWN_MS) {
    ESP_LOGD(TAG, "Page from %s (repeat, ignored)", label);
    return;
  }
  this->last_press_ = now;
  ESP_LOGI(TAG, "PS press from %s", label);
  this->press_event_->trigger("press");
  this->press_callback_.call(address);
  this->hold_for_(this->pause_number_->state);
}

void DualsenseWake::update_pc_online_() {
  const std::string &ip = this->pc_ip_text_->state;
  if (ip != this->checked_ip_) {
    this->checked_ip_ = ip;
    this->ip_valid_ = !ip.empty() && ipaddr_aton(ip.c_str(), &this->ip_);
    if (!ip.empty() && !this->ip_valid_) {
      ESP_LOGW(TAG, "Invalid PC IP '%s', ignoring it", ip.c_str());
    }
  }

  bool online = false;
  bool known = true;
  if (this->ip_valid_) {
    uint32_t now = millis();
    bool wifi_up = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
    if (!wifi_up) {
      // Can't ping: the PC may well be on, so don't risk answering in its place
      this->wifi_since_ = 0;
      known = false;
    } else {
      if (this->wifi_since_ == 0) {
        this->wifi_since_ = now | 1;
      }
      if (now - this->last_ping_sent_ >= PING_PERIOD_MS) {
        this->last_ping_sent_ = now;
        this->ping_(this->ip_);
      }
      uint32_t last_ok = last_ping_ok_.load();
      online = last_ok != 0 && now - last_ok < PC_OFFLINE_AFTER_MS;
      // Offline only after a full timeout of pings since Wi-Fi came up
      known = online || now - this->wifi_since_ >= PC_OFFLINE_AFTER_MS;
    }
  }
  this->pc_known_ = known;
  if (!known || online == this->pc_online_) {
    return;
  }
  if (this->pc_online_ && !online) {
    // The gamepad keeps reconnecting for a while after the PC shuts down: don't treat that as a press
    this->hold_for_(this->guard_number_->state);
  }
  this->pc_online_ = online;
  this->pc_online_sensor_->publish_state(online);
  ESP_LOGI(TAG, "PC %s", online ? "online" : "offline");
}

void DualsenseWake::ping_(const ip_addr_t &target) {
  if (!ping_done_.load()) {
    return;
  }
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = target;
  cfg.count = 1;
  cfg.timeout_ms = 1000;
  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = [](esp_ping_handle_t, void *) { last_ping_ok_.store(millis()); };
  cbs.on_ping_end = [](esp_ping_handle_t hdl, void *) {
    esp_ping_delete_session(hdl);
    ping_done_.store(true);
  };
  esp_ping_handle_t hdl;
  if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
    return;
  }
  ping_done_.store(false);
  if (esp_ping_start(hdl) != ESP_OK) {
    esp_ping_delete_session(hdl);
    ping_done_.store(true);
  }
}

}  // namespace dualsense_wake
}  // namespace esphome
