#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PHOSH_TYPE_HOME_ASSISTANT_QUICK_SETTING_PREFS phosh_home_assistant_quick_setting_prefs_get_type ()

G_DECLARE_FINAL_TYPE (PhoshHomeAssistantQuickSettingPrefs,
                      phosh_home_assistant_quick_setting_prefs,
                      PHOSH, HOME_ASSISTANT_QUICK_SETTING_PREFS,
                      AdwPreferencesDialog)

G_END_DECLS
