/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include <platform.h>

#ifdef USE_LED_STRIP

#include "build/build_config.h"

#include "common/axis.h"
#include "common/color.h"
#include "common/maths.h"
#include "common/printf.h"
#include "common/typeconversion.h"
#include "common/utils.h"

#include "config/feature.h"
#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "drivers/light_ws2811strip.h"
#include "drivers/serial.h"
#include "drivers/vtx_common.h"

#include "fc/config.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"

#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/navigation.h"
#include "flight/pid.h"
#include "flight/servos.h"

#include "io/beeper.h"
#include "io/gimbal.h"
#include "io/gps.h"
#include "io/ledstrip.h"
#include "io/serial.h"
#include "io/vtx_string.h"

#include "rx/rx.h"

#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/battery.h"
#include "sensors/boardalignment.h"
#include "sensors/gyro.h"
#include "sensors/sensors.h"

#include "telemetry/telemetry.h"

PG_REGISTER_ARRAY_WITH_RESET_FN(ledStripConfig_t, LED_STRIP_PROFILE_COUNT, ledStripProfiles, PG_LED_STRIP_PROFILES, 0);

ledStripConfig_t *currentLedStripProfile;

static bool ledStripInitialised = false;
static bool ledStripEnabled = true;

static void ledStripDisable(void);

//#define USE_LED_ANIMATION

#define HZ_TO_US(hz) ((int32_t)((1000 * 1000) / (hz)))

#define MAX_TIMER_DELAY (5 * 1000 * 1000)

#if LED_MAX_STRIP_LENGTH > WS2811_LED_STRIP_LENGTH
# error "Led strip length must match driver"
#endif

typedef enum {
    COLOR_BLACK = 0,
    COLOR_WHITE,
    COLOR_RED,
    COLOR_ORANGE,
    COLOR_YELLOW,
    COLOR_LIME_GREEN,
    COLOR_GREEN,
    COLOR_MINT_GREEN,
    COLOR_CYAN,
    COLOR_LIGHT_BLUE,
    COLOR_BLUE,
    COLOR_DARK_VIOLET,
    COLOR_MAGENTA,
    COLOR_DEEP_PINK
} colorId_e;

const hsvColor_t hsv[] = {
    //                        H    S    V
    [COLOR_BLACK] =        {  0,   0,   0},
    [COLOR_WHITE] =        {  0, 255, 255},
    [COLOR_RED] =          {  0,   0, 255},
    [COLOR_ORANGE] =       { 30,   0, 255},
    [COLOR_YELLOW] =       { 60,   0, 255},
    [COLOR_LIME_GREEN] =   { 90,   0, 255},
    [COLOR_GREEN] =        {120,   0, 255},
    [COLOR_MINT_GREEN] =   {150,   0, 255},
    [COLOR_CYAN] =         {180,   0, 255},
    [COLOR_LIGHT_BLUE] =   {210,   0, 255},
    [COLOR_BLUE] =         {240,   0, 255},
    [COLOR_DARK_VIOLET] =  {270,   0, 255},
    [COLOR_MAGENTA] =      {300,   0, 255},
    [COLOR_DEEP_PINK] =    {330,   0, 255},
};
// macro to save typing on default colors
#define HSV(color) (hsv[COLOR_ ## color])

STATIC_UNIT_TESTED uint8_t ledGridRows;
// grid offsets
STATIC_UNIT_TESTED int8_t highestYValueForNorth;
STATIC_UNIT_TESTED int8_t lowestYValueForSouth;
STATIC_UNIT_TESTED int8_t highestXValueForWest;
STATIC_UNIT_TESTED int8_t lowestXValueForEast;

STATIC_UNIT_TESTED ledCounts_t ledCounts;

static const modeColorIndexes_t defaultModeColors[] = {
    //                          NORTH             EAST               SOUTH            WEST             UP          DOWN
    [LED_MODE_ORIENTATION] = {{ COLOR_WHITE,      COLOR_DARK_VIOLET, COLOR_RED,       COLOR_DEEP_PINK, COLOR_BLUE, COLOR_ORANGE }},
    [LED_MODE_HEADFREE]    = {{ COLOR_LIME_GREEN, COLOR_DARK_VIOLET, COLOR_ORANGE,    COLOR_DEEP_PINK, COLOR_BLUE, COLOR_ORANGE }},
    [LED_MODE_HORIZON]     = {{ COLOR_BLUE,       COLOR_DARK_VIOLET, COLOR_YELLOW,    COLOR_DEEP_PINK, COLOR_BLUE, COLOR_ORANGE }},
    [LED_MODE_ANGLE]       = {{ COLOR_CYAN,       COLOR_DARK_VIOLET, COLOR_YELLOW,    COLOR_DEEP_PINK, COLOR_BLUE, COLOR_ORANGE }},
    [LED_MODE_MAG]         = {{ COLOR_MINT_GREEN, COLOR_DARK_VIOLET, COLOR_ORANGE,    COLOR_DEEP_PINK, COLOR_BLUE, COLOR_ORANGE }},
    [LED_MODE_BARO]        = {{ COLOR_LIGHT_BLUE, COLOR_DARK_VIOLET, COLOR_RED,       COLOR_DEEP_PINK, COLOR_BLUE, COLOR_ORANGE }},
};

