#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#ifndef COLLECTOR_UI_HEAP_KIB
#define COLLECTOR_UI_HEAP_KIB 24
#endif
#define LV_MEM_SIZE (COLLECTOR_UI_HEAP_KIB * 1024U)
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#endif
void collector_ui_assert_fail(void);
#ifdef __cplusplus
}
#endif
#endif
#define LV_ASSERT_HANDLER collector_ui_assert_fail();
#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_LOG 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
