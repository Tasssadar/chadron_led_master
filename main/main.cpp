#include <stdio.h>
#include <inttypes.h>
#include "rbwifi.h"
#include "gridui.h"
#include "nvs_flash.h"
#include "SmartLeds.h"
#include <array>

#define GRIDUI_LAYOUT_DEFINITION
#include "layout.hpp"

#include "wifi_credentials.h"

using namespace gridui;

static constexpr const int LED_COUNT = 5*30;

static constexpr const int PIN_CLK = 12;
static constexpr const int PIN_DATA = 11;

static nvs_handle_t gNvs;
static TickType_t gNvsNextSave = 0;

enum Mode : uint8_t {
    MODE_RGB,
    MODE_HSV,
    MODE_RAINBOW,

    MODE_MAX
};

static Mode gMode = MODE_RGB;

static const std::array modeCheckboxes {
    &Layout.modeRgb,
    &Layout.modeHsv,
    &Layout.modeRainbow,
};

static Widget* const widgetsModeRgb[] = {
    &Layout.rgbR,
    &Layout.rgbG,
    &Layout.rgbB,
    &Layout.rgbRtit,
    &Layout.rgbGtit,
    &Layout.rgbBtit,
    nullptr,
};

static Widget* const widgetsModeHsv[] = {
    &Layout.hsvH,
    &Layout.hsvS,
    &Layout.hsvV,
    &Layout.hsvHtit,
    &Layout.hsvStit,
    &Layout.hsvVtit,
    nullptr,
};

static Widget* const widgetsModeRainbow[] = {
    &Layout.rainbowSpeed,
    &Layout.rainbowLength,
    &Layout.rainbowLengthTit,
    &Layout.rainbowSpeedTitle,
    nullptr,
};

static const std::array<Widget* const *, MODE_MAX> widgetsPerMode {
    widgetsModeRgb,
    widgetsModeHsv,
    widgetsModeRainbow,
};

static const std::array slidersToSave {
    &Layout.brightness,
    &Layout.rainbowSpeed,
    &Layout.rainbowLength,
    &Layout.rgbR,
    &Layout.rgbG,
    &Layout.rgbB,
    &Layout.hsvH,
    &Layout.hsvS,
    &Layout.hsvV,
};

static void displayWidgetsForMode(Mode newMode) {
    for(size_t mode = 0; mode < widgetsPerMode.size(); ++mode) {
        auto widgets = widgetsPerMode[mode];
        for(size_t i = 0; widgets[i] != nullptr; ++i) {
            auto w = widgets[i];
            if(mode == newMode) {
                w->setCss("visibility", "");
            } else {
                w->setCss("visibility", "hidden");
            }
        }
    }
}

static void scheduleNvsSave() {
    gNvsNextSave = xTaskGetTickCount() + pdMS_TO_TICKS(500);
}

static void onModeChanged(Checkbox& checkedCb) {
    if(!checkedCb.checked()) {
        return;
    }

    for(size_t i = 0; i < modeCheckboxes.size(); ++i) {
        auto cb = modeCheckboxes[i];
        if(cb->uuid() == checkedCb.uuid()) {
            gMode = (Mode)i;
            displayWidgetsForMode(gMode);
            ESP_ERROR_CHECK(nvs_set_u8(gNvs, "mode", i));
            scheduleNvsSave();
        } else {
            cb->setChecked(false);
        }
    }
}

static void onSaveSliderChanged(Slider& s) {
    char buff[8];
    snprintf(buff, sizeof(buff), "s%x", s.uuid());
    ESP_ERROR_CHECK(nvs_set_i32(gNvs, buff, s.value()));
    scheduleNvsSave();
}

static void buildWidgets(builder::_LayoutBuilder builder) {
    char buff[8];
    std::array modeSwitchers {
        &builder.modeRgb,
        &builder.modeHsv,
        &builder.modeRainbow,
    };
    for(int i = 0; i < modeSwitchers.size(); ++i) {
        auto *sw = modeSwitchers[i];
        sw->checked(i == gMode);
        sw->onChanged(onModeChanged);
    }

    std::array slidersToSaveBuilder {
        &builder.brightness,
        &builder.rainbowSpeed,
        &builder.rainbowLength,
        &builder.rgbR,
        &builder.rgbG,
        &builder.rgbB,
        &builder.hsvH,
        &builder.hsvS,
        &builder.hsvV,
    };
    for(auto slider : slidersToSaveBuilder) {
        slider->onChanged(onSaveSliderChanged);
    }

    builder.commit();

    displayWidgetsForMode(gMode);

    // Load saved slider values
    for(auto s : slidersToSave) {
        snprintf(buff, sizeof(buff), "s%x", s->uuid());
        int32_t value = s->value();
        if(nvs_get_i32(gNvs, buff, &value) == ESP_OK) {
            s->setValue(value);
        }
    }
}

extern "C" void app_main(void)
{
    printf("Starting\n");
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("chadron", NVS_READWRITE, &gNvs));

    nvs_get_u8(gNvs, "mode", (uint8_t*)&gMode);

    rb::WiFi::connect(WIFI_NAME, WIFI_PASS);
    UI.begin("VojtechBocek", "Chadron LED master");
    buildWidgets(Layout.begin());

    Apa102 leds(LED_COUNT, PIN_CLK, PIN_DATA, DoubleBuffer, 10*1000*1000);
    leds.show();

    uint8_t rainbowOffset = 0;
    while(true) {
        const uint8_t brightness = 0xE0 | uint8_t(Layout.brightness.value());

        TickType_t delay = pdMS_TO_TICKS(50);
        switch(gMode) {
        case MODE_RGB: {
            const Rgb clr(Layout.rgbR.value(), Layout.rgbG.value(), Layout.rgbB.value());
            for(int i = 0; i < LED_COUNT; ++i) {
                leds[i].v = brightness;
                leds[i] = clr;
            }
            break;
        }
        case MODE_HSV: {
            const Hsv clr(Layout.hsvH.value(), Layout.hsvS.value(), Layout.hsvV.value());
            for(int i = 0; i < LED_COUNT; ++i) {
                leds[i].v = brightness;
                leds[i] = clr;
            }
            break;
        }
        case MODE_RAINBOW: {
            const int step = Layout.rainbowLength.value();
            for(int i = 0; i < LED_COUNT; ++i) {
                leds[i].v = brightness;
                leds[i] = Hsv(rainbowOffset + i*step, 255, 255);
            }
            rainbowOffset += step;
            delay = pdMS_TO_TICKS(Layout.rainbowSpeed.value());
            break;
        }
        default:
            break;
        }

        leds.wait();
        leds.show();
        vTaskDelay(delay);

        if(gNvsNextSave != 0 && xTaskGetTickCount() > gNvsNextSave) {
            nvs_commit(gNvs);
            gNvsNextSave = 0;
        }
    }
}