static const specialColorIndexes_t defaultSpecialColors[] = {
    {{ [LED_SCOLOR_DISARMED]        = COLOR_GREEN,
       [LED_SCOLOR_ARMED]           = COLOR_BLUE,
       [LED_SCOLOR_ANIMATION]       = COLOR_WHITE,
       [LED_SCOLOR_BACKGROUND]      = COLOR_BLACK,
       [LED_SCOLOR_BLINKBACKGROUND] = COLOR_BLACK,
       [LED_SCOLOR_GPSNOSATS]       = COLOR_RED,
       [LED_SCOLOR_GPSNOLOCK]       = COLOR_ORANGE,
       [LED_SCOLOR_GPSLOCKED]       = COLOR_GREEN,
    }}
};

void setLedStripProfile(uint8_t ledStripProfileIndex)
{
    if (ledStripProfileIndex < LED_STRIP_PROFILE_COUNT) {
        systemConfigMutable()->activeLedStripProfile = ledStripProfileIndex;
        currentLedStripProfile = ledStripProfilesMutable(ledStripProfileIndex);
    }
}

void pgResetFn_ledStripProfiles(ledStripConfig_t *ledStripConfig)
{
	for(int i = 0; i < LED_STRIP_PROFILE_COUNT; i++) {
		ledStripConfig_t *ledStripConfigToReset = &ledStripConfig[i]; 
		memset(ledStripConfigToReset->ledConfigs, 0, LED_MAX_STRIP_LENGTH * sizeof(ledConfig_t));
    	// copy hsv colors as default
   		memset(ledStripConfigToReset->colors, 0, ARRAYLEN(hsv) * sizeof(hsvColor_t));
    	BUILD_BUG_ON(LED_CONFIGURABLE_COLOR_COUNT < ARRAYLEN(hsv));
    	for (unsigned colorIndex = 0; colorIndex < ARRAYLEN(hsv); colorIndex++) {
        	ledStripConfigToReset->colors[colorIndex] = hsv[colorIndex];
    	}
    	memcpy_fn(&ledStripConfigToReset->modeColors, &defaultModeColors, sizeof(defaultModeColors));
    	memcpy_fn(&ledStripConfigToReset->specialColors, &defaultSpecialColors, sizeof(defaultSpecialColors));
    	ledStripConfigToReset->ledstrip_visual_beeper = 0;
    	ledStripConfigToReset->ledstrip_aux_channel = THROTTLE;

    	for (int i = 0; i < USABLE_TIMER_CHANNEL_COUNT; i++) {
        	if (timerHardware[i].usageFlags & TIM_USE_LED) {
            	ledStripConfigToReset->ioTag = timerHardware[i].tag;
            	return;
        	}
    	}
   		ledStripConfigToReset->ioTag = IO_TAG_NONE;
	}
}

static int scaledThrottle;
static int auxInput;

static void updateLedRingCounts(void);

STATIC_UNIT_TESTED void updateDimensions(void)
{
    int maxX = 0;
    int minX = LED_XY_MASK;
    int maxY = 0;
    int minY = LED_XY_MASK;

    for (int ledIndex = 0; ledIndex < ledCounts.count; ledIndex++) {
		const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];

        int ledX = ledGetX(ledConfig);
        maxX = MAX(ledX, maxX);
        minX = MIN(ledX, minX);
        int ledY = ledGetY(ledConfig);
        maxY = MAX(ledY, maxY);
        minY = MIN(ledY, minY);
    }

    ledGridRows = maxY - minY + 1;

    if (minX < maxX) {
        lowestXValueForEast = (minX + maxX) / 2 + 1;
        highestXValueForWest = (minX + maxX - 1) / 2;
    } else {
        lowestXValueForEast = LED_XY_MASK / 2;
        highestXValueForWest = lowestXValueForEast - 1;
    }
    if (minY < maxY) {
        lowestYValueForSouth = (minY + maxY) / 2 + 1;
        highestYValueForNorth = (minY + maxY - 1) / 2;
    } else {
        lowestYValueForSouth = LED_XY_MASK / 2;
        highestYValueForNorth = lowestYValueForSouth - 1;
    }

}

STATIC_UNIT_TESTED void updateLedCount(void)
{
    int count = 0, countRing = 0, countScanner= 0;

    for (int ledIndex = 0; ledIndex < LED_MAX_STRIP_LENGTH; ledIndex++) {
		const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex]; 

        if (!(*ledConfig))
            break;

        count++;

        if (ledGetFunction(ledConfig) == LED_FUNCTION_THRUST_RING)
            countRing++;

        if (ledGetOverlayBit(ledConfig, LED_OVERLAY_LARSON_SCANNER))
            countScanner++;
    }

    ledCounts.count = count;
    ledCounts.ring = countRing;
    ledCounts.larson = countScanner;
}

void reevaluateLedConfig(void)
{
    updateLedCount();
    updateDimensions();
    updateLedRingCounts();
}

// get specialColor by index
static const hsvColor_t* getSC(ledSpecialColorIds_e index)
{
	return &ledStripProfiles(systemConfig()->activeLedStripProfile)->colors[ledStripProfiles(systemConfig()->activeLedStripProfile)->specialColors.color[index]];
}

static const char directionCodes[LED_DIRECTION_COUNT] = { 'N', 'E', 'S', 'W', 'U', 'D' };
static const char baseFunctionCodes[LED_BASEFUNCTION_COUNT]   = { 'C', 'F', 'A', 'L', 'S', 'G', 'R' };
static const char overlayCodes[LED_OVERLAY_COUNT]   = { 'T', 'O', 'B', 'V', 'I', 'W' };

#define CHUNK_BUFFER_SIZE 11

