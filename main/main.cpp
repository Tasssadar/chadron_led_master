#include <array>
#include <cmath>
#include <inttypes.h>
#include <map>
#include <set>
#include <stdio.h>
#include <thread>

#include "SmartLeds.h"
#include "esp_netif.h"
#include "gridui.h"
#include "nvs_flash.h"
#include "rbwifi.h"

#include "fastled.h"
#include "wifi_credentials.h"

#define GRIDUI_LAYOUT_DEFINITION
#include "layout.hpp"

using namespace gridui;

static constexpr const int CNT_FRONT_BOTTOM = 16;
static constexpr const int CNT_FRONT_TOP = 15;
static constexpr const int CNT_FRONT = CNT_FRONT_BOTTOM + CNT_FRONT_TOP;
static constexpr const int CNT_BACK_LEFT = 10;
static constexpr const int CNT_BACK_BOTTOM = 19;
static constexpr const int CNT_BACK_RIGHT = 9;
static constexpr const int CNT_BACK_TOP = 19;
static constexpr const int CNT_BACK = CNT_BACK_LEFT + CNT_BACK_BOTTOM + CNT_BACK_RIGHT + CNT_BACK_TOP;

static constexpr const int LED_COUNT = CNT_FRONT + CNT_BACK;

static constexpr const int PIN_CLK = 12;
static constexpr const int PIN_DATA = 11;

static nvs_handle_t gNvs;
static TickType_t gNvsNextSave = 0;

struct ColorPreset {
    const char* const name;
    Rgb color;
};

static const std::array COLOR_PRESETS = {
    ColorPreset { "Warm", Rgb(255, 125, 17) },
    ColorPreset { "Yellow", Rgb(255, 115, 0) },
};

enum Mode : uint8_t {
    MODE_RGB,
    MODE_HSV,
    MODE_RAINBOW,

    MODE_MAX
};

enum SubMode : uint8_t {
    SUB_NONE,

    SUB_BREATHE,
    SUB_WAVE,
};

static Mode gMode = MODE_RGB;
static SubMode gSub = SUB_NONE;

static const std::array modeCheckboxes {
    &Layout.modeRgb,
    &Layout.modeHsv,
    &Layout.modeRainbow,
};

static const std::array subCheckboxes {
    &Layout.subBreathe,
    &Layout.subWave,
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

static std::vector<Button> rgbPresetWidgets;

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
    &Layout.rainbowSpeedTit,
    &Layout.rainboxModeYellow,
    nullptr,
};

static Widget* const widgetsSubBreathe[] = {
    &Layout.breatheDelay,
    &Layout.breatheDelayTit,
    &Layout.breatheMinBrightness,
    &Layout.breatheMinBrightnessTit,
    nullptr,
};

static Widget* const widgetsSubWave[] = {
    &Layout.waveDelay,
    &Layout.waveDelayTit,
    &Layout.waveMinBrightness,
    &Layout.waveMinBrightnessTit,
    &Layout.waveSize,
    &Layout.waveSizeTit,
    nullptr,
};

static const std::array<Widget* const*, MODE_MAX> widgetsPerMode {
    widgetsModeRgb,
    widgetsModeHsv,
    widgetsModeRainbow,
};

static const std::map<Mode, std::set<SubMode>> subsPerMode {
    { MODE_RGB, { SUB_BREATHE, SUB_WAVE } },
    { MODE_HSV, { SUB_BREATHE, SUB_WAVE } },
};

static const std::map<SubMode, Widget* const*> widgetsPerSub {
    { SUB_BREATHE, widgetsSubBreathe },
    { SUB_WAVE, widgetsSubWave },
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
    &Layout.breatheDelay,
    &Layout.breatheMinBrightness,
    &Layout.waveDelay,
    &Layout.waveSize,
    &Layout.waveMinBrightness,
};

