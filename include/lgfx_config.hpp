#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI3_HOST;   // VSPI
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = 0;           // tắt DMA cho dễ share bus với RC522
      cfg.pin_sclk    = 18;
      cfg.pin_mosi    = 23;
      cfg.pin_miso    = 19;          // chân MISO của RC522 (bus dùng chung)
      cfg.pin_dc      = 16;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs          = 5;
      cfg.pin_rst         = 17;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 170;
      cfg.panel_height    = 320;
      cfg.offset_x        = 35;      // màn 1.9" 170x320 lệch 35px
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.readable        = false;
      cfg.invert          = true;
      cfg.rgb_order       = false;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = true;    // nhả bus sau mỗi lần vẽ
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 4;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};