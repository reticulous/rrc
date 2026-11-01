/**
 * rrc_app.h — the on-device RRC app as a boot-registered Service.
 *
 * RrcApp is an LcdApp: the straddle's `services:` entry points the generated
 * boot code at this header. Methods are defined in rrc_lcd.cpp, where the
 * file-static UI state lives. Compiled only under conditional/spangap-lcd/.
 *
 * The app is a view over the rrc task's storage and a writer of its command
 * keys: the rooms list (Rooms / Hubs tabs), a hub's screen, a room's log with
 * its composer, the room's info page and a member's card. appInit() (boot
 * task) subscribes rrc.url_lcd on the lcd task, so another app can open a hub
 * or a room whether or not RRC was ever opened.
 */
#pragma once

#include "lcd_app.h"
#include "lvgl.h"

class RrcApp : public LcdApp {
public:
    RrcApp();
    void onCreate(lv_obj_t* root) override;
    /** Back to the screen the current one was opened from (a dialog first);
     *  the rooms list falls through to Home. */
    bool onBack() override;
    /** A hub screen in the foreground asks the node to measure the round
     *  trip; going to the background stops that and keeps the composer's
     *  text. */
    void onShow() override;
    void onHide() override;
    /** Drops the open room's log subscription and the hub watch, nulls every
     *  widget handle (the shared subscriptions outlive the layer) and deletes
     *  pending timers. */
    void onClose() override;

protected:
    void appInit() override;
};