static std::vector<builder::Slider*> slidersToSaveBuilder(const builder::_LayoutBuilder& builder) {
    return std::vector {
        &builder.brightness,
        &builder.rainbowSpeed,
        &builder.rainbowLength,
        &builder.rgbR,
        &builder.rgbG,
        &builder.rgbB,
        &builder.hsvH,
        &builder.hsvS,
        &builder.hsvV,
        &builder.breatheDelay,
        &builder.breatheMinBrightness,
        &builder.waveDelay,
        &builder.waveSize,
        &builder.waveMinBrightness,
    };
}

static void stretchChannelsEvenly(Apa102::ApaRgb& led, uint8_t max) {
    led.r = (uint16_t(led.r) * max) >> 8;
    led.g = (uint16_t(led.g) * max) >> 8;
    led.b = (uint16_t(led.b) * max) >> 8;
}

static void displaySubWidgets() {
    auto activeSubs = subsPerMode.find(gMode);
    if (activeSubs != subsPerMode.end()) {
        for (auto sub : activeSubs->second) {
            auto widgets = widgetsPerSub.at(sub);
            for (size_t i = 0; widgets[i] != nullptr; ++i) {
                auto w = widgets[i];
                if (sub == gSub) {
                    w->setCss("visibility", "");
                } else {
                    w->setCss("visibility", "hidden");
                }
            }
        }
        for (auto cb : subCheckboxes) {
            cb->setCss("visibility", "");
        }
    } else {
        for (auto cb : subCheckboxes) {
            cb->setCss("visibility", "hidden");
        }
    }

    for (auto itr : widgetsPerSub) {
        if (activeSubs != subsPerMode.end() && activeSubs->second.contains(itr.first)) {
            continue;
        }

        auto widgets = itr.second;
        for (size_t i = 0; widgets[i] != nullptr; ++i) {
            auto w = widgets[i];
            w->setCss("visibility", "hidden");
        }
    }
}

static void displayWidgetsForMode() {
    for (size_t mode = 0; mode < widgetsPerMode.size(); ++mode) {
        auto widgets = widgetsPerMode[mode];
        for (size_t i = 0; widgets[i] != nullptr; ++i) {
            auto w = widgets[i];
            if (mode == gMode) {
                w->setCss("visibility", "");
            } else {
                w->setCss("visibility", "hidden");
            }
        }
    }

    for (auto& btn : rgbPresetWidgets) {
        if (gMode == MODE_RGB) {
            btn.setCss("visibility", "");
        } else {
            btn.setCss("visibility", "hidden");
        }
    }

    displaySubWidgets();
}

static void scheduleNvsSave() {
    gNvsNextSave = xTaskGetTickCount() + pdMS_TO_TICKS(500);
}

static void onModeChanged(Checkbox& checkedCb) {
    for (size_t i = 0; i < modeCheckboxes.size(); ++i) {
        auto cb = modeCheckboxes[i];
        if (cb->uuid() == checkedCb.uuid()) {
            gMode = (Mode)i;
            displayWidgetsForMode();
            ESP_ERROR_CHECK(nvs_set_u8(gNvs, "mode", i));
            scheduleNvsSave();
            if (!cb->checked()) {
                cb->setChecked(true);
            }
        } else {
            cb->setChecked(false);
        }
    }
}

static void onSubChanged(Checkbox& checkedCb) {
    gSub = SUB_NONE;
    for (size_t i = 0; i < subCheckboxes.size(); ++i) {
        auto cb = subCheckboxes[i];
        if (cb->checked()) {
            if (cb->uuid() == checkedCb.uuid()) {
                gSub = (SubMode)(i + 1);
            } else {
                cb->setChecked(false);
            }
        }
    }
    displaySubWidgets();
    ESP_ERROR_CHECK(nvs_set_u8(gNvs, "sub", gSub));
}

static void onSaveSliderChanged(Slider& s) {
    char buff[8];
    snprintf(buff, sizeof(buff), "s%x", s.uuid());
    ESP_ERROR_CHECK(nvs_set_i32(gNvs, buff, s.value()));
    scheduleNvsSave();
}

