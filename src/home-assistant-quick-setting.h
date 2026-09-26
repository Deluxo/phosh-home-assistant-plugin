#pragma once

#include "quick-setting.h"

G_BEGIN_DECLS

#define PHOSH_TYPE_HOME_ASSISTANT_QUICK_SETTING phosh_home_assistant_quick_setting_get_type ()

G_DECLARE_FINAL_TYPE (PhoshHomeAssistantQuickSetting,
                      phosh_home_assistant_quick_setting,
                      PHOSH, HOME_ASSISTANT_QUICK_SETTING, PhoshQuickSetting)

G_END_DECLS
