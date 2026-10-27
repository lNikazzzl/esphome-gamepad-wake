#pragma once

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "lwip/ip_addr.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/event/event.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace dualsense_wake {

// One HCI_Connection_Request as queued from the controller task
struct Caller {
  uint8_t addr[6];  // HCI byte order
  uint32_t cod;     // Class of Device
};

class DualsenseWake : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the template entities (HARDWARE) have restored their values
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_host_mac_text(text::Text *t) { this->host_mac_text_ = t; }
  void set_pc_ip_text(text::Text *t) { this->pc_ip_text_ = t; }
  void set_enabled_switch(switch_::Switch *s) { this->enabled_switch_ = s; }
  void set_any_device_switch(switch_::Switch *s) { this->any_device_switch_ = s; }
  void set_pause_number(number::Number *n) { this->pause_number_ = n; }
  void set_guard_number(number::Number *n) { this->guard_number_ = n; }
  void set_press_event(event::Event *e) { this->press_event_ = e; }
  void set_listening_sensor(binary_sensor::BinarySensor *s) { this->listening_sensor_ = s; }
  void set_pc_online_sensor(binary_sensor::BinarySensor *s) { this->pc_online_sensor_ = s; }
  void set_last_caller_sensor(text_sensor::TextSensor *s) { this->last_caller_sensor_ = s; }
  void set_chip_address_sensor(text_sensor::TextSensor *s) { this->chip_address_sensor_ = s; }

  void add_on_press_callback(std::function<void(std::string)> &&callback) {
    this->press_callback_.add(std::move(callback));
  }

 protected:
  bool bt_start_();
  bool hci_send_(uint16_t opcode, const uint8_t *params, uint8_t len);
  void set_page_scan_(bool on);
  void handle_caller_(const Caller &caller);
  void update_pc_online_();
  void ping_(const ip_addr_t &target);
  void hold_for_(float seconds);
  bool holding_() const;

  static void on_send_available_();
  static int on_hci_packet_(uint8_t *data, uint16_t len);

  text::Text *host_mac_text_{nullptr};
  text::Text *pc_ip_text_{nullptr};
  switch_::Switch *enabled_switch_{nullptr};
  switch_::Switch *any_device_switch_{nullptr};
  number::Number *pause_number_{nullptr};
  number::Number *guard_number_{nullptr};
  event::Event *press_event_{nullptr};
  binary_sensor::BinarySensor *listening_sensor_{nullptr};
  binary_sensor::BinarySensor *pc_online_sensor_{nullptr};
  text_sensor::TextSensor *last_caller_sensor_{nullptr};
  text_sensor::TextSensor *chip_address_sensor_{nullptr};

  uint8_t host_mac_[6]{};
  bool bt_ready_{false};
  bool bt_failed_{false};
  bool reboot_pending_{false};
  bool scan_enabled_{false};
  bool pc_online_{false};
  bool pc_known_{true};  // false while an IP is set but there is no fresh ping result
  uint32_t hold_until_{0};
  bool hold_active_{false};
  uint32_t last_press_{0};
  uint32_t last_ping_sent_{0};
  uint32_t wifi_since_{0};  // when Wi-Fi came up, 0 = down
  std::string checked_ip_;
  bool ip_valid_{false};
  ip_addr_t ip_{};
  std::string checked_mac_;
  bool chip_address_published_{false};
  std::string last_caller_;
  CallbackManager<void(std::string)> press_callback_;

  static SemaphoreHandle_t send_ready_;
  static QueueHandle_t caller_queue_;
  static std::atomic<bool> addr_read_;
  static uint8_t controller_addr_[6];
  static std::atomic<uint32_t> last_ping_ok_;
  static std::atomic<bool> ping_done_;
};

class PressTrigger : public Trigger<std::string> {
 public:
  explicit PressTrigger(DualsenseWake *parent) {
    parent->add_on_press_callback([this](const std::string &address) { this->trigger(address); });
  }
};

}  // namespace dualsense_wake
}  // namespace esphome