bool parseLedStripConfig(int ledIndex, const char *config)
{
    if (ledIndex >= LED_MAX_STRIP_LENGTH)
        return false;

    enum parseState_e {
        X_COORDINATE,
        Y_COORDINATE,
        DIRECTIONS,
        FUNCTIONS,
        RING_COLORS,
        PARSE_STATE_COUNT
    };
    static const char chunkSeparators[PARSE_STATE_COUNT] = {',', ':', ':', ':', '\0'};

  	ledConfig_t *ledConfig = &ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];
	memset((void *)ledConfig, 0, sizeof(ledConfig_t));

    int x = 0, y = 0, color = 0;   // initialize to prevent warnings
    int baseFunction = 0;
    int overlay_flags = 0;
    int direction_flags = 0;

    for (enum parseState_e parseState = 0; parseState < PARSE_STATE_COUNT; parseState++) {
        char chunk[CHUNK_BUFFER_SIZE];
        {
            char chunkSeparator = chunkSeparators[parseState];
            int chunkIndex = 0;
            while (*config  && *config != chunkSeparator && chunkIndex < (CHUNK_BUFFER_SIZE - 1)) {
                chunk[chunkIndex++] = *config++;
            }
            chunk[chunkIndex++] = 0; // zero-terminate chunk
            if (*config != chunkSeparator) {
                return false;
            }
            config++;   // skip separator
        }
        switch (parseState) {
            case X_COORDINATE:
                x = atoi(chunk);
                break;
            case Y_COORDINATE:
                y = atoi(chunk);
                break;
            case DIRECTIONS:
                for (char *ch = chunk; *ch; ch++) {
                    for (ledDirectionId_e dir = 0; dir < LED_DIRECTION_COUNT; dir++) {
                        if (directionCodes[dir] == *ch) {
                            direction_flags |= LED_FLAG_DIRECTION(dir);
                            break;
                        }
                    }
                }
                break;
            case FUNCTIONS:
                for (char *ch = chunk; *ch; ch++) {
                    for (ledBaseFunctionId_e fn = 0; fn < LED_BASEFUNCTION_COUNT; fn++) {
                        if (baseFunctionCodes[fn] == *ch) {
                            baseFunction = fn;
                            break;
                        }
                    }

                    for (ledOverlayId_e ol = 0; ol < LED_OVERLAY_COUNT; ol++) {
                        if (overlayCodes[ol] == *ch) {
                            overlay_flags |= LED_FLAG_OVERLAY(ol);
                            break;
                        }
                    }
                }
                break;
            case RING_COLORS:
                color = atoi(chunk);
                if (color >= LED_CONFIGURABLE_COLOR_COUNT)
                    color = 0;
                break;
            case PARSE_STATE_COUNT:; // prevent warning
        }
    }

    *ledConfig = DEFINE_LED(x, y, color, direction_flags, baseFunction, overlay_flags, 0);

    reevaluateLedConfig();

    return true;
}

void generateLedConfig(ledConfig_t *ledConfig, char *ledConfigBuffer, size_t bufferSize)
{
    char directions[LED_DIRECTION_COUNT + 1];
    char baseFunctionOverlays[LED_OVERLAY_COUNT + 2];

    memset(ledConfigBuffer, 0, bufferSize);

    char *dptr = directions;
    for (ledDirectionId_e dir = 0; dir < LED_DIRECTION_COUNT; dir++) {
        if (ledGetDirectionBit(ledConfig, dir)) {
            *dptr++ = directionCodes[dir];
        }
    }
    *dptr = 0;

    char *fptr = baseFunctionOverlays;
    *fptr++ = baseFunctionCodes[ledGetFunction(ledConfig)];

    for (ledOverlayId_e ol = 0; ol < LED_OVERLAY_COUNT; ol++) {
        if (ledGetOverlayBit(ledConfig, ol)) {
            *fptr++ = overlayCodes[ol];
        }
    }
    *fptr = 0;

    // TODO - check buffer length
    tfp_sprintf(ledConfigBuffer, "%u,%u:%s:%s:%u", ledGetX(ledConfig), ledGetY(ledConfig), directions, baseFunctionOverlays, ledGetColor(ledConfig));
}

typedef enum {
    // the ordering is important, see below how NSEW is mapped to  NE/SE/NW/SW
    QUADRANT_NORTH      = 1 << 0,
    QUADRANT_SOUTH      = 1 << 1,
    QUADRANT_EAST       = 1 << 2,
    QUADRANT_WEST       = 1 << 3,
} quadrant_e;

static quadrant_e getLedQuadrant(const int ledIndex)
{
	const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];

    int x = ledGetX(ledConfig);
    int y = ledGetY(ledConfig);

    int quad = 0;
    if (y <= highestYValueForNorth)
        quad |= QUADRANT_NORTH;
    else if (y >= lowestYValueForSouth)
        quad |= QUADRANT_SOUTH;
    if (x >= lowestXValueForEast)
        quad |= QUADRANT_EAST;
    else if (x <= highestXValueForWest)
        quad |= QUADRANT_WEST;

    return quad;
}

static hsvColor_t* getDirectionalModeColor(const int ledIndex, const modeColorIndexes_t *modeColors)
{
    const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];
	const int ledDirection = ledGetDirection(ledConfig);

    for (unsigned i = 0; i < LED_DIRECTION_COUNT; i++) {
        if (ledDirection & (1 << i)) {
        	return &ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->colors[modeColors->color[i]];
		}
    }

    return NULL;
}

// map flight mode to led mode, in order of priority
// flightMode == 0 is always active
static const struct {
    uint16_t flightMode;
    uint8_t ledMode;
} flightModeToLed[] = {
    {HEADFREE_MODE, LED_MODE_HEADFREE},
#ifdef USE_MAG
    {MAG_MODE,      LED_MODE_MAG},
#endif
#ifdef USE_BARO
    {BARO_MODE,     LED_MODE_BARO},
#endif
    {HORIZON_MODE,  LED_MODE_HORIZON},
    {ANGLE_MODE,    LED_MODE_ANGLE},
    {0,             LED_MODE_ORIENTATION},
};

