#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "pio_usb.h"
#include "tusb.h"

const uint8_t colemakFixMap[128] = {
  0, 0, 0, 0, HID_KEY_A, HID_KEY_T, HID_KEY_X,
  HID_KEY_C, HID_KEY_K, HID_KEY_E, HID_KEY_G, 
  HID_KEY_N, HID_KEY_L, HID_KEY_Y, HID_KEY_B, 
  HID_KEY_U, HID_KEY_H, HID_KEY_J, HID_KEY_SEMICOLON, 
  HID_KEY_R, HID_KEY_Q, HID_KEY_S, HID_KEY_D, 
  HID_KEY_F, HID_KEY_I, HID_KEY_V, HID_KEY_W, 
  HID_KEY_Z, HID_KEY_O, HID_KEY_BACKSLASH, 
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 
  HID_KEY_P, 0, 0, HID_KEY_M, HID_KEY_COMMA, 
  HID_KEY_PERIOD, 0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0
};

enum modes {
  SARCASM,
  COLEMAK_FIX
};

static struct keybind {
  uint8_t modifiers[2];
  uint8_t keycode;
};

typedef struct keybind keybind_t;

struct keybind SARCASM_KEYBIND = {
  {KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI, KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL},
  HID_KEY_X
};

struct keybind COLEMAK_FIX_KEYBIND = {
  {KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI, KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL},
  HID_KEY_F
};

bool enabled_modes[2] = { false, false };

// core1 - host events
void core1_main() {
  sleep_ms(10);

  // Use tuh_configure() to pass pio configuration to the host stack
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
  tuh_init(1);

  while (true) {
    tuh_task(); // tinyusb host task
  }
}

// core0 - device events
int main(void) {
  set_sys_clock_khz(120000, true);
  sleep_ms(10);

  multicore_reset_core1();
  multicore_launch_core1(core1_main);

  tud_init(0);

  while (true) {
    tud_task(); // tinyusb device task
    tud_cdc_write_flush();
  }

  return 0;
}

// CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
  (void) itf;

  char buf[64];
  uint32_t count = tud_cdc_read(buf, sizeof(buf));

  (void) count;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  (void)desc_report;
  (void)desc_len;

  // Interface protocol (hid_interface_protocol_enum_t)
  const char* protocol_str[] = { "None", "Keyboard", "Mouse" };
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  char tempbuf[256];
  int count = sprintf(tempbuf, "[%04x:%04x][%u] HID Interface%u, Protocol = %s\r\n", vid, pid, dev_addr, instance, protocol_str[itf_protocol]);

  tud_cdc_write(tempbuf, count);
  tud_cdc_write_flush();

  // Receive report from boot keyboard & mouse only
  // tuh_hid_report_received_cb() will be invoked when report is available
  if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD || itf_protocol == HID_ITF_PROTOCOL_MOUSE)
  {
    if ( !tuh_hid_receive_report(dev_addr, instance) )
    {
      tud_cdc_write_str("Error: cannot request report\r\n");
    }
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  char tempbuf[256];
  int count = sprintf(tempbuf, "[%u] HID Interface%u is unmounted\r\n", dev_addr, instance);
  tud_cdc_write(tempbuf, count);
  tud_cdc_write_flush();
}

static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
  for(uint8_t i=0; i<6; i++) {
    if (report->keycode[i] == keycode)  return true;
  }

  return false;
}

uint8_t fix_colemak_layout(uint8_t colemak_key) {
  if(colemak_key >= HID_KEY_A && colemak_key <= HID_KEY_CAPS_LOCK) {
    const uint8_t new_key = colemakFixMap[colemak_key];
    if(new_key != 0) {
      return new_key;
    }
  }
  return colemak_key;
} 

bool matches_keybind(hid_keyboard_report_t* report, keybind_t* bind) {
  bool is_keybind = false;
  for(uint8_t i=0; i < 2; i++) {
    is_keybind |= (report->modifier & bind->modifiers[i]) != 0 && report->keycode[0] == bind->keycode;
  }
  return is_keybind;
}

bool check_keybinds(hid_keyboard_report_t* report) {
  if(matches_keybind(report, &SARCASM_KEYBIND)) {
    enabled_modes[SARCASM] = !enabled_modes[SARCASM];
    return true;
  }

  if(matches_keybind(report, &COLEMAK_FIX_KEYBIND)) {
    enabled_modes[COLEMAK_FIX] = !enabled_modes[COLEMAK_FIX];
    return true;
  }

  return false;
}