static void buildColorPresets(builder::_LayoutBuilder builder) {
    const float layoutWidth = 12 - 1; // minus padding
    float x = 0.5f;
    const float y = 4;
    const float btnHeight = 0.5f;
    const float btnWidth = ((layoutWidth - (float(COLOR_PRESETS.size() - 1) * 0.5f)) / COLOR_PRESETS.size());

    for (const auto& preset : COLOR_PRESETS) {
        auto& btn = UI.button(x, y, btnWidth, btnHeight);
        btn.text(preset.name);

        const auto color = preset.color;
        btn.onPress([color](Button&) {
            Layout.rgbR.setValue(color.r);
            Layout.rgbG.setValue(color.g);
            Layout.rgbB.setValue(color.b);
        });

        rgbPresetWidgets.emplace_back(btn.finish());
        x += btnWidth + 0.5f;
    }
}

static void buildSavedCheckbox(builder::Checkbox& cb, const char* valueName, bool defaultValue) {
    uint8_t enable = defaultValue ? 1 : 0;
    nvs_get_u8(gNvs, valueName, &enable);
    cb.checked(enable);
    cb.onChanged([=](Checkbox& b) {
        ESP_ERROR_CHECK(nvs_set_u8(gNvs, valueName, b.checked()));
        scheduleNvsSave();
    });
}

static void buildWidgets(builder::_LayoutBuilder builder) {
    char buff[8];
    std::array modeSwitchers {
        &builder.modeRgb,
        &builder.modeHsv,
        &builder.modeRainbow,
    };
    for (int i = 0; i < modeSwitchers.size(); ++i) {
        auto* sw = modeSwitchers[i];
        sw->checked(i == gMode);
        sw->onChanged(onModeChanged);
    }

    std::array subSwitchers {
        &builder.subBreathe,
        &builder.subWave,
    };
    for (int i = 0; i < subSwitchers.size(); ++i) {
        auto* sw = subSwitchers[i];
        sw->checked(i + 1 == gSub);
        sw->onChanged(onSubChanged);
    }

    for (auto slider : slidersToSaveBuilder(builder)) {
        slider->onChanged(onSaveSliderChanged);
    }

    buildSavedCheckbox(builder.enableBack, "eb", true);
    buildSavedCheckbox(builder.enableFront, "ef", true);
    buildSavedCheckbox(builder.rainboxModeYellow, "rnYel", false);

    buildColorPresets(builder);

    builder.commit();

    displayWidgetsForMode();

    // Load saved slider values
    for (auto s : slidersToSave) {
        snprintf(buff, sizeof(buff), "s%x", s->uuid());
        int32_t value = s->value();
        if (nvs_get_i32(gNvs, buff, &value) == ESP_OK) {
            s->setValue(value);
        }
    }
}

static TickType_t applySub(Apa102& leds) {
    switch (gSub) {
    case SUB_BREATHE: {
        static float mult = 0.5f;
        static float delta = 0.005f;

        if (mult >= 1.f) {
            delta = -0.005f;
        } else if (mult <= 0.f) {
            delta = 0.005f;
        }
        mult += delta;

        const int16_t min = Layout.breatheMinBrightness.value();
        int16_t curBrightness = min + (mult * mult) * (255 - min);
        curBrightness = std::min(int16_t(255), std::max(int16_t(0), curBrightness));

        auto base = Rgb(leds[0].r, leds[0].g, leds[0].b);
        base.stretchChannelsEvenly(curBrightness);
        for (int i = 0; i < LED_COUNT; ++i) {
            leds[i] = base;
        }
        return pdMS_TO_TICKS(Layout.breatheDelay.value());
    }
    case SUB_WAVE: {
        static float offset = 0;
        const float size = Layout.waveSize.value();
        const float segment = size * 2;
        const int16_t min = Layout.waveMinBrightness.value();
        const auto base = Rgb(leds[0].r, leds[0].g, leds[0].b);
        for (int16_t i = 0; i < LED_COUNT; ++i) {
            float posInSegment = std::fmod((float(i) + offset), segment) / size;
            if (posInSegment > 1.f) {
                posInSegment = 2.f - posInSegment;
            }

            int16_t cur = min + (posInSegment * posInSegment) * (255 - min);
            cur = std::min(int16_t(255), std::max(int16_t(0), cur));

            auto curRgb = base;
            curRgb.stretchChannelsEvenly(cur);
            leds[i] = curRgb;
        }

        offset += 0.01;
        if (offset >= segment) {
            offset = 0;
        }
        return pdMS_TO_TICKS(Layout.waveDelay.value() / 100);
    }
    default:
        return pdMS_TO_TICKS(100);
    }
}