static void applyLedFixedLayers(void)
{
    for (int ledIndex = 0; ledIndex < ledCounts.count; ledIndex++) {
        const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];
		hsvColor_t color = *getSC(LED_SCOLOR_BACKGROUND);

        int fn = ledGetFunction(ledConfig);
        int hOffset = HSV_HUE_MAX + 1;

        switch (fn) {
        case LED_FUNCTION_COLOR:
			color = ledStripProfiles(systemConfig()->activeLedStripProfile)->colors[ledGetColor(ledConfig)];

			hsvColor_t nextColor = ledStripProfiles(systemConfig()->activeLedStripProfile)->colors[(ledGetColor(ledConfig) + 1 + LED_CONFIGURABLE_COLOR_COUNT) % LED_CONFIGURABLE_COLOR_COUNT];

			hsvColor_t previousColor = ledStripProfiles(systemConfig()->activeLedStripProfile)->colors[(ledGetColor(ledConfig) - 1 + LED_CONFIGURABLE_COLOR_COUNT) % LED_CONFIGURABLE_COLOR_COUNT];

            if (ledGetOverlayBit(ledConfig, LED_OVERLAY_THROTTLE)) {   //smooth fade with selected Aux channel of all HSV values from previousColor through color to nextColor
                int centerPWM = (PWM_RANGE_MIN + PWM_RANGE_MAX) / 2;
                if (auxInput < centerPWM) {
                    color.h = scaleRange(auxInput, PWM_RANGE_MIN, centerPWM, previousColor.h, color.h);
                    color.s = scaleRange(auxInput, PWM_RANGE_MIN, centerPWM, previousColor.s, color.s);
                    color.v = scaleRange(auxInput, PWM_RANGE_MIN, centerPWM, previousColor.v, color.v);
                } else {
                    color.h = scaleRange(auxInput, centerPWM, PWM_RANGE_MAX, color.h, nextColor.h);
                    color.s = scaleRange(auxInput, centerPWM, PWM_RANGE_MAX, color.s, nextColor.s);
                    color.v = scaleRange(auxInput, centerPWM, PWM_RANGE_MAX, color.v, nextColor.v);
                }
            }

            break;

        case LED_FUNCTION_FLIGHT_MODE:
            for (unsigned i = 0; i < ARRAYLEN(flightModeToLed); i++)
                if (!flightModeToLed[i].flightMode || FLIGHT_MODE(flightModeToLed[i].flightMode)) {
                    const hsvColor_t *directionalColor = getDirectionalModeColor(ledIndex, &ledStripProfiles(systemConfig()->activeLedStripProfile)->modeColors[flightModeToLed[i].ledMode]);
					if (directionalColor) {
                        color = *directionalColor;
                    }

                    break; // stop on first match
                }
            break;

        case LED_FUNCTION_ARM_STATE:
            color = ARMING_FLAG(ARMED) ? *getSC(LED_SCOLOR_ARMED) : *getSC(LED_SCOLOR_DISARMED);
            break;

        case LED_FUNCTION_BATTERY:
            color = HSV(RED);
            hOffset += scaleRange(calculateBatteryPercentageRemaining(), 0, 100, -30, 120);
            break;

        case LED_FUNCTION_RSSI:
            color = HSV(RED);
            hOffset += scaleRange(getRssi() * 100, 0, 1023, -30, 120);
            break;

        default:
            break;
        }

        if ((fn != LED_FUNCTION_COLOR) && ledGetOverlayBit(ledConfig, LED_OVERLAY_THROTTLE)) {
            hOffset += scaleRange(auxInput, PWM_RANGE_MIN, PWM_RANGE_MAX, 0, HSV_HUE_MAX + 1);
        }

        color.h = (color.h + hOffset) % (HSV_HUE_MAX + 1);
        setLedHsv(ledIndex, &color);
    }
}

static void applyLedHsv(uint32_t mask, const hsvColor_t *color)
{
    for (int ledIndex = 0; ledIndex < ledCounts.count; ledIndex++) {
        const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];
		if ((*ledConfig & mask) == mask)
            setLedHsv(ledIndex, color);
    }
}

typedef enum {
    WARNING_ARMING_DISABLED,
    WARNING_LOW_BATTERY,
    WARNING_FAILSAFE,
} warningFlags_e;

static void applyLedWarningLayer(bool updateNow, timeUs_t *timer)
{
    static uint8_t warningFlashCounter = 0;
    static uint8_t warningFlags = 0;          // non-zero during blinks

    if (updateNow) {
        // keep counter running, so it stays in sync with blink
        warningFlashCounter++;
        warningFlashCounter &= 0xF;

        if (warningFlashCounter == 0) {      // update when old flags was processed
            warningFlags = 0;
            if (batteryConfig()->voltageMeterSource != VOLTAGE_METER_NONE && getBatteryState() != BATTERY_OK)
                warningFlags |= 1 << WARNING_LOW_BATTERY;
            if (failsafeIsActive())
                warningFlags |= 1 << WARNING_FAILSAFE;
            if (!ARMING_FLAG(ARMED) && isArmingDisabled())
                warningFlags |= 1 << WARNING_ARMING_DISABLED;
        }
        *timer += HZ_TO_US(10);
    }

    const hsvColor_t *warningColor = NULL;

    if (warningFlags) {
        bool colorOn = (warningFlashCounter % 2) == 0;   // w_w_
        warningFlags_e warningId = warningFlashCounter / 4;
        if (warningFlags & (1 << warningId)) {
            switch (warningId) {
                case WARNING_ARMING_DISABLED:
                    warningColor = colorOn ? &HSV(GREEN)  : &HSV(BLACK);
                    break;
                case WARNING_LOW_BATTERY:
                    warningColor = colorOn ? &HSV(RED)    : &HSV(BLACK);
                    break;
                case WARNING_FAILSAFE:
                    warningColor = colorOn ? &HSV(YELLOW) : &HSV(BLUE);
                    break;
                default:;
            }
        }
    } else {
        if (isBeeperOn()) {
            warningColor = &HSV(ORANGE);
        }
    }

    if (warningColor) {
        applyLedHsv(LED_MOV_OVERLAY(LED_FLAG_OVERLAY(LED_OVERLAY_WARNING)), warningColor);
    }
}

