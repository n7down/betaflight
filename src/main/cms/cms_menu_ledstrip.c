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
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "platform.h"

#ifdef USE_CMS

#include "build/version.h"

#include "cms/cms.h"
#include "cms/cms_types.h"
#include "cms/cms_menu_ledstrip.h"

#include "config/feature.h"
#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "fc/config.h"

#include "io/ledstrip.h"

#ifdef USE_LED_STRIP

static uint8_t ledProfile;

static bool featureRead = false;
static uint8_t cmsx_FeatureLedstrip;

static long applyLedProfile(displayPort_t *pDisp, const void *self)
{
	UNUSED(pDisp);
	UNUSED(self);

	switch(ledProfile) {
		case 0: 
			setProfile(DEFAULT);
			break;
		case 1: 
			setProfile(WHITE);
			break;
		case 2:
			setProfile(RED);
			break;
		default:
			setProfile(DEFAULT);
			break;
	}
	return 0;
}

static long cmsx_Ledstrip_FeatureRead(void)
{
    if (!featureRead) {
        cmsx_FeatureLedstrip = feature(FEATURE_LED_STRIP) ? 1 : 0;
        featureRead = true;
    }
    return 0;
}

static long cmsx_Ledstrip_FeatureWriteback(const OSD_Entry *self)
{
    UNUSED(self);
    if (featureRead) {
        if (cmsx_FeatureLedstrip)
            featureSet(FEATURE_LED_STRIP);
        else
            featureClear(FEATURE_LED_STRIP);
    }

    return 0;
}

static const char * const LED_PROFILE_NAMES[] = {
	"DEFAULT ",
	"WHITE   ",
	"RED     "
};

static OSD_TAB_t entryLed = {&ledProfile, 2, LED_PROFILE_NAMES};

static OSD_Entry cmsx_menuLedstripEntries[] =
{
    { "-- LED STRIP --", OME_Label, NULL, NULL, 0 },
    { "ENABLED",         OME_Bool,  NULL, &cmsx_FeatureLedstrip, 0 },
	{ "LED PROFILE", 	 OME_TAB,	applyLedProfile, &entryLed, 0 },
    { "BACK", OME_Back, NULL, NULL, 0 },
    { NULL, OME_END, NULL, NULL, 0 }
};

CMS_Menu cmsx_menuLedstrip = {
#ifdef CMS_MENU_DEBUG
    .GUARD_text = "MENULED",
    .GUARD_type = OME_MENU,
#endif
    .onEnter = cmsx_Ledstrip_FeatureRead,
    .onExit = cmsx_Ledstrip_FeatureWriteback,
    .entries = cmsx_menuLedstripEntries
};
#endif // LED_STRIP
#endif // CMS