static bool applyStartupAnim(Apa102& leds) {
    static constexpr const float duration = pdMS_TO_TICKS(600);
    static const TickType_t start = xTaskGetTickCount();
    TickType_t now = xTaskGetTickCount();

    const float c = float(now - start) / duration;
    if (c >= 1.f) {
        return false;
    }

    float posC;

    static const constexpr size_t OFF_BACK_LEFT = CNT_FRONT + CNT_BACK_LEFT;
    static const constexpr size_t OFF_BACK_BOTTOM = OFF_BACK_LEFT + CNT_BACK_BOTTOM;
    static const constexpr size_t OFF_BACK_RIGHT = OFF_BACK_BOTTOM + CNT_BACK_RIGHT;

    for (size_t i = 0; i < LED_COUNT; ++i) {
        if (i <= CNT_FRONT_BOTTOM / 2) {
            posC = float(i) / (CNT_FRONT_BOTTOM / 2);
        } else if (i < CNT_FRONT_BOTTOM) {
            posC = float(CNT_FRONT_BOTTOM - i) / (CNT_FRONT_BOTTOM / 2);
        } else if (i < CNT_FRONT_BOTTOM + CNT_FRONT_TOP / 2) {
            posC = float(i - CNT_FRONT_BOTTOM) / (CNT_FRONT_TOP / 2);
        } else if (i < CNT_FRONT) {
            posC = float(CNT_FRONT_TOP - (i - CNT_FRONT_BOTTOM)) / (CNT_FRONT_TOP / 2);
        } else if (i < OFF_BACK_LEFT) {
            posC = 0.f;
        } else if (i <= OFF_BACK_LEFT + CNT_BACK_BOTTOM / 2) {
            posC = float(i - OFF_BACK_LEFT) / (CNT_BACK_BOTTOM / 2);
        } else if (i < OFF_BACK_LEFT + CNT_BACK_BOTTOM) {
            posC = float(CNT_BACK_BOTTOM - (i - OFF_BACK_LEFT)) / (CNT_BACK_BOTTOM / 2);
        } else if (i < OFF_BACK_BOTTOM + CNT_BACK_RIGHT) {
            posC = 0.f;
        } else if (i <= OFF_BACK_RIGHT + CNT_BACK_TOP / 2) {
            posC = float(i - OFF_BACK_RIGHT) / (CNT_BACK_TOP / 2);
        } else if (i < OFF_BACK_RIGHT + CNT_BACK_TOP) {
            posC = float(CNT_BACK_TOP - (i - OFF_BACK_RIGHT)) / (CNT_BACK_TOP / 2);
        }

        const int cur = ((posC - 1) + c * 2) * 255;
        stretchChannelsEvenly(leds[i], std::max(0, std::min(255, cur)));
    }
    return true;
}

static void wifiConnectTask(void*) {
    rb::WiFi::connect(WIFI_NAME, WIFI_PASS);
    vTaskDelete(NULL);
}