#ifdef USE_VTX_COMMON
static void applyLedVtxLayer(bool updateNow, timeUs_t *timer)
{
    static uint16_t frequency = 0;
    static uint8_t power = 255;
    static uint8_t pit = 255;
    static uint8_t showSettings = false;
    static uint16_t lastCheck = 0;
    static bool blink = false;

    if (!vtxCommonDeviceRegistered()) {
        return;
    }

    uint8_t band = 255, channel = 255;
    uint16_t check = 0;

    if (updateNow) {
        // keep counter running, so it stays in sync with vtx
        vtxCommonGetBandAndChannel(&band, &channel);
        vtxCommonGetPowerIndex(&power);
        vtxCommonGetPitMode(&pit);

        frequency = vtx58frequencyTable[band - 1][channel - 1]; //subtracting 1 from band and channel so that correct frequency is returned.
                                                                //might not be correct for tramp but should fix smart audio.
        // check if last vtx values have changed.
        check = pit + (power << 1) + (band << 4) + (channel << 8);
        if (!showSettings && check != lastCheck) {
            // display settings for 3 seconds.
            showSettings = 15;
        }
        lastCheck = check; // quick way to check if any settings changed.

        if (showSettings) {
            showSettings--;
        }
        blink = !blink;
        *timer += HZ_TO_US(5); // check 5 times a second
    }

    hsvColor_t color = {0, 0, 0};
    if (showSettings) { // show settings
        uint8_t vtxLedCount = 0;
        for (int i = 0; i < ledCounts.count && vtxLedCount < 6; ++i) {
            const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[i];
			if (ledGetOverlayBit(ledConfig, LED_OVERLAY_VTX)) {
                if (vtxLedCount == 0) {
                    color.h = HSV(GREEN).h;
                    color.s = HSV(GREEN).s;
                    color.v = blink ? 15 : 0; // blink received settings
                }
                else if (vtxLedCount > 0 && power >= vtxLedCount && !pit) { // show power
                    color.h = HSV(ORANGE).h;
                    color.s = HSV(ORANGE).s;
                    color.v = blink ? 15 : 0; // blink received settings
                }
                else { // turn rest off
                    color.h = HSV(BLACK).h;
                    color.s = HSV(BLACK).s;
                    color.v = HSV(BLACK).v;
                }
                setLedHsv(i, &color);
                ++vtxLedCount;
            }
        }
    }
    else { // show frequency
        // calculate the VTX color based on frequency
        int colorIndex = 0;
        if (frequency <= 5672) {
            colorIndex = COLOR_WHITE;
        } else if (frequency <= 5711) {
            colorIndex = COLOR_RED;
        } else if (frequency <= 5750) {
            colorIndex = COLOR_ORANGE;
        } else if (frequency <= 5789) {
            colorIndex = COLOR_YELLOW;
        } else if (frequency <= 5829) {
            colorIndex = COLOR_GREEN;
        } else if (frequency <= 5867) {
            colorIndex = COLOR_BLUE;
        } else if (frequency <= 5906) {
            colorIndex = COLOR_DARK_VIOLET;
        } else {
            colorIndex = COLOR_DEEP_PINK;
        }
        // hsvColor_t color = ledStripConfig()->colors[colorIndex];
        hsvColor_t color = ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->colors[colorIndex];
		color.v = pit ? (blink ? 15 : 0) : 255; // blink when in pit mode
        applyLedHsv(LED_MOV_OVERLAY(LED_FLAG_OVERLAY(LED_OVERLAY_VTX)), &color);
    }
}
#endif

static void applyLedBatteryLayer(bool updateNow, timeUs_t *timer)
{
    static bool flash = false;

    int timerDelayUs = HZ_TO_US(1);

    if (updateNow) {

        switch (getBatteryState()) {
            case BATTERY_OK:
                flash = true;
                timerDelayUs = HZ_TO_US(1);

                break;
            case BATTERY_WARNING:
                flash = !flash;
                timerDelayUs = HZ_TO_US(2);

                break;
            default:
                flash = !flash;
                timerDelayUs = HZ_TO_US(8);

                break;
        }
    }

    *timer += timerDelayUs;

    if (!flash) {
       const hsvColor_t *bgc = getSC(LED_SCOLOR_BACKGROUND);
       applyLedHsv(LED_MOV_FUNCTION(LED_FUNCTION_BATTERY), bgc);
    }
}

