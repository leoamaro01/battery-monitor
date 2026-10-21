// The whole UI: three swipeable pages plus screen sleep. Call ui_init() once after LVGL
// and the display are set up; it refreshes itself from g_state on an LVGL timer.
#pragma once

void ui_init();
// Jump to a page (0 = home, 1 = pack, 2 = batteries) without animation.
void ui_show_page(int index);