extern "C" void app_main(void) {
    Apa102 leds(LED_COUNT, PIN_CLK, PIN_DATA, DoubleBuffer, 2'000'000);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("chadron", NVS_READWRITE, &gNvs));

    nvs_get_u8(gNvs, "mode", (uint8_t*)&gMode);
    nvs_get_u8(gNvs, "sub", (uint8_t*)&gSub);

    esp_netif_init();
    UI.begin("VojtechBocek", "Chadron LED master");
    buildWidgets(Layout.begin());

    xTaskCreatePinnedToCore(wifiConnectTask, "", 4096, NULL, 1, NULL, 1);

    const Apa102::ApaRgb rgbOff;

    auto* previous_data = new Apa102::ApaRgb[LED_COUNT];

    float rainbowOffset = 0;
    bool wait_for_show = true;
    bool startup_done = false;

    while (true) {
        const uint8_t brightness = 0xE0 | uint8_t(Layout.brightness.value());

        TickType_t delay = 0;
        switch (gMode) {
        case MODE_RGB: {
            const Rgb clr(Layout.rgbR.value(), Layout.rgbG.value(), Layout.rgbB.value());
            for (int i = 0; i < LED_COUNT; ++i) {
                leds[i].v = brightness;
                leds[i] = clr;
            }
            delay = applySub(leds);
            break;
        }
        case MODE_HSV: {
            const Hsv clr(Layout.hsvH.value(), Layout.hsvS.value(), Layout.hsvV.value());
            for (int i = 0; i < LED_COUNT; ++i) {
                leds[i].v = brightness;
                leds[i] = clr;
            }
            delay = applySub(leds);
            break;
        }
        case MODE_RAINBOW: {
            const int step = Layout.rainbowLength.value();
            const bool yellowOnly = Layout.rainboxModeYellow.checked();
            for (int i = 0; i < LED_COUNT; ++i) {
                leds[i].v = brightness;

                uint8_t h = rainbowOffset + i * step;

                if (yellowOnly) {
                    if (h <= 127) {
                        h = (float(h) / 127) * 35;
                    } else {
                        h = (1 - (float(h - 128) / 127)) * 35;
                    }
                }
                hsv2rgb_rainbow(Hsv(h, 255, 255), leds[i]);
            }
            rainbowOffset += float(step) / 100.f;
            if (rainbowOffset >= 255.f) {
                rainbowOffset = 0.f;
            }
            delay = pdMS_TO_TICKS(Layout.rainbowSpeed.value() / 100.f);
            break;
        }
        default:
            break;
        }

        if (!Layout.enableFront.checked()) {
            std::fill_n((uint32_t*)&leds[0], CNT_FRONT, *((uint32_t*)&rgbOff));
        }
        if (!Layout.enableBack.checked()) {
            std::fill_n((uint32_t*)&leds[CNT_FRONT], CNT_BACK, *((uint32_t*)&rgbOff));
        }

        if (!startup_done) {
            if (applyStartupAnim(leds)) {
                if (delay > pdMS_TO_TICKS(50)) {
                    delay = pdMS_TO_TICKS(50);
                }
            } else {
                startup_done = true;
            }
        }

        auto start = xTaskGetTickCount();
        if (wait_for_show) {
            leds.wait();
            wait_for_show = false;
        }

        auto waitDuration = xTaskGetTickCount() - start;
        if (waitDuration < delay) {
            delay -= waitDuration;
        } else {
            delay = 0;
        }
        vTaskDelay(delay);

        if (memcmp(previous_data, &leds[0], LED_COUNT * sizeof(Apa102::ApaRgb)) != 0) {
            memcpy(previous_data, &leds[0], LED_COUNT * sizeof(Apa102::ApaRgb));
            leds.show();
            wait_for_show = true;
        }

        if (gNvsNextSave != 0 && xTaskGetTickCount() > gNvsNextSave) {
            nvs_commit(gNvs);
            gNvsNextSave = 0;
        }
    }
}