static void applyLedRssiLayer(bool updateNow, timeUs_t *timer)
{
    static bool flash = false;

    int timerDelay = HZ_TO_US(1);

    if (updateNow) {
        int state = (getRssi() * 100) / 1023;

        if (state > 50) {
            flash = true;
            timerDelay = HZ_TO_US(1);
        } else if (state > 20) {
            flash = !flash;
            timerDelay = HZ_TO_US(2);
        } else {
            flash = !flash;
            timerDelay = HZ_TO_US(8);
        }
    }

    *timer += timerDelay;

    if (!flash) {
        const hsvColor_t *bgc = getSC(LED_SCOLOR_BACKGROUND);
        applyLedHsv(LED_MOV_FUNCTION(LED_FUNCTION_RSSI), bgc);
    }
}

#ifdef USE_GPS
static void applyLedGpsLayer(bool updateNow, timeUs_t *timer)
{

    static uint8_t gpsPauseCounter = 0;
    const uint8_t blinkPauseLength = 4;

    if (updateNow) {
        static uint8_t gpsFlashCounter = 0;
        if (gpsPauseCounter > 0) {
            gpsPauseCounter--;
        } else if (gpsFlashCounter >= gpsSol.numSat) {
            gpsFlashCounter = 0;
            gpsPauseCounter = blinkPauseLength;
        } else {
            gpsFlashCounter++;
            gpsPauseCounter = 1;
        }
        *timer += HZ_TO_US(2.5f);
    }

    const hsvColor_t *gpsColor;

    if (gpsSol.numSat == 0 || !sensors(SENSOR_GPS)) {
        gpsColor = getSC(LED_SCOLOR_GPSNOSATS);
    } else {
        bool colorOn = gpsPauseCounter == 0;  // each interval starts with pause
        if (STATE(GPS_FIX)) {
            gpsColor = colorOn ? getSC(LED_SCOLOR_GPSLOCKED) : getSC(LED_SCOLOR_BACKGROUND);
        } else {
            gpsColor = colorOn ? getSC(LED_SCOLOR_GPSNOLOCK) : getSC(LED_SCOLOR_GPSNOSATS);
        }
    }

    applyLedHsv(LED_MOV_FUNCTION(LED_FUNCTION_GPS), gpsColor);
}

#endif

#define INDICATOR_DEADBAND 25

static void applyLedIndicatorLayer(bool updateNow, timeUs_t *timer)
{
    static bool flash = 0;

    if (updateNow) {
        if (rxIsReceivingSignal()) {
            // calculate update frequency
            int scale = MAX(ABS(rcCommand[ROLL]), ABS(rcCommand[PITCH]));  // 0 - 500
            scale = scale - INDICATOR_DEADBAND;  // start increasing frequency right after deadband
            *timer += HZ_TO_US(5 + (45 * scale) / (500 - INDICATOR_DEADBAND));   // 5 - 50Hz update, 2.5 - 25Hz blink

            flash = !flash;
        } else {
            *timer += HZ_TO_US(5);
        }
    }

    if (!flash)
        return;

    const hsvColor_t *flashColor = &HSV(ORANGE); // TODO - use user color?

    quadrant_e quadrants = 0;
    if (rcCommand[ROLL] > INDICATOR_DEADBAND) {
        quadrants |= QUADRANT_EAST;
    } else if (rcCommand[ROLL] < -INDICATOR_DEADBAND) {
        quadrants |= QUADRANT_WEST;
    }
    if (rcCommand[PITCH] > INDICATOR_DEADBAND) {
        quadrants |= QUADRANT_NORTH;
    } else if (rcCommand[PITCH] < -INDICATOR_DEADBAND) {
        quadrants |= QUADRANT_SOUTH;
    }

    for (int ledIndex = 0; ledIndex < ledCounts.count; ledIndex++) {
        const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];
		if (ledGetOverlayBit(ledConfig, LED_OVERLAY_INDICATOR)) {
            if (getLedQuadrant(ledIndex) & quadrants)
                setLedHsv(ledIndex, flashColor);
        }
    }
}

#define ROTATION_SEQUENCE_LED_COUNT 6 // 2 on, 4 off
#define ROTATION_SEQUENCE_LED_WIDTH 2 // 2 on

static void updateLedRingCounts(void)
{
    int seqLen;
    // try to split in segments/rings of exactly ROTATION_SEQUENCE_LED_COUNT leds
    if ((ledCounts.ring % ROTATION_SEQUENCE_LED_COUNT) == 0) {
        seqLen = ROTATION_SEQUENCE_LED_COUNT;
    } else {
        seqLen = ledCounts.ring;
        // else split up in equal segments/rings of at most ROTATION_SEQUENCE_LED_COUNT leds
        // TODO - improve partitioning (15 leds -> 3x5)
        while ((seqLen > ROTATION_SEQUENCE_LED_COUNT) && ((seqLen % 2) == 0)) {
            seqLen /= 2;
        }
    }
    ledCounts.ringSeqLen = seqLen;
}

static void applyLedThrustRingLayer(bool updateNow, timeUs_t *timer)
{
    static uint8_t rotationPhase;
    int ledRingIndex = 0;

    if (updateNow) {
        rotationPhase = rotationPhase > 0 ? rotationPhase - 1 : ledCounts.ringSeqLen - 1;

        *timer += HZ_TO_US(5 + (45 * scaledThrottle) / 100);  // 5 - 50Hz update rate
    }

    for (int ledIndex = 0; ledIndex < ledCounts.count; ledIndex++) {
        // const ledConfig_t *ledConfig = &ledStripConfig()->ledConfigs[ledIndex];
        const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];
		if (ledGetFunction(ledConfig) == LED_FUNCTION_THRUST_RING) {

            bool applyColor;
            if (ARMING_FLAG(ARMED)) {
                applyColor = (ledRingIndex + rotationPhase) % ledCounts.ringSeqLen < ROTATION_SEQUENCE_LED_WIDTH;
            } else {
                applyColor = !(ledRingIndex % 2); // alternating pattern
            }

            if (applyColor) {
                const hsvColor_t *ringColor = &ledStripProfiles(systemConfig()->activeLedStripProfile)->colors[ledGetColor(ledConfig)];
				setLedHsv(ledIndex, ringColor);
            }

            ledRingIndex++;
        }
    }
}