// convert hid keycode to ascii and print via usb device CDC (ignore non-printable)
static void process_kbd_report(uint8_t dev_addr,uint8_t instance, hid_keyboard_report_t* report)
{
  static bool flip = true;
  (void) dev_addr;
  static hid_keyboard_report_t prev_report = { 0, 0, {0} }; // previous report to check key released
  bool flush = false;

  hid_keyboard_report_t new_rep = { 0, 0, {0} };
  memcpy(&new_rep, report, sizeof(hid_keyboard_report_t));

  if(check_keybinds(&new_rep)) return;
  
  if(enabled_modes[COLEMAK_FIX]) {
    for(uint8_t i=0; i<6; i++){
      new_rep.keycode[i] = fix_colemak_layout(new_rep.keycode[i]);
    }
  }

  if(enabled_modes[SARCASM]) {
    bool haschar = false;
    for(uint8_t i=0; i<6; i++){
      uint8_t keycode = new_rep.keycode[i];
      if(keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        haschar = true;
      }
    }

    // don't mess with things that aren't normal typing
    bool isOnlyShift = new_rep.modifier == KEYBOARD_MODIFIER_LEFTSHIFT || new_rep.modifier == KEYBOARD_MODIFIER_RIGHTSHIFT || new_rep.modifier == 0;

    if(isOnlyShift && haschar){
      // we want to flip all keys individually
      for(uint8_t i=0; i<6; i++){
        uint8_t keycode = new_rep.keycode[i];
        if(keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
          uint8_t modifier = flip ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
          hid_keyboard_report_t sArCaSm_RePorT = {modifier, 0, {keycode}};
          flip = !flip;
          tud_hid_keyboard_report(1, sArCaSm_RePorT.modifier, sArCaSm_RePorT.keycode);
        }
      }

      return;
    }
  }

  tud_hid_keyboard_report(1, new_rep.modifier, new_rep.keycode);
  
  for(uint8_t i=0; i<6; i++)
  {
    uint8_t keycode = report->keycode[i];
    if ( keycode )
    {
      if ( find_key_in_report(&prev_report, keycode) )
      {
        // exist in previous report means the current key is holding
      }else{
        static uint8_t leds = 0;

        if (keycode == HID_KEY_CAPS_LOCK) {
          leds ^= KEYBOARD_LED_CAPSLOCK;  //  = 2
          tuh_hid_set_report(dev_addr, instance, 0, HID_REPORT_TYPE_OUTPUT,
                             &leds, sizeof(leds));
        } else if (keycode == HID_KEY_NUM_LOCK) {
          leds ^= KEYBOARD_LED_NUMLOCK;
          tuh_hid_set_report(dev_addr, instance, 0, HID_REPORT_TYPE_OUTPUT,
                             &leds, sizeof(leds));
        } else if (keycode == HID_KEY_SCROLL_LOCK) {
          leds ^= KEYBOARD_LED_SCROLLLOCK;
          tuh_hid_set_report(dev_addr, instance, 0, HID_REPORT_TYPE_OUTPUT,
                             &leds, sizeof(leds));
        }
      }
    }
  }

  if (flush) tud_cdc_write_flush();

  prev_report = *report;
}

// send mouse report to usb device CDC
static void process_mouse_report(uint8_t dev_addr, hid_mouse_report_t const * report)
{
  //------------- button state  -------------//
  //uint8_t button_changed_mask = report->buttons ^ prev_report.buttons;
  char l = report->buttons & MOUSE_BUTTON_LEFT   ? 'L' : '-';
  char m = report->buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-';
  char r = report->buttons & MOUSE_BUTTON_RIGHT  ? 'R' : '-';

  char tempbuf[32];
  int count = sprintf(tempbuf, "[%u] %c%c%c %d %d %d\r\n", dev_addr, l, m, r, report->x, report->y, report->wheel);

  tud_cdc_write(tempbuf, count);
  tud_cdc_write_flush();
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) len;
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch(itf_protocol)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      process_kbd_report(dev_addr, instance, (hid_keyboard_report_t const*) report );
    break;

    case HID_ITF_PROTOCOL_MOUSE:
      process_mouse_report(dev_addr, (hid_mouse_report_t const*) report );
    break;

    default: break;
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    tud_cdc_write_str("Error: cannot request report\r\n");
  }
}