typedef struct larsonParameters_s {
    uint8_t currentBrightness;
    int8_t currentIndex;
    int8_t direction;
} larsonParameters_t;

static int brightnessForLarsonIndex(larsonParameters_t *larsonParameters, uint8_t larsonIndex)
{
    int offset = larsonIndex - larsonParameters->currentIndex;
    static const int larsonLowValue = 8;

    if (ABS(offset) > 1)
        return (larsonLowValue);

    if (offset == 0)
        return (larsonParameters->currentBrightness);

    if (larsonParameters->direction == offset) {
        return (larsonParameters->currentBrightness - 127);
    }

    return (255 - larsonParameters->currentBrightness);

}

static void larsonScannerNextStep(larsonParameters_t *larsonParameters, int delta)
{
    if (larsonParameters->currentBrightness > (255 - delta)) {
        larsonParameters->currentBrightness = 127;
        if (larsonParameters->currentIndex >= ledCounts.larson || larsonParameters->currentIndex < 0) {
            larsonParameters->direction = -larsonParameters->direction;
        }
        larsonParameters->currentIndex += larsonParameters->direction;
    } else {
        larsonParameters->currentBrightness += delta;
    }
}

static void applyLarsonScannerLayer(bool updateNow, timeUs_t *timer)
{
    static larsonParameters_t larsonParameters = { 0, 0, 1 };

    if (updateNow) {
        larsonScannerNextStep(&larsonParameters, 15);
        *timer += HZ_TO_US(60);
    }

    int scannerLedIndex = 0;
    for (unsigned i = 0; i < ledCounts.count; i++) {

		const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[i];

        if (ledGetOverlayBit(ledConfig, LED_OVERLAY_LARSON_SCANNER)) {
            hsvColor_t ledColor;
            getLedHsv(i, &ledColor);
            ledColor.v = brightnessForLarsonIndex(&larsonParameters, scannerLedIndex);
            setLedHsv(i, &ledColor);
            scannerLedIndex++;
        }
    }
}

// blink twice, then wait ; either always or just when landing
static void applyLedBlinkLayer(bool updateNow, timeUs_t *timer)
{
    const uint16_t blinkPattern = 0x8005; // 0b1000000000000101;
    static uint16_t blinkMask;

    if (updateNow) {
        blinkMask = blinkMask >> 1;
        if (blinkMask <= 1)
            blinkMask = blinkPattern;

        *timer += HZ_TO_US(10);
    }

    bool ledOn = (blinkMask & 1);  // b_b_____...
    if (!ledOn) {
        for (int i = 0; i < ledCounts.count; ++i) {
			const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[i];
            if (ledGetOverlayBit(ledConfig, LED_OVERLAY_BLINK)) {
                setLedHsv(i, getSC(LED_SCOLOR_BLINKBACKGROUND));
            }
        }
    }
}

#ifdef USE_LED_ANIMATION
static void applyLedAnimationLayer(bool updateNow, timeUs_t *timer)
{
    static uint8_t frameCounter = 0;
    const int animationFrames = ledGridRows;
    if (updateNow) {
        frameCounter = (frameCounter + 1 < animationFrames) ? frameCounter + 1 : 0;
        *timer += HZ_TO_US(20);
    }

    if (ARMING_FLAG(ARMED))
        return;

    int previousRow = frameCounter > 0 ? frameCounter - 1 : animationFrames - 1;
    int currentRow = frameCounter;
    int nextRow = (frameCounter + 1 < animationFrames) ? frameCounter + 1 : 0;

    for (int ledIndex = 0; ledIndex < ledCounts.count; ledIndex++) {
		const ledConfig_t *ledConfig = &ledStripProfiles(systemConfig()->activeLedStripProfile)->ledConfigs[ledIndex];

        if (ledGetY(ledConfig) == previousRow) {
            setLedHsv(ledIndex, getSC(LED_SCOLOR_ANIMATION));
            scaleLedValue(ledIndex, 50);
        } else if (ledGetY(ledConfig) == currentRow) {
            setLedHsv(ledIndex, getSC(LED_SCOLOR_ANIMATION));
        } else if (ledGetY(ledConfig) == nextRow) {
            scaleLedValue(ledIndex, 50);
        }
    }
}
#endif

typedef enum {
    timBlink,
    timLarson,
    timBattery,
    timRssi,
#ifdef USE_GPS
    timGps,
#endif
    timWarning,
#ifdef USE_VTX_COMMON
    timVtx,
#endif
    timIndicator,
#ifdef USE_LED_ANIMATION
    timAnimation,
#endif
    timRing,
    timTimerCount
} timId_e;

static timeUs_t timerVal[timTimerCount];

// function to apply layer.
// function must replan self using timer pointer
// when updateNow is true (timer triggered), state must be updated first,
//  before calculating led state. Otherwise update started by different trigger
//  may modify LED state.
typedef void applyLayerFn_timed(bool updateNow, timeUs_t *timer);

static applyLayerFn_timed* layerTable[] = {
    [timBlink] = &applyLedBlinkLayer,
    [timLarson] = &applyLarsonScannerLayer,
    [timBattery] = &applyLedBatteryLayer,
    [timRssi] = &applyLedRssiLayer,
#ifdef USE_GPS
    [timGps] = &applyLedGpsLayer,
#endif
    [timWarning] = &applyLedWarningLayer,
#ifdef USE_VTX_COMMON
    [timVtx] = &applyLedVtxLayer,
#endif
    [timIndicator] = &applyLedIndicatorLayer,
#ifdef USE_LED_ANIMATION
    [timAnimation] = &applyLedAnimationLayer,
#endif
    [timRing] = &applyLedThrustRingLayer
};

void ledStripUpdate(timeUs_t currentTimeUs)
{
    if (!(ledStripInitialised && isWS2811LedStripReady())) {
        return;
    }

   	if (IS_RC_MODE_ACTIVE(BOXLEDLOW) && !(ledStripProfiles(systemConfig()->activeLedStripProfile)->ledstrip_visual_beeper && isBeeperOn())) { 
		if (ledStripEnabled) {
            ledStripDisable();
            ledStripEnabled = false;
        }
        return;
    }
    ledStripEnabled = true;

    const uint32_t now = currentTimeUs;

    // test all led timers, setting corresponding bits
    uint32_t timActive = 0;
    for (timId_e timId = 0; timId < timTimerCount; timId++) {
        // sanitize timer value, so that it can be safely incremented. Handles inital timerVal value.
        const timeDelta_t delta = cmpTimeUs(now, timerVal[timId]);
        // max delay is limited to 5s
        if (delta < 0 && delta > -MAX_TIMER_DELAY)
            continue;  // not ready yet
        timActive |= 1 << timId;
        if (delta >= 100 * 1000 || delta < 0) {
            timerVal[timId] = now;
        }
    }

    if (!timActive)
        return;          // no change this update, keep old state

    // apply all layers; triggered timed functions has to update timers

    scaledThrottle = ARMING_FLAG(ARMED) ? scaleRange(rcData[THROTTLE], PWM_RANGE_MIN, PWM_RANGE_MAX, 0, 100) : 0;
	auxInput = rcData[ledStripProfiles(systemConfig()->activeLedStripProfile)->ledstrip_aux_channel];

    applyLedFixedLayers();

    for (timId_e timId = 0; timId < ARRAYLEN(layerTable); timId++) {
        uint32_t *timer = &timerVal[timId];
        bool updateNow = timActive & (1 << timId);
        (*layerTable[timId])(updateNow, timer);
    }
    ws2811UpdateStrip();
}

bool parseColor(int index, const char *colorConfig)
{
    const char *remainingCharacters = colorConfig;

	hsvColor_t *color = &ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->colors[index];

    bool result = true;
    static const uint16_t hsv_limit[HSV_COLOR_COMPONENT_COUNT] = {
        [HSV_HUE] = HSV_HUE_MAX,
        [HSV_SATURATION] = HSV_SATURATION_MAX,
        [HSV_VALUE] = HSV_VALUE_MAX,
    };
    for (int componentIndex = 0; result && componentIndex < HSV_COLOR_COMPONENT_COUNT; componentIndex++) {
        int val = atoi(remainingCharacters);
        if (val > hsv_limit[componentIndex]) {
            result = false;
            break;
        }
        switch (componentIndex) {
            case HSV_HUE:
                color->h = val;
                break;
            case HSV_SATURATION:
                color->s = val;
                break;
            case HSV_VALUE:
                color->v = val;
                break;
        }
        remainingCharacters = strchr(remainingCharacters, ',');
        if (remainingCharacters) {
            remainingCharacters++;  // skip separator
        } else {
            if (componentIndex < HSV_COLOR_COMPONENT_COUNT - 1) {
                result = false;
            }
        }
    }

    if (!result) {
        memset(color, 0, sizeof(*color));
    }

    return result;
}

/*
 * Redefine a color in a mode.
 * */
bool setModeColor(ledModeIndex_e modeIndex, int modeColorIndex, int colorIndex)
{
    // check color
    if (colorIndex < 0 || colorIndex >= LED_CONFIGURABLE_COLOR_COUNT)
        return false;
    if (modeIndex < LED_MODE_COUNT) {  // modeIndex_e is unsigned, so one-sided test is enough
        if (modeColorIndex < 0 || modeColorIndex >= LED_DIRECTION_COUNT)
            return false;
    	ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->modeColors[modeIndex].color[modeColorIndex] = colorIndex;
	} else if (modeIndex == LED_SPECIAL) {
        if (modeColorIndex < 0 || modeColorIndex >= LED_SPECIAL_COLOR_COUNT)
            return false;
    	ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->specialColors.color[modeColorIndex] = colorIndex;
	} else if (modeIndex == LED_AUX_CHANNEL) {
        if (modeColorIndex < 0 || modeColorIndex >= 1)
            return false;
    	ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->ledstrip_aux_channel = colorIndex;
	} else {
        return false;
    }
    return true;
}

void ledStripInit(void)
{
    colors = ledStripProfilesMutable(systemConfig()->activeLedStripProfile)->colors;
    modeColors = ledStripProfiles(systemConfig()->activeLedStripProfile)->modeColors;
    specialColors = ledStripProfiles(systemConfig()->activeLedStripProfile)->specialColors;
	ledStripInitialised = false;
}

void ledStripEnable(void)
{
    reevaluateLedConfig();
    ledStripInitialised = true;

	ws2811LedStripInit(ledStripProfiles(systemConfig()->activeLedStripProfile)->ioTag);
}

static void ledStripDisable(void)
{
    setStripColor(&HSV(BLACK));

    ws2811UpdateStrip();
}
#endif

