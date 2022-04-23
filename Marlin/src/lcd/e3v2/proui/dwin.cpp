/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2021 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * DWIN Enhanced implementation for PRO UI
 * Author: Miguel A. Risco-Castillo (MRISCOC)
 * Version: 3.15.2
 * Date: 2022/03/01
 */

#include "../../../inc/MarlinConfig.h"

#if ENABLED(DWIN_LCD_PROUI)

#include "dwin.h"
#include "menus.h"
#include "dwin_popup.h"

#include "../../fontutils.h"
#include "../../marlinui.h"

#include "../../../sd/cardreader.h"

#include "../../../MarlinCore.h"
#include "../../../core/serial.h"
#include "../../../core/macros.h"

#include "../../../module/temperature.h"
#include "../../../module/printcounter.h"
#include "../../../module/motion.h"
#include "../../../module/planner.h"

#include "../../../gcode/gcode.h"
#include "../../../gcode/queue.h"

#if HAS_FILAMENT_SENSOR
  #include "../../../feature/runout.h"
#endif

#if ENABLED(EEPROM_SETTINGS)
  #include "../../../module/settings.h"
#endif

#if ENABLED(HOST_ACTION_COMMANDS)
  #include "../../../feature/host_actions.h"
#endif

#if ANY(AUTO_BED_LEVELING_BILINEAR, AUTO_BED_LEVELING_LINEAR, AUTO_BED_LEVELING_3POINT) && DISABLED(PROBE_MANUALLY)
  #define HAS_ONESTEP_LEVELING 1
#endif

#if HAS_MESH || HAS_ONESTEP_LEVELING
  #include "../../../feature/bedlevel/bedlevel.h"
#endif

#if HAS_BED_PROBE
  #include "../../../module/probe.h"
#endif

#ifdef BLTOUCH_HS_MODE
  #include "../../../feature/bltouch.h"
#endif

#if ANY(BABYSTEPPING, HAS_BED_PROBE, HAS_WORKSPACE_OFFSET)
  #define HAS_ZOFFSET_ITEM 1
  #if !HAS_BED_PROBE && ENABLED(BABYSTEPPING)
    #define JUST_BABYSTEP 1
  #endif
  #if EITHER(BABYSTEP_ZPROBE_OFFSET, JUST_BABYSTEP)
    #include "../../../feature/babystep.h"
  #endif
#endif

#if ENABLED(POWER_LOSS_RECOVERY)
  #include "../../../feature/powerloss.h"
#endif

#if HAS_GCODE_PREVIEW
  #include "gcode_preview.h"
#endif

#if HAS_ESDIAG
  #include "endstop_diag.h"
#endif

#if HAS_MESH
  #include "meshviewer.h"
#endif

#if ENABLED(PRINTCOUNTER)
  #include "printstats.h"
#endif

#if ENABLED(CASE_LIGHT_MENU)
  #include "../../../feature/caselight.h"
#endif

#if ENABLED(LED_CONTROL_MENU)
  #include "../../../feature/leds/leds.h"
#endif

#include <WString.h>
#include <stdio.h>
#include <string.h>

#ifndef MACHINE_SIZE
  #define MACHINE_SIZE STRINGIFY(X_BED_SIZE) "x" STRINGIFY(Y_BED_SIZE) "x" STRINGIFY(Z_MAX_POS)
#endif

#include "lockscreen.h"

#define PAUSE_HEAT

#define MENU_CHAR_LIMIT  24

// Print speed limit
#define MIN_PRINT_SPEED  10
#define MAX_PRINT_SPEED 999

// Print flow limit
#define MIN_PRINT_FLOW   10
#define MAX_PRINT_FLOW   299

// Load and Unload limits
#define MAX_LOAD_UNLOAD  500

// Feedspeed limit (max feedspeed = DEFAULT_MAX_FEEDRATE * 2)
#define MIN_MAXFEEDSPEED      1
#define MIN_MAXACCELERATION   1
#define MIN_MAXJERK           0.1
#define MIN_STEP              1
#define MAX_STEP              999.9

// Extruder's temperature limits
#define MIN_ETEMP  HEATER_0_MINTEMP
#define MAX_ETEMP  (HEATER_0_MAXTEMP - HOTEND_OVERSHOOT)

#define FEEDRATE_E      (60)

// Minimum unit (0.1) : multiple (10)
#define UNITFDIGITS 1
#define MINUNITMULT POW(10, UNITFDIGITS)

#define ENCODER_WAIT_MS                  20
#define DWIN_VAR_UPDATE_INTERVAL         1024
#define DWIN_SCROLL_UPDATE_INTERVAL      SEC_TO_MS(2)
#define DWIN_REMAIN_TIME_UPDATE_INTERVAL SEC_TO_MS(20)

#define BABY_Z_VAR TERN(HAS_BED_PROBE, probe.offset.z, dwin_zoffset)

// Structs
HMI_value_t HMI_value;
HMI_flag_t HMI_flag{0};
HMI_data_t HMI_data;

millis_t dwin_heat_time = 0;

uint8_t checkkey = 255, last_checkkey = MainMenu;

enum SelectItem : uint8_t {
  PAGE_PRINT = 0,
  PAGE_PREPARE,
  PAGE_CONTROL,
  PAGE_ADVANCE,
  PAGE_COUNT,

  PRINT_SETUP = 0,
  PRINT_PAUSE_RESUME,
  PRINT_STOP,
  PRINT_COUNT
};

typedef struct {
  uint8_t now, last;
  void set(uint8_t v) { now = last = v; }
  void reset() { set(0); }
  bool changed() { bool c = (now != last); if (c) last = now; return c; }
  bool dec() { if (now) now--; return changed(); }
  bool inc(uint8_t v) { if (now < (v - 1)) now++; else now = (v - 1); return changed(); }
} select_t;

select_t select_page{0}, select_file{0}, select_print{0};
uint8_t index_file     = MROWS;

bool hash_changed = true; // Flag to know if message status was changed

constexpr float max_feedrate_edit_values[]        = DEFAULT_MAX_FEEDRATE;
constexpr float max_acceleration_edit_values[]    = DEFAULT_MAX_ACCELERATION;

#if HAS_CLASSIC_JERK
  constexpr float max_jerk_edit_values[]          = { DEFAULT_XJERK, DEFAULT_YJERK, DEFAULT_ZJERK, DEFAULT_EJERK };
#endif

static uint8_t _percent_done = 0;
static uint32_t _remain_time = 0;

// Additional Aux Host Support
static bool sdprint = false;

#if HAS_ZOFFSET_ITEM
  float dwin_zoffset = 0, last_zoffset = 0;
#endif

#if HAS_HOTEND
  float last_E = 0;
#endif

// New menu system pointers
MenuClass *PrepareMenu = nullptr;
MenuClass *TrammingMenu = nullptr;
MenuClass *MoveMenu = nullptr;
MenuClass *ControlMenu = nullptr;
MenuClass *AdvancedSettings = nullptr;
#if HAS_HOME_OFFSET
  MenuClass *HomeOffMenu = nullptr;
#endif
#if HAS_BED_PROBE
  MenuClass *ProbeSetMenu = nullptr;
#endif
MenuClass *FilSetMenu = nullptr;
MenuClass *SelectColorMenu = nullptr;
MenuClass *GetColorMenu = nullptr;
MenuClass *TuneMenu = nullptr;
MenuClass *MotionMenu = nullptr;
MenuClass *FilamentMenu = nullptr;
#if ENABLED(MESH_BED_LEVELING)
  MenuClass *ManualMesh = nullptr;
#endif
#if HAS_HOTEND
  MenuClass *PreheatMenu = nullptr;
#endif
MenuClass *TemperatureMenu = nullptr;
MenuClass *MaxSpeedMenu = nullptr;
MenuClass *MaxAccelMenu = nullptr;
MenuClass *MaxJerkMenu = nullptr;
MenuClass *StepsMenu = nullptr;
MenuClass *HotendPIDMenu = nullptr;
MenuClass *BedPIDMenu = nullptr;
#if ENABLED(CASELIGHT_USES_BRIGHTNESS)
  MenuClass *CaseLightMenu = nullptr;
#endif
#if ENABLED(LED_CONTROL_MENU)
  MenuClass *LedControlMenu = nullptr;
#endif
#if HAS_BED_PROBE
  MenuClass *ZOffsetWizMenu = nullptr;
#endif
#if ENABLED(INDIVIDUAL_AXIS_HOMING_SUBMENU)
  MenuClass *HomingMenu = nullptr;
#endif

// Updatable menuitems pointers
MenuItemClass *HotendTargetItem = nullptr;
MenuItemClass *BedTargetItem = nullptr;
MenuItemClass *FanSpeedItem = nullptr;
MenuItemClass *MMeshMoveZItem = nullptr;

#define DWIN_LANGUAGE_EEPROM_ADDRESS 0x01   // Between 0x01 and 0x63 (EEPROM_OFFSET-1)
                                            // BL24CXX::check() uses 0x00

inline bool HMI_IsChinese() { return HMI_flag.language == DWIN_CHINESE; }

void HMI_SetLanguageCache() {
  DWIN_JPG_CacheTo1(HMI_IsChinese() ? Language_Chinese : Language_English);
}

void HMI_SetLanguage() {
  #if BOTH(EEPROM_SETTINGS, IIC_BL24CXX_EEPROM)
    BL24CXX::read(DWIN_LANGUAGE_EEPROM_ADDRESS, (uint8_t*)&HMI_flag.language, sizeof(HMI_flag.language));
  #endif
  HMI_SetLanguageCache();
}

void HMI_ToggleLanguage() {
  HMI_flag.language = HMI_IsChinese() ? DWIN_ENGLISH : DWIN_CHINESE;
  HMI_SetLanguageCache();
  #if BOTH(EEPROM_SETTINGS, IIC_BL24CXX_EEPROM)
    BL24CXX::write(DWIN_LANGUAGE_EEPROM_ADDRESS, (uint8_t*)&HMI_flag.language, sizeof(HMI_flag.language));
  #endif
}

//-----------------------------------------------------------------------------
// Main Buttons
//-----------------------------------------------------------------------------

typedef struct { uint16_t x, y[2], w, h; } text_info_t;

void ICON_Button(const bool selected, const int iconid, const frame_rect_t &ico, const text_info_t (&txt), FSTR_P caption) {
  DWIN_ICON_Show(true, false, false, ICON, iconid + selected, ico.x, ico.y);
  if (selected) DWINUI::Draw_Box(0, HMI_data.Highlight_Color, ico);
  if (HMI_IsChinese()) {
    DWIN_Frame_AreaCopy(1, txt.x, txt.y[selected], txt.x + txt.w - 1, txt.y[selected] + txt.h - 1, ico.x + (ico.w - txt.w) / 2, (ico.y + ico.h - 25) - txt.h/2);
  }
  else {
    const uint16_t x = ico.x + (ico.w - strlen_P(FTOP(caption)) * DWINUI::fontWidth()) / 2,
                   y = (ico.y + ico.h - 20) - DWINUI::fontHeight() / 2;
    DWINUI::Draw_String(x, y, caption);
  }
}

//
// Main Menu: "Print"
//
void ICON_Print() {
  constexpr frame_rect_t ico = { 17, 110, 110, 100 };
  constexpr text_info_t txt = { 1, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_page.now == PAGE_PRINT, ICON_Print_0, ico, txt, GET_TEXT_F(MSG_BUTTON_PRINT));
}

//
// Main Menu: "Prepare"
//
void ICON_Prepare() {
  constexpr frame_rect_t ico = { 145, 110, 110, 100 };
  constexpr text_info_t txt = { 31, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_page.now == PAGE_PREPARE, ICON_Prepare_0, ico, txt, GET_TEXT_F(MSG_PREPARE));
}

//
// Main Menu: "Control"
//
void ICON_Control() {
  constexpr frame_rect_t ico = { 17, 226, 110, 100 };
  constexpr text_info_t txt = { 61, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_page.now == PAGE_CONTROL, ICON_Control_0, ico, txt, GET_TEXT_F(MSG_CONTROL));
}

//
// Main Menu: "Advanced Settings"
//
void ICON_AdvSettings() {
  constexpr frame_rect_t ico = { 145, 226, 110, 100 };
  constexpr text_info_t txt = { 91, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_page.now == PAGE_ADVANCE, ICON_Info_0, ico, txt, GET_TEXT_F(MSG_BUTTON_ADVANCED));
}

//
// Printing: "Tune"
//
void ICON_Tune() {
  constexpr frame_rect_t ico = { 8, 232, 80, 100 };
  constexpr text_info_t txt = { 121, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_print.now == PRINT_SETUP, ICON_Setup_0, ico, txt, GET_TEXT_F(MSG_TUNE));
}

//
// Printing: "Pause"
//
void ICON_Pause() {
  constexpr frame_rect_t ico = { 96, 232, 80, 100 };
  constexpr text_info_t txt = { 181, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_print.now == PRINT_PAUSE_RESUME, ICON_Pause_0, ico, txt, GET_TEXT_F(MSG_BUTTON_PAUSE));
}

//
// Printing: "Resume"
//
void ICON_Resume() {
  constexpr frame_rect_t ico = { 96, 232, 80, 100 };
  constexpr text_info_t txt = { 1, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 15 };
  ICON_Button(select_print.now == PRINT_PAUSE_RESUME, ICON_Continue_0, ico, txt, GET_TEXT_F(MSG_BUTTON_RESUME));
}

//
// Printing: "Stop"
//
void ICON_Stop() {
  constexpr frame_rect_t ico = { 184, 232, 80, 100 };
  constexpr text_info_t txt = { 151, { 405, TERN(USE_STOCK_DWIN_SET, 446, 447) }, 27, 12 };
  ICON_Button(select_print.now == PRINT_STOP, ICON_Stop_0, ico, txt, GET_TEXT_F(MSG_BUTTON_STOP));
}

//-----------------------------------------------------------------------------
// Drawing routines
//-----------------------------------------------------------------------------

void Move_Highlight(const int8_t from, const int8_t newline) {
  Erase_Menu_Cursor(newline - from);
  Draw_Menu_Cursor(newline);
}

void Add_Menu_Line() {
  Move_Highlight(1, MROWS);
  DWIN_Draw_HLine(HMI_data.SplitLine_Color, 16, MYPOS(MROWS + 1), 240);
}

void Scroll_Menu(const uint8_t dir) {
  DWIN_Frame_AreaMove(1, dir, MLINE, HMI_data.Background_Color, 0, 31, DWIN_WIDTH, 349);
  switch (dir) {
    case DWIN_SCROLL_DOWN: Move_Highlight(-1, 0); break;
    case DWIN_SCROLL_UP:   Add_Menu_Line(); break;
  }
}

inline uint16_t nr_sd_menu_items() {
  return card.get_num_Files() + !card.flag.workDirIsRoot;
}

void Erase_Menu_Text(const uint8_t line) {
  DWIN_Draw_Rectangle(1, HMI_data.Background_Color, LBLX, MBASE(line) - 14, 271, MBASE(line) + 28);
}

// Draw "Back" line at the top
void Draw_Back_First(const bool is_sel=true) {
  Draw_Menu_Line(0, ICON_Back);
  if (HMI_IsChinese())
    DWIN_Frame_AreaCopy(1, 129, 72, 156, 84, LBLX, MBASE(0));
  else
    DWINUI::Draw_String(LBLX, MBASE(0), GET_TEXT_F(MSG_BACK));
  if (is_sel) Draw_Menu_Cursor(0);
}

//PopUps
void Popup_window_PauseOrStop() {
  if (HMI_IsChinese()) {
    DWINUI::ClearMenuArea();
    Draw_Popup_Bkgd();
         if (select_print.now == PRINT_PAUSE_RESUME) DWIN_Frame_AreaCopy(1, 237, 338, 269, 356, 98, 150);
    else if (select_print.now == PRINT_STOP) DWIN_Frame_AreaCopy(1, 221, 320, 253, 336, 98, 150);
    DWIN_Frame_AreaCopy(1, 220, 304, 264, 319, 130, 150);
    DWINUI::Draw_IconWB(ICON_Confirm_C, 26, 280);
    DWINUI::Draw_IconWB(ICON_Cancel_C, 146, 280);
    Draw_Select_Highlight(true);
    DWIN_UpdateLCD();
  }
  else
    DWIN_Popup_ConfirmCancel(ICON_BLTouch, select_print.now == PRINT_PAUSE_RESUME ? GET_TEXT_F(MSG_PAUSE_PRINT) : GET_TEXT_F(MSG_STOP_PRINT));
}

#if HAS_HOTEND

  void Popup_Window_ETempTooLow() {
    if (HMI_IsChinese()) {
      HMI_SaveProcessID(WaitResponse);
      DWINUI::ClearMenuArea();
      Draw_Popup_Bkgd();
      DWINUI::Draw_Icon(ICON_TempTooLow, 102, 105);
      DWIN_Frame_AreaCopy(1, 103, 371, 136, 386,  69, 240);
      DWIN_Frame_AreaCopy(1, 170, 371, 270, 386, 102, 240);
      DWINUI::Draw_IconWB(ICON_Confirm_C, 86, 280);
      DWIN_UpdateLCD();
    }
    else
      DWIN_Popup_Confirm(ICON_TempTooLow, GET_TEXT_F(MSG_HOTEND_TOO_COLD), GET_TEXT_F(MSG_PLEASE_PREHEAT));
  }

#endif

#if HAS_HOTEND || HAS_HEATED_BED
  void DWIN_Popup_Temperature(const bool toohigh) {
    HMI_SaveProcessID(WaitResponse);
    if (HMI_IsChinese()) {
      DWINUI::ClearMenuArea();
      Draw_Popup_Bkgd();
      if (toohigh) {
        DWINUI::Draw_Icon(ICON_TempTooHigh, 102, 165);
        DWIN_Frame_AreaCopy(1, 103, 371, 237, 386, 52, 285);
        DWIN_Frame_AreaCopy(1, 151, 389, 185, 402, 187, 285);
        DWIN_Frame_AreaCopy(1, 189, 389, 271, 402, 95, 310);
      }
      else {
        DWINUI::Draw_Icon(ICON_TempTooLow, 102, 165);
        DWIN_Frame_AreaCopy(1, 103, 371, 270, 386, 52, 285);
        DWIN_Frame_AreaCopy(1, 189, 389, 271, 402, 95, 310);
      }
    }
    else DWIN_Show_Popup(toohigh ? ICON_TempTooHigh : ICON_TempTooLow, F("Nozzle or Bed temperature"), toohigh ? F("is too high") : F("is too low"), BTN_Continue);
  }
#endif

// Draw status line
void DWIN_DrawStatusLine(const char *text) {
  DWIN_Draw_Rectangle(1, HMI_data.StatusBg_Color, 0, STATUS_Y, DWIN_WIDTH, STATUS_Y + 20);
  if (text) DWINUI::Draw_CenteredString(HMI_data.StatusTxt_Color, STATUS_Y + 2, text);
}

// Clear & reset status line
void DWIN_ResetStatusLine() {
  ui.status_message[0] = 0;
  DWIN_CheckStatusMessage();
}

// Djb2 hash algorithm
void DWIN_CheckStatusMessage() {
  static uint32_t old_hash = 0;
  char * str = &ui.status_message[0];
  uint32_t hash = 5381;
  char c;
  while ((c = *str++)) hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  hash_changed = hash != old_hash;
  old_hash = hash;
};

void DWIN_DrawStatusMessage() {
  #if ENABLED(STATUS_MESSAGE_SCROLLING)

    // Get the UTF8 character count of the string
    uint8_t slen = utf8_strlen(ui.status_message);

    // If the string fits the status line do not scroll it
    if (slen <= LCD_WIDTH) {
       if (hash_changed) {
         DWIN_DrawStatusLine(ui.status_message);
         hash_changed = false;
       }
    }
    else {
      // String is larger than the available line space

      // Get a pointer to the next valid UTF8 character
      // and the string remaining length
      uint8_t rlen;
      const char *stat = MarlinUI::status_and_len(rlen);
      DWIN_Draw_Rectangle(1, HMI_data.StatusBg_Color, 0, STATUS_Y, DWIN_WIDTH, STATUS_Y + 20);
      DWINUI::MoveTo(0, STATUS_Y + 2);
      DWINUI::Draw_String(HMI_data.StatusTxt_Color, stat, LCD_WIDTH);

      // If the string doesn't completely fill the line...
      if (rlen < LCD_WIDTH) {
        DWINUI::Draw_Char(HMI_data.StatusTxt_Color, '.');  // Always at 1+ spaces left, draw a dot
        uint8_t chars = LCD_WIDTH - rlen;                  // Amount of space left in characters
        if (--chars) {                            // Draw a second dot if there's space
          DWINUI::Draw_Char(HMI_data.StatusTxt_Color, '.');
          if (--chars)
            DWINUI::Draw_String(HMI_data.StatusTxt_Color, ui.status_message, chars); // Print a second copy of the message
        }
      }
      MarlinUI::advance_status_scroll();
    }

  #else

    if (hash_changed) {
      ui.status_message[LCD_WIDTH] = 0;
      DWIN_DrawStatusLine(ui.status_message);
      hash_changed = false;
    }

  #endif
}

void Draw_Print_Labels() {
  if (HMI_IsChinese()) {
    DWIN_Frame_AreaCopy(1,  0, 72,  63, 86,  41, 173);  // Printing Time
    DWIN_Frame_AreaCopy(1, 65, 72, 128, 86, 176, 173);  // Remain
  }
  else {
    DWINUI::Draw_String( 46, 173, GET_TEXT_F(MSG_INFO_PRINT_TIME));
    DWINUI::Draw_String(181, 173, GET_TEXT_F(MSG_REMAINING_TIME));
  }
}

void Draw_Print_ProgressBar() {
  DWIN_ICON_Show(true, false, false, ICON, ICON_Bar, 15, 93);
  DWIN_Draw_Rectangle(1, HMI_data.Barfill_Color, 16 + _percent_done * 240 / 100, 93, 256, 113);
  DWINUI::Draw_Int(HMI_data.PercentTxt_Color, HMI_data.Background_Color, 3, 117, 133, _percent_done);
  DWINUI::Draw_String(HMI_data.PercentTxt_Color, 142, 133, F("%"));
}

void Draw_Print_ProgressElapsed() {
  char buf[10];
  duration_t elapsed = print_job_timer.duration(); // print timer
  sprintf_P(buf, PSTR("%02i:%02i"), (uint16_t)(elapsed.value / 3600), ((uint16_t)elapsed.value % 3600) / 60);
  DWINUI::Draw_String(HMI_data.Text_Color, HMI_data.Background_Color, 47, 192, buf);
}

void Draw_Print_ProgressRemain() {
  char buf[10];
  sprintf_P(buf, PSTR("%02i:%02i"), (uint16_t)(_remain_time / 3600), ((uint16_t)_remain_time % 3600) / 60);
  DWINUI::Draw_String(HMI_data.Text_Color, HMI_data.Background_Color, 181, 192, buf);
}

void ICON_ResumeOrPause() {
  if (checkkey == PrintProcess) printingIsPaused() ? ICON_Resume() : ICON_Pause();
}

// Update filename on print
void DWIN_Print_Header(const char *text = nullptr) {
  static char headertxt[31] = "";  // Print header text
  if (text) {
    const int8_t size = _MIN(30U, strlen_P(text));
    LOOP_L_N(i, size) headertxt[i] = text[i];
    headertxt[size] = '\0';
  }
  if (checkkey == PrintProcess || checkkey == PrintDone) {
    DWIN_Draw_Rectangle(1, HMI_data.Background_Color, 0, 60, DWIN_WIDTH, 60+16);
    DWINUI::Draw_CenteredString(60, headertxt);
  }
}

void Draw_PrintProcess() {
  if (HMI_IsChinese())
    Title.FrameCopy(30, 1, 42, 14);                     // "Printing"
  else
    Title.ShowCaption(GET_TEXT_F(MSG_PRINTING));
  DWINUI::ClearMenuArea();
  DWIN_Print_Header(sdprint ? card.longest_filename() : nullptr);
  Draw_Print_Labels();
  DWINUI::Draw_Icon(ICON_PrintTime, 15, 173);
  DWINUI::Draw_Icon(ICON_RemainTime, 150, 171);
  Draw_Print_ProgressBar();
  Draw_Print_ProgressElapsed();
  Draw_Print_ProgressRemain();
  ICON_Tune();
  ICON_ResumeOrPause();
  ICON_Stop();
}

void Goto_PrintProcess() {
  if (checkkey == PrintProcess)
    ICON_ResumeOrPause();
  else {
    checkkey = PrintProcess;
    Draw_PrintProcess();
  }
  DWIN_UpdateLCD();
}

void Draw_PrintDone() {
  // show percent bar and value
  _percent_done = 100;
  _remain_time = 0;

  Title.ShowCaption(GET_TEXT_F(MSG_PRINT_DONE));
  DWINUI::ClearMenuArea();
  DWIN_Print_Header(nullptr);
  Draw_Print_ProgressBar();
  Draw_Print_Labels();
  DWINUI::Draw_Icon(ICON_PrintTime, 15, 173);
  DWINUI::Draw_Icon(ICON_RemainTime, 150, 171);
  Draw_Print_ProgressElapsed();
  Draw_Print_ProgressRemain();
  // show print done confirm
  DWINUI::Draw_Button(BTN_Confirm, 86, 273);
}

void Goto_PrintDone() {
  wait_for_user = true;
  if (checkkey != PrintDone) {
    checkkey = PrintDone;
    Draw_PrintDone();
    DWIN_UpdateLCD();
  }
}

void Draw_Main_Menu() {
  DWINUI::ClearMenuArea();
  if (HMI_IsChinese())
    Title.FrameCopy(2, 2, 26, 13);   // "Home" etc
  else
    Title.ShowCaption(MACHINE_NAME);
  DWINUI::Draw_Icon(ICON_LOGO, 71, 52);  // CREALITY logo
  DWIN_ResetStatusLine();
  ICON_Print();
  ICON_Prepare();
  ICON_Control();
  ICON_AdvSettings();
}

void Goto_Main_Menu() {
  if (checkkey == MainMenu) return;
  checkkey = MainMenu;
  Draw_Main_Menu();
  DWIN_UpdateLCD();
}

// Draw X, Y, Z and blink if in an un-homed or un-trusted state
void _update_axis_value(const AxisEnum axis, const uint16_t x, const uint16_t y, const bool blink, const bool force) {
  const bool draw_qmark = axis_should_home(axis),
             draw_empty = NONE(HOME_AFTER_DEACTIVATE, DISABLE_REDUCED_ACCURACY_WARNING) && !draw_qmark && !axis_is_trusted(axis);

  // Check for a position change
  static xyz_pos_t oldpos = { -1, -1, -1 };
  const float p = current_position[axis];
  const bool changed = oldpos[axis] != p;
  if (changed) oldpos[axis] = p;

  if (force || changed || draw_qmark || draw_empty) {
    if (blink && draw_qmark)
      DWINUI::Draw_String(HMI_data.Coordinate_Color, HMI_data.Background_Color, x, y, F("  - ? -"));
    else if (blink && draw_empty)
      DWINUI::Draw_String(HMI_data.Coordinate_Color, HMI_data.Background_Color, x, y, F("       "));
    else
      DWINUI::Draw_Signed_Float(HMI_data.Coordinate_Color, HMI_data.Background_Color, 3, 2, x, y, p);
  }
}

void _draw_xyz_position(const bool force) {
  //SERIAL_ECHOPGM("Draw XYZ:");
  static bool _blink = false;
  const bool blink = !!(millis() & 0x400UL);
  if (force || blink != _blink) {
    _blink = blink;
    //SERIAL_ECHOPGM(" (blink)");
    _update_axis_value(X_AXIS,  27, 459, blink, true);
    _update_axis_value(Y_AXIS, 112, 459, blink, true);
    _update_axis_value(Z_AXIS, 197, 459, blink, true);
  }
  //SERIAL_EOL();
}

void update_variable() {
  #if HAS_HOTEND
    static celsius_t _hotendtemp = 0, _hotendtarget = 0;
    const celsius_t hc = thermalManager.wholeDegHotend(0),
                    ht = thermalManager.degTargetHotend(0);
    const bool _new_hotend_temp = _hotendtemp != hc,
               _new_hotend_target = _hotendtarget != ht;
    if (_new_hotend_temp) _hotendtemp = hc;
    if (_new_hotend_target) _hotendtarget = ht;
  #endif
  #if HAS_HEATED_BED
    static celsius_t _bedtemp = 0, _bedtarget = 0;
    const celsius_t bc = thermalManager.wholeDegBed(),
                    bt = thermalManager.degTargetBed();
    const bool _new_bed_temp = _bedtemp != bc,
               _new_bed_target = _bedtarget != bt;
    if (_new_bed_temp) _bedtemp = bc;
    if (_new_bed_target) _bedtarget = bt;
  #endif
  #if HAS_FAN
    static uint8_t _fanspeed = 0;
    const bool _new_fanspeed = _fanspeed != thermalManager.fan_speed[0];
    if (_new_fanspeed) _fanspeed = thermalManager.fan_speed[0];
  #endif

  if (checkkey == Menu && (CurrentMenu == TuneMenu || CurrentMenu == TemperatureMenu)) {
    // Tune page temperature update
    #if HAS_HOTEND
      if (_new_hotend_target)
        HotendTargetItem->draw(CurrentMenu->line(HotendTargetItem->pos));
    #endif
    #if HAS_HEATED_BED
      if (_new_bed_target)
        BedTargetItem->draw(CurrentMenu->line(BedTargetItem->pos));
    #endif
    #if HAS_FAN
      if (_new_fanspeed)
        FanSpeedItem->draw(CurrentMenu->line(FanSpeedItem->pos));
    #endif
  }

  // Bottom temperature update

  #if HAS_HOTEND
    if (_new_hotend_temp)
      DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 28, 384, _hotendtemp);
    if (_new_hotend_target)
      DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 25 + 4 * STAT_CHR_W + 6, 384, _hotendtarget);

    static int16_t _flow = planner.flow_percentage[0];
    if (_flow != planner.flow_percentage[0]) {
      _flow = planner.flow_percentage[0];
      DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 116 + 2 * STAT_CHR_W, 417, _flow);
    }
  #endif

  #if HAS_HEATED_BED
    if (_new_bed_temp)
      DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 28, 417, _bedtemp);
    if (_new_bed_target)
      DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 25 + 4 * STAT_CHR_W + 6, 417, _bedtarget);
  #endif

  static int16_t _feedrate = 100;
  if (_feedrate != feedrate_percentage) {
    _feedrate = feedrate_percentage;
    DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 116 + 2 * STAT_CHR_W, 384, _feedrate);
  }

  #if HAS_FAN
    if (_new_fanspeed) {
      _fanspeed = thermalManager.fan_speed[0];
      DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 195 + 2 * STAT_CHR_W, 384, _fanspeed);
    }
  #endif

  static float _offset = 0;
  if (BABY_Z_VAR != _offset) {
    _offset = BABY_Z_VAR;
    DWINUI::Draw_Signed_Float(DWIN_FONT_STAT, HMI_data.Indicator_Color,  HMI_data.Background_Color, 2, 2, 204, 417, _offset);
  }

  #if HAS_MESH
    static bool _leveling_active = false;
    if (_leveling_active != planner.leveling_active) {
      _leveling_active = planner.leveling_active;
      DWIN_Draw_Box(1, HMI_data.Background_Color, 186, 416, 20, 20);
      if (_leveling_active)
        DWINUI::Draw_Icon(ICON_SetZOffset, 186, 416);
      else
        DWINUI::Draw_Icon(ICON_Zoffset, 187, 416);
    }
  #endif

  _draw_xyz_position(false);
}

/**
 * Read and cache the working directory.
 *
 * TODO: New code can follow the pattern of menu_media.cpp
 * and rely on Marlin caching for performance. No need to
 * cache files here.
 */

#ifndef strcasecmp_P
  #define strcasecmp_P(a, b) strcasecmp((a), (b))
#endif

void make_name_without_ext(char *dst, char *src, size_t maxlen=MENU_CHAR_LIMIT) {
  size_t pos = strlen(src);  // index of ending nul

  // For files, remove the extension
  // which may be .gcode, .gco, or .g
  if (!card.flag.filenameIsDir)
    while (pos && src[pos] != '.') pos--; // find last '.' (stop at 0)

  if (!pos) pos = strlen(src);  // pos = 0 ('.' not found) restore pos

  size_t len = pos;   // nul or '.'
  if (len > maxlen) { // Keep the name short
    pos        = len = maxlen; // move nul down
    dst[--pos] = '.'; // insert dots
    dst[--pos] = '.';
    dst[--pos] = '.';
  }

  dst[len] = '\0';    // end it

  // Copy down to 0
  while (pos--) dst[pos] = src[pos];
}

void HMI_SDCardInit() { card.cdroot(); }

#if ENABLED(SCROLL_LONG_FILENAMES)

  char shift_name[LONG_FILENAME_LENGTH + 1];
  int8_t shift_amt; // = 0
  millis_t shift_ms; // = 0

  // Init the shift name based on the highlighted item
  void Init_Shift_Name() {
    const bool is_subdir = !card.flag.workDirIsRoot;
    const int8_t filenum = select_file.now - 1 - is_subdir; // Skip "Back" and ".."
    const uint16_t fileCnt = card.get_num_Files();
    if (WITHIN(filenum, 0, fileCnt - 1)) {
      card.getfilename_sorted(SD_ORDER(filenum, fileCnt));
      char * const name = card.longest_filename();
      make_name_without_ext(shift_name, name, 100);
    }
  }

  void Init_SDItem_Shift() {
    shift_amt = 0;
    shift_ms = select_file.now > 0 && strlen(shift_name) > MENU_CHAR_LIMIT ? millis() + 750UL : 0;
  }

#endif

/**
 * Display an SD item, adding a CDUP for subfolders.
 */
void Draw_SDItem(const uint16_t item, int16_t row=-1) {
  if (row < 0) row = item + 1 + MROWS - index_file;
  const bool is_subdir = !card.flag.workDirIsRoot;
  if (is_subdir && item == 0)
    return Draw_Menu_Line(row, ICON_Folder, "..");

  card.getfilename_sorted(SD_ORDER(item - is_subdir, card.get_num_Files()));
  char * const name = card.longest_filename();

  #if ENABLED(SCROLL_LONG_FILENAMES)
    // Init the current selected name
    // This is used during scroll drawing
    if (item == select_file.now - 1) {
      make_name_without_ext(shift_name, name, 100);
      Init_SDItem_Shift();
    }
  #endif

  // Draw the file/folder with name aligned left
  char str[strlen(name) + 1];
  make_name_without_ext(str, name);
  const uint8_t icon = card.flag.filenameIsDir ? ICON_Folder : card.fileIsBinary() ? ICON_Binary : ICON_File;
  Draw_Menu_Line(row, icon, str);
}

#if ENABLED(SCROLL_LONG_FILENAMES)

  void Draw_SDItem_Shifted(uint8_t &shift) {
    // Limit to the number of chars past the cutoff
    const size_t len = strlen(shift_name);
    NOMORE(shift, _MAX(len - MENU_CHAR_LIMIT, 0U));

    // Shorten to the available space
    const size_t lastchar = _MIN((signed)len, shift + MENU_CHAR_LIMIT);

    const char c = shift_name[lastchar];
    shift_name[lastchar] = '\0';

    const uint8_t row = select_file.now + MROWS - index_file; // skip "Back" and scroll
    Erase_Menu_Text(row);
    Draw_Menu_Line(row, 0, &shift_name[shift]);

    shift_name[lastchar] = c;
  }

#endif

// Redraw the first set of SD Files
void Redraw_SD_List() {
  select_file.reset();
  index_file = MROWS;

  DWINUI::ClearMenuArea(); // Leave title bar unchanged

  Draw_Back_First();

  if (card.isMounted()) {
    // As many files as will fit
    LOOP_L_N(i, _MIN(nr_sd_menu_items(), MROWS))
      Draw_SDItem(i, i + 1);

    TERN_(SCROLL_LONG_FILENAMES, Init_SDItem_Shift());
  }
  else {
    DWIN_Draw_Rectangle(1, HMI_data.AlertBg_Color, 10, MBASE(3) - 10, DWIN_WIDTH - 10, MBASE(4));
    DWINUI::Draw_CenteredString(font12x24, HMI_data.AlertTxt_Color, MBASE(3), GET_TEXT_F(MSG_MEDIA_NOT_INSERTED));
  }
}

bool DWIN_lcd_sd_status = false;

void SDCard_Up() {
  card.cdup();
  Redraw_SD_List();
  DWIN_lcd_sd_status = false; // On next DWIN_Update
}

void SDCard_Folder(char * const dirname) {
  card.cd(dirname);
  Redraw_SD_List();
  DWIN_lcd_sd_status = false; // On next DWIN_Update
}

//
// Watch for media mount / unmount
//
void HMI_SDCardUpdate() {
  if (HMI_flag.home_flag) return;
  if (DWIN_lcd_sd_status != card.isMounted()) {
    DWIN_lcd_sd_status = card.isMounted();
    //SERIAL_ECHOLNPGM("HMI_SDCardUpdate: ", DWIN_lcd_sd_status);
    if (DWIN_lcd_sd_status) {  // Media inserted
      if (checkkey == SelectFile)
        Redraw_SD_List();
    }
    else {    // Media removed
      // clean file icon
      if (checkkey == SelectFile) {
        Redraw_SD_List();
      }
      else if (sdprint && card.isPrinting() && printingIsActive()) {
        wait_for_heatup = wait_for_user = false;
        HMI_flag.abort_flag = true; // Abort print
      }
    }
    DWIN_UpdateLCD();
  }
}

//
// The Dashboard is always on-screen, except during
// full-screen modal dialogs.
//
void DWIN_Draw_Dashboard() {

  DWIN_Draw_Rectangle(1, HMI_data.Background_Color, 0, STATUS_Y + 21, DWIN_WIDTH, DWIN_HEIGHT - 1);

  #if HAS_HOTEND
    DWINUI::Draw_Icon(ICON_HotendTemp, 10, 383);
    DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 28, 384, thermalManager.wholeDegHotend(0));
    DWINUI::Draw_String(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 25 + 3 * STAT_CHR_W + 5, 384, F("/"));
    DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 25 + 4 * STAT_CHR_W + 6, 384, thermalManager.degTargetHotend(0));

    DWINUI::Draw_Icon(ICON_StepE, 112, 417);
    DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 116 + 2 * STAT_CHR_W, 417, planner.flow_percentage[0]);
    DWINUI::Draw_String(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 116 + 5 * STAT_CHR_W + 2, 417, F("%"));
  #endif

  #if HAS_HEATED_BED
    DWINUI::Draw_Icon(ICON_BedTemp, 10, 416);
    DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 28, 417, thermalManager.wholeDegBed());
    DWINUI::Draw_String(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 25 + 3 * STAT_CHR_W + 5, 417, F("/"));
    DWINUI::Draw_Int(true, true, 0, DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 25 + 4 * STAT_CHR_W + 6, 417, thermalManager.degTargetBed());
  #endif

  DWINUI::Draw_Icon(ICON_Speed, 113, 383);
  DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 116 + 2 * STAT_CHR_W, 384, feedrate_percentage);
  DWINUI::Draw_String(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 116 + 5 * STAT_CHR_W + 2, 384, F("%"));

  #if HAS_FAN
    DWINUI::Draw_Icon(ICON_FanSpeed, 187, 383);
    DWINUI::Draw_Int(DWIN_FONT_STAT, HMI_data.Indicator_Color, HMI_data.Background_Color, 3, 195 + 2 * STAT_CHR_W, 384, thermalManager.fan_speed[0]);
  #endif

  #if HAS_ZOFFSET_ITEM
    DWINUI::Draw_Icon(planner.leveling_active ? ICON_SetZOffset : ICON_Zoffset, 187, 416);
  #endif

  DWINUI::Draw_Signed_Float(DWIN_FONT_STAT, HMI_data.Indicator_Color,  HMI_data.Background_Color, 2, 2, 204, 417, BABY_Z_VAR);

  DWIN_Draw_Rectangle(1, HMI_data.SplitLine_Color, 0, 449, DWIN_WIDTH, 451);

  DWINUI::Draw_Icon(ICON_MaxSpeedX,  10, 456);
  DWINUI::Draw_Icon(ICON_MaxSpeedY,  95, 456);
  DWINUI::Draw_Icon(ICON_MaxSpeedZ, 180, 456);
  _draw_xyz_position(true);

}

void Draw_Info_Menu() {
  DWINUI::ClearMenuArea();
  Draw_Back_First();
  if (HMI_IsChinese())
    Title.FrameCopy(30, 17, 28, 13);                        // "Info"
  else
    Title.ShowCaption(GET_TEXT_F(MSG_INFO_SCREEN));

  if (HMI_IsChinese()) {
    DWIN_Frame_AreaCopy(1, 197, 149, 252, 161, 108, 102);   // "Size"
    DWIN_Frame_AreaCopy(1,   1, 164,  56, 176, 108, 175);   // "Firmware Version"
    DWIN_Frame_AreaCopy(1,  58, 164, 113, 176, 105, 248);   // "Contact Details"
    DWINUI::Draw_CenteredString(268, F(CORP_WEBSITE));
  }
  else {
    DWINUI::Draw_CenteredString(102, F("Size"));
    DWINUI::Draw_CenteredString(175, F("Firmware version"));
    DWINUI::Draw_CenteredString(248, F("Build Datetime"));
    DWINUI::Draw_CenteredString(268, F(STRING_DISTRIBUTION_DATE));
  }
  DWINUI::Draw_CenteredString(122, F(MACHINE_SIZE));
  DWINUI::Draw_CenteredString(195, F(SHORT_BUILD_VERSION));

  LOOP_L_N(i, 3) {
    DWINUI::Draw_Icon(ICON_PrintSize + i, ICOX, 99 + i * 73);
    DWIN_Draw_HLine(HMI_data.SplitLine_Color, 16, MBASE(2) + i * 73, 240);
  }
}

void Draw_Print_File_Menu() {
  if (HMI_IsChinese())
    Title.FrameCopy(0, 31, 56, 14);    // "Print file"
  else
    Title.ShowCaption(GET_TEXT_F(MSG_MEDIA_MENU));
  Redraw_SD_List();
}

// Main Process
void HMI_MainMenu() {
  EncoderState encoder_diffState = get_encoder_state();
  if (encoder_diffState == ENCODER_DIFF_NO) return;

  if (encoder_diffState == ENCODER_DIFF_CW) {
    if (select_page.inc(PAGE_COUNT)) {
      switch (select_page.now) {
        case PAGE_PRINT: ICON_Print(); break;
        case PAGE_PREPARE: ICON_Print(); ICON_Prepare(); break;
        case PAGE_CONTROL: ICON_Prepare(); ICON_Control(); break;
        case PAGE_ADVANCE: ICON_Control(); ICON_AdvSettings(); break;
      }
    }
  }
  else if (encoder_diffState == ENCODER_DIFF_CCW) {
    if (select_page.dec()) {
      switch (select_page.now) {
        case PAGE_PRINT: ICON_Print(); ICON_Prepare(); break;
        case PAGE_PREPARE: ICON_Prepare(); ICON_Control(); break;
        case PAGE_CONTROL: ICON_Control(); ICON_AdvSettings(); break;
        case PAGE_ADVANCE: ICON_AdvSettings(); break;
      }
    }
  }
  else if (encoder_diffState == ENCODER_DIFF_ENTER) {
    switch (select_page.now) {
      case PAGE_PRINT:
        checkkey = SelectFile;
        card.mount();
        delay(300);
        Draw_Print_File_Menu();
        break;
      case PAGE_PREPARE: Draw_Prepare_Menu(); break;
      case PAGE_CONTROL: Draw_Control_Menu(); break;
      case PAGE_ADVANCE: Draw_AdvancedSettings_Menu(); break;
    }
  }
  DWIN_UpdateLCD();
}

// Select (and Print) File
void HMI_SelectFile() {
  EncoderState encoder_diffState = get_encoder_state();

  const uint16_t hasUpDir = !card.flag.workDirIsRoot;

  if (encoder_diffState == ENCODER_DIFF_NO) {
    #if ENABLED(SCROLL_LONG_FILENAMES)
      if (shift_ms && select_file.now >= 1 + hasUpDir) {
        // Scroll selected filename every second
        const millis_t ms = millis();
        if (ELAPSED(ms, shift_ms)) {
          const bool was_reset = shift_amt < 0;
          shift_ms = ms + 375UL + was_reset * 250UL;  // ms per character
          uint8_t shift_new = shift_amt + 1;          // Try to shift by...
          Draw_SDItem_Shifted(shift_new);             // Draw the item
          if (!was_reset && shift_new == 0)           // Was it limited to 0?
            shift_ms = 0;                             // No scrolling needed
          else if (shift_new == shift_amt)            // Scroll reached the end
            shift_new = -1;                           // Reset
          shift_amt = shift_new;                      // Set new scroll
        }
      }
    #endif
    return;
  }

  // First pause is long. Easy.
  // On reset, long pause must be after 0.

  const uint16_t fullCnt = nr_sd_menu_items();

  if (encoder_diffState == ENCODER_DIFF_CW && fullCnt) {
    if (select_file.inc(1 + fullCnt)) {
      const uint8_t itemnum = select_file.now - 1;              // -1 for "Back"
      if (TERN0(SCROLL_LONG_FILENAMES, shift_ms)) {             // If line was shifted
        Erase_Menu_Text(itemnum + MROWS - index_file);          // Erase and
        Draw_SDItem(itemnum - 1);                               // redraw
      }
      if (select_file.now > MROWS && select_file.now > index_file) { // Cursor past the bottom
        index_file = select_file.now;                           // New bottom line
        Scroll_Menu(DWIN_SCROLL_UP);
        Draw_SDItem(itemnum, MROWS);                            // Draw and init the shift name
      }
      else {
        Move_Highlight(1, select_file.now + MROWS - index_file); // Just move highlight
        TERN_(SCROLL_LONG_FILENAMES, Init_Shift_Name());         // ...and init the shift name
      }
      TERN_(SCROLL_LONG_FILENAMES, Init_SDItem_Shift());
    }
  }
  else if (encoder_diffState == ENCODER_DIFF_CCW && fullCnt) {
    if (select_file.dec()) {
      const uint8_t itemnum = select_file.now - 1;              // -1 for "Back"
      if (TERN0(SCROLL_LONG_FILENAMES, shift_ms)) {             // If line was shifted
        Erase_Menu_Text(select_file.now + 1 + MROWS - index_file); // Erase and
        Draw_SDItem(itemnum + 1);                               // redraw
      }
      if (select_file.now < index_file - MROWS) {               // Cursor past the top
        index_file--;                                           // New bottom line
        Scroll_Menu(DWIN_SCROLL_DOWN);
        if (index_file == MROWS) {
          Draw_Back_First();
          TERN_(SCROLL_LONG_FILENAMES, shift_ms = 0);
        }
        else {
          Draw_SDItem(itemnum, 0);                              // Draw the item (and init shift name)
        }
      }
      else {
        Move_Highlight(-1, select_file.now + MROWS - index_file); // Just move highlight
        TERN_(SCROLL_LONG_FILENAMES, Init_Shift_Name());        // ...and init the shift name
      }
      TERN_(SCROLL_LONG_FILENAMES, Init_SDItem_Shift());        // Reset left. Init timer.
    }
  }
  else if (encoder_diffState == ENCODER_DIFF_ENTER) {
    if (select_file.now == 0) { // Back
      select_page.set(PAGE_PRINT);
      return Goto_Main_Menu();
    }
    else if (hasUpDir && select_file.now == 1) { // CD-Up
      SDCard_Up();
      goto HMI_SelectFileExit;
    }
    else {
      const uint16_t filenum = select_file.now - 1 - hasUpDir;
      card.getfilename_sorted(SD_ORDER(filenum, card.get_num_Files()));

      // Enter that folder!
      if (card.flag.filenameIsDir) {
        SDCard_Folder(card.filename);
        goto HMI_SelectFileExit;
      }

      // Reset highlight for next entry
      select_print.reset();
      select_file.reset();

      // Start choice and print SD file
      HMI_flag.heat_flag = true;
      HMI_flag.print_finish = false;

      if (card.fileIsBinary())
        return DWIN_Popup_Confirm(ICON_Error, F("Please check filenames"), F("Only G-code can be printed"));
      else
        return Goto_ConfirmToPrint();
    }
  }

  HMI_SelectFileExit:
  DWIN_UpdateLCD();
}

// Pause or Stop popup
void onClick_PauseOrStop() {
  switch (select_print.now) {
    case PRINT_PAUSE_RESUME: if (HMI_flag.select_flag) HMI_flag.pause_flag = true; break; // confirm pause
    case PRINT_STOP: if (HMI_flag.select_flag) HMI_flag.abort_flag = true; break; // stop confirmed then abort print
    default: break;
  }
  return Goto_PrintProcess();
}

// Printing
void HMI_Printing() {
  EncoderState encoder_diffState = get_encoder_state();
  if (encoder_diffState == ENCODER_DIFF_NO) return;
  // Avoid flicker by updating only the previous menu
  if (encoder_diffState == ENCODER_DIFF_CW) {
    if (select_print.inc(PRINT_COUNT)) {
      switch (select_print.now) {
        case PRINT_SETUP: ICON_Tune(); break;
        case PRINT_PAUSE_RESUME: ICON_Tune(); ICON_ResumeOrPause(); break;
        case PRINT_STOP: ICON_ResumeOrPause(); ICON_Stop(); break;
      }
    }
  }
  else if (encoder_diffState == ENCODER_DIFF_CCW) {
    if (select_print.dec()) {
      switch (select_print.now) {
        case PRINT_SETUP: ICON_Tune(); ICON_ResumeOrPause(); break;
        case PRINT_PAUSE_RESUME: ICON_ResumeOrPause(); ICON_Stop(); break;
        case PRINT_STOP: ICON_Stop(); break;
      }
    }
  }
  else if (encoder_diffState == ENCODER_DIFF_ENTER) {
    switch (select_print.now) {
      case PRINT_SETUP: Draw_Tune_Menu(); break;
      case PRINT_PAUSE_RESUME:
        if (printingIsPaused()) {  // if printer is already in pause
          ui.resume_print();
          break;
        }
        else
          return Goto_Popup(Popup_window_PauseOrStop, onClick_PauseOrStop);
      case PRINT_STOP:
        return Goto_Popup(Popup_window_PauseOrStop, onClick_PauseOrStop);
      default: break;
    }
  }
  DWIN_UpdateLCD();
}

#include "../../../libs/buzzer.h"

void HMI_AudioFeedback(const bool success/*=true*/) {
  #if HAS_BUZZER
    if (success) {
      BUZZ(100, 659);
      BUZZ(10, 0);
      BUZZ(100, 698);
    }
    else
      BUZZ(40, 440);
  #endif
}

void Draw_Main_Area() {
  switch (checkkey) {
    case MainMenu:               Draw_Main_Menu(); break;
    case SelectFile:             Draw_Print_File_Menu(); break;
    case PrintProcess:           Draw_PrintProcess(); break;
    case PrintDone:              Draw_PrintDone(); break;
    #if HAS_ESDIAG
      case ESDiagProcess:        Draw_EndStopDiag(); break;
    #endif
    case Popup:                  popupDraw(); break;
    case Locked:                 lockScreen.draw(); break;
    case Menu:
    case SetInt:
    case SetPInt:
    case SetIntNoDraw:
    case SetFloat:
    case SetPFloat:              CurrentMenu->draw(); break;
    default: break;
  }
}

void HMI_ReturnScreen() {
  checkkey = last_checkkey;
  wait_for_user = false;
  Draw_Main_Area();
}

void HMI_WaitForUser() {
  get_encoder_state();
  if (!wait_for_user) {
    switch (checkkey) {
      case PrintDone:
        select_page.reset();
        Goto_Main_Menu();
        break;
      #if HAS_ONESTEP_LEVELING
      case Leveling:
        //TERN_(ProUI, ProEx.StopLeveling());
        HMI_ReturnScreen();
        break;
      #endif
      default:
        HMI_ReturnScreen();
        break;
    }
  }
}

void HMI_Init() {
  DWINUI::Draw_Box(1, Color_Black, {5, 220, DWIN_WIDTH-5, DWINUI::fontHeight()});
  DWINUI::Draw_CenteredString(Color_White, 220, F("Professional Firmware "));
  for (uint16_t t = 0; t <= 100; t += 2) {
    DWINUI::Draw_Icon(ICON_Bar, 15, 260);
    DWIN_Draw_Rectangle(1, HMI_data.Background_Color, 15 + t * 242 / 100, 260, 257, 280);
    DWIN_UpdateLCD();
    delay(20);
  }
  HMI_SetLanguage();
}

void EachMomentUpdate() {
  static millis_t next_var_update_ms = 0, next_rts_update_ms = 0, next_status_update_ms = 0;

  const millis_t ms = millis();
  if (ELAPSED(ms, next_var_update_ms)) {
    next_var_update_ms = ms + DWIN_VAR_UPDATE_INTERVAL;
    update_variable();
    switch (checkkey) {
      #if HAS_ESDIAG
        case ESDiagProcess:
          ESDiag.Update();
          break;
      #endif
      default:
        break;
    }
  }

  if (ELAPSED(ms, next_status_update_ms)) {
    next_status_update_ms = ms + 500;
    DWIN_DrawStatusMessage();
  }

  if (PENDING(ms, next_rts_update_ms)) return;
  next_rts_update_ms = ms + DWIN_SCROLL_UPDATE_INTERVAL;

  if (checkkey == PrintProcess) { // print process

    // Print pause
    if (HMI_flag.pause_flag && !HMI_flag.home_flag) {
      HMI_flag.pause_flag = false;
      if (!HMI_flag.pause_action) {
        HMI_flag.pause_action = true;
        return ui.pause_print();
      }
    }

    // if print done
    if (HMI_flag.print_finish && !HMI_flag.home_flag) {
      HMI_flag.print_finish = false;
      return DWIN_Print_Finished();
    }

    // if print was aborted
    if (HMI_flag.abort_flag && !HMI_flag.home_flag) { // Print Stop
      HMI_flag.abort_flag = false;
      if (!HMI_flag.abort_action) {
        HMI_flag.abort_action = true;
        ui.abort_print();
        return Goto_PrintDone();
      }
    }

    duration_t elapsed = print_job_timer.duration(); // print timer

    if (sdprint && card.isPrinting()) {
      uint8_t percentDone = card.percentDone();
      if (_percent_done != percentDone) { // print percent
          _percent_done = percentDone;
          Draw_Print_ProgressBar();
        }

      // Estimate remaining time every 20 seconds
      static millis_t next_remain_time_update = 0;
      if (_percent_done > 1 && ELAPSED(ms, next_remain_time_update) && !HMI_flag.heat_flag && !HMI_flag.remain_flag) {
        _remain_time = (elapsed.value - dwin_heat_time) / (_percent_done * 0.01f) - (elapsed.value - dwin_heat_time);
        next_remain_time_update += DWIN_REMAIN_TIME_UPDATE_INTERVAL;
        Draw_Print_ProgressRemain();
      }
    }

    // Print time so far
    static uint16_t last_Printtime = 0;
    const uint16_t min = (elapsed.value % 3600) / 60;
    if (last_Printtime != min) { // 1 minute update
      last_Printtime = min;
      Draw_Print_ProgressElapsed();
    }

  }

  #if ENABLED(POWER_LOSS_RECOVERY)
    else if (DWIN_lcd_sd_status && recovery.dwin_flag) { // resume print before power off
      return Goto_PowerLossRecovery();
    }
  #endif // POWER_LOSS_RECOVERY

  DWIN_UpdateLCD();
}

#if ENABLED(POWER_LOSS_RECOVERY)
  void Popup_PowerLossRecovery() {
    DWINUI::ClearMenuArea();
    Draw_Popup_Bkgd();
    if (HMI_IsChinese()) {
      DWIN_Frame_AreaCopy(1, 160, 338, 235, 354, 98, 115);
      DWIN_Frame_AreaCopy(1, 103, 321, 271, 335, 52, 167);
      DWINUI::Draw_IconWB(ICON_Cancel_C,    26, 280);
      DWINUI::Draw_IconWB(ICON_Continue_C, 146, 280);
    }
    else {
      DWINUI::Draw_CenteredString(HMI_data.PopupTxt_Color, 70, GET_TEXT_F(MSG_OUTAGE_RECOVERY));
      DWINUI::Draw_CenteredString(HMI_data.PopupTxt_Color, 147, F("It looks like the last"));
      DWINUI::Draw_CenteredString(HMI_data.PopupTxt_Color, 167, F("file was interrupted."));
      DWINUI::Draw_Button(BTN_Cancel,    26, 280);
      DWINUI::Draw_Button(BTN_Continue, 146, 280);
    }
    SdFile *dir = nullptr;
    const char * const filename = card.diveToFile(true, dir, recovery.info.sd_filename);
    card.selectFileByName(filename);
    DWINUI::Draw_CenteredString(HMI_data.PopupTxt_Color, 207, card.longest_filename());
    Draw_Select_Highlight(HMI_flag.select_flag);
    DWIN_UpdateLCD();
  }

  void onClick_PowerLossRecovery() {
    if (HMI_flag.select_flag) {
      queue.inject(F("M1000C"));
      select_page.reset();
      return Goto_Main_Menu();
    }
    else {
      select_print.set(PRINT_SETUP);
      queue.inject(F("M1000"));
      sdprint = true;
      return Goto_PrintProcess();
    }
  }

  void Goto_PowerLossRecovery() {
    recovery.dwin_flag = false;
    LCD_MESSAGE_F(GET_TEXT_F(MSG_CONTINUE_PRINT_JOB));
    Goto_Popup(Popup_PowerLossRecovery, onClick_PowerLossRecovery);
  }

#endif // POWER_LOSS_RECOVERY


void DWIN_HandleScreen() {
  switch (checkkey) {
    case MainMenu:        HMI_MainMenu(); break;
    case Menu:            HMI_Menu(); break;
    case SetInt:          HMI_SetInt(); break;
    case SetPInt:         HMI_SetPInt(); break;
    case SetIntNoDraw:    HMI_SetIntNoDraw(); break;
    case SetFloat:        HMI_SetFloat(); break;
    case SetPFloat:       HMI_SetPFloat(); break;
    case SelectFile:      HMI_SelectFile(); break;
    case PrintProcess:    HMI_Printing(); break;
    case Popup:           HMI_Popup(); break;
    case Leveling:        //TERN_(ProUI, HMI_WaitForUser());
                          break;
    case Locked:          HMI_LockScreen(); break;
    case PrintDone:
    TERN_(HAS_ESDIAG, case ESDiagProcess:)
    case WaitResponse:    HMI_WaitForUser(); break;
    case Homing:
    case PidProcess:
    case NothingToDo:     break;
    default: break;
  }
}

bool IDisPopUp() {    // If ID is popup...
  return  (checkkey == NothingToDo)
       || (checkkey == WaitResponse)
       || (checkkey == Homing)
       || (checkkey == Leveling)
       || (checkkey == PidProcess)
       || TERN0(HAS_ESDIAG, (checkkey == ESDiagProcess))
       || (checkkey == Popup);
}

void HMI_SaveProcessID(const uint8_t id) {
  if (checkkey != id) {
    if (!IDisPopUp()) last_checkkey = checkkey; // if previous is not a popup
    if ((id == Popup)
         || TERN0(HAS_ESDIAG, (id == ESDiagProcess))
         || (id == PrintDone)
         || (id == Leveling)
         || (id == WaitResponse)) wait_for_user = true;
    checkkey = id;
  }
}

void DWIN_StartHoming() {
  HMI_flag.home_flag = true;
  HMI_SaveProcessID(Homing);
  Title.ShowCaption(GET_TEXT_F(MSG_HOMING));
  DWIN_Show_Popup(ICON_BLTouch, GET_TEXT_F(MSG_HOMING), GET_TEXT_F(MSG_PLEASE_WAIT));
}

void DWIN_CompletedHoming() {
  HMI_flag.home_flag = false;
  dwin_zoffset = TERN0(HAS_BED_PROBE, probe.offset.z);
  if (HMI_flag.abort_action) DWIN_Print_Aborted(); else HMI_ReturnScreen();
}

void DWIN_MeshLevelingStart() {
  #if HAS_ONESTEP_LEVELING
    HMI_SaveProcessID(Leveling);
    Title.ShowCaption(GET_TEXT_F(MSG_BED_LEVELING));
    DWIN_Show_Popup(ICON_AutoLeveling, GET_TEXT_F(MSG_BED_LEVELING), GET_TEXT_F(MSG_PLEASE_WAIT));
  #elif ENABLED(MESH_BED_LEVELING)
    Draw_ManualMesh_Menu();
  #endif
}

void DWIN_CompletedLeveling() {
  TERN_(HAS_ONESTEP_LEVELING, if (planner.leveling_active) Goto_MeshViewer());
}

#if HAS_MESH
  void DWIN_MeshUpdate(const int8_t xpos, const int8_t ypos, const_float_t zval) {
    char msg[33] = "";
    char str_1[6] = "";
    sprintf_P(msg, PSTR(S_FMT " %i/%i Z=%s"), GET_TEXT(MSG_PROBING_POINT), xpos, ypos, dtostrf(zval, 1, 2, str_1));
    ui.set_status(msg);
  }
#endif

// PID process
void DWIN_PidTuning(pidresult_t result) {
  switch (result) {
    case PID_BED_START:
      HMI_SaveProcessID(NothingToDo);
      DWIN_Draw_Popup(ICON_TempTooHigh, GET_TEXT_F(MSG_PID_AUTOTUNE), F("for BED is running."));
      break;
    case PID_EXTR_START:
      HMI_SaveProcessID(NothingToDo);
      DWIN_Draw_Popup(ICON_TempTooHigh, GET_TEXT_F(MSG_PID_AUTOTUNE), F("for Nozzle is running."));
      break;
    case PID_BAD_EXTRUDER_NUM:
      checkkey = last_checkkey;
      DWIN_Popup_Confirm(ICON_TempTooLow, F("PID Autotune failed!"), F("Bad extruder"));
      break;
    case PID_TUNING_TIMEOUT:
      checkkey = last_checkkey;
      DWIN_Popup_Confirm(ICON_TempTooHigh, F("Error"), GET_TEXT_F(MSG_PID_TIMEOUT));
      break;
    case PID_TEMP_TOO_HIGH:
      checkkey = last_checkkey;
      DWIN_Popup_Confirm(ICON_TempTooHigh, F("PID Autotune failed!"), F("Temperature too high"));
      break;
    case PID_DONE:
      checkkey = last_checkkey;
      DWIN_Popup_Confirm(ICON_TempTooLow, GET_TEXT_F(MSG_PID_AUTOTUNE), GET_TEXT_F(MSG_BUTTON_DONE));
      break;
    default:
      checkkey = last_checkkey;
      break;
  }
}

// Started a Print Job
void DWIN_Print_Started(const bool sd) {
  sdprint = IS_SD_PRINTING() || sd;
  _percent_done = 0;
  _remain_time = 0;
  HMI_flag.remain_flag = false;
  HMI_flag.pause_flag = false;
  HMI_flag.pause_action = false;
  HMI_flag.abort_flag = false;
  HMI_flag.abort_action = false;
  HMI_flag.print_finish = false;
  Goto_PrintProcess();
}

// Pause a print job
void DWIN_Print_Pause() {
  ICON_ResumeOrPause();
}

// Resume print job
void DWIN_Print_Resume() {
  HMI_flag.pause_action = false;
  ICON_ResumeOrPause();
  if (printJobOngoing()) {
    LCD_MESSAGE(MSG_RESUME_PRINT);
    Goto_PrintProcess();
  }
}

// Ended print job
void DWIN_Print_Finished() {
  if (HMI_flag.abort_flag || checkkey == PrintDone) return;
  TERN_(POWER_LOSS_RECOVERY, if (card.isPrinting()) recovery.cancel());
  HMI_flag.pause_flag = false;
  wait_for_heatup = false;
  planner.finish_and_disable();
    thermalManager.cooldown();
  Goto_PrintDone();
}

// Print was aborted
void DWIN_Print_Aborted() {
  TERN_(DEBUG_DWIN, SERIAL_ECHOLNPGM("DWIN_Print_Aborted"));
  HMI_flag.abort_action = false;
  wait_for_heatup = false;
  planner.finish_and_disable();
  thermalManager.cooldown();
  Goto_PrintDone();
}

// Progress Bar update
void DWIN_Progress_Update() {
  if (parser.seenval('P')) _percent_done = parser.byteval('P');
  if (parser.seenval('R')) {
    _remain_time = parser.ulongval('R') * 60;
    HMI_flag.remain_flag = true;
  }
  if (checkkey == PrintProcess) {
    Draw_Print_ProgressBar();
    Draw_Print_ProgressRemain();
    Draw_Print_ProgressElapsed();
  }
}

#if HAS_FILAMENT_SENSOR
  // Filament Runout process
  void DWIN_FilamentRunout(const uint8_t extruder) { LCD_MESSAGE(MSG_RUNOUT_SENSOR); }
#endif

void DWIN_SetColorDefaults() {
  HMI_data.Background_Color = Def_Background_Color;
  HMI_data.Cursor_color     = Def_Cursor_color;
  HMI_data.TitleBg_color    = Def_TitleBg_color;
  HMI_data.TitleTxt_color   = Def_TitleTxt_color;
  HMI_data.Text_Color       = Def_Text_Color;
  HMI_data.Selected_Color   = Def_Selected_Color;
  HMI_data.SplitLine_Color  = Def_SplitLine_Color;
  HMI_data.Highlight_Color  = Def_Highlight_Color;
  HMI_data.StatusBg_Color   = Def_StatusBg_Color;
  HMI_data.StatusTxt_Color  = Def_StatusTxt_Color;
  HMI_data.PopupBg_color    = Def_PopupBg_color;
  HMI_data.PopupTxt_Color   = Def_PopupTxt_Color;
  HMI_data.AlertBg_Color    = Def_AlertBg_Color;
  HMI_data.AlertTxt_Color   = Def_AlertTxt_Color;
  HMI_data.PercentTxt_Color = Def_PercentTxt_Color;
  HMI_data.Barfill_Color    = Def_Barfill_Color;
  HMI_data.Indicator_Color  = Def_Indicator_Color;
  HMI_data.Coordinate_Color = Def_Coordinate_Color;
}

void DWIN_SetDataDefaults() {
  DWIN_SetColorDefaults();
  DWINUI::SetColors(HMI_data.Text_Color, HMI_data.Background_Color, HMI_data.StatusBg_Color);
  TERN_(HAS_HOTEND,     HMI_data.HotendPidT = PREHEAT_1_TEMP_HOTEND);
  TERN_(HAS_HEATED_BED, HMI_data.BedPidT    = PREHEAT_1_TEMP_BED);
  TERN_(HAS_HOTEND,     HMI_data.PidCycles  = 5);
  #if ENABLED(PREVENT_COLD_EXTRUSION)
    HMI_data.ExtMinT = EXTRUDE_MINTEMP;
    ApplyExtMinT();
  #endif
  #if BOTH(HAS_HEATED_BED, PREHEAT_BEFORE_LEVELING)
    HMI_data.BedLevT = PREHEAT_1_TEMP_BED;
  #endif
  TERN_(BAUD_RATE_GCODE, SetBaud250K());
}

void DWIN_StoreSettings(char *buff) {
  memcpy(buff, &HMI_data, _MIN(sizeof(HMI_data), eeprom_data_size));
}

void DWIN_LoadSettings(const char *buff) {
  // (void *)-> Avoid Warning when save data different from uintX_t in HMI_data_t struct
  memcpy((void *)&HMI_data, buff, _MIN(sizeof(HMI_data), eeprom_data_size));
  dwin_zoffset = TERN0(HAS_BED_PROBE, probe.offset.z);
  if (HMI_data.Text_Color == HMI_data.Background_Color) DWIN_SetColorDefaults();
  DWINUI::SetColors(HMI_data.Text_Color, HMI_data.Background_Color, HMI_data.StatusBg_Color);
  TERN_(PREVENT_COLD_EXTRUSION, ApplyExtMinT());
  feedrate_percentage = 100;
  TERN_(BAUD_RATE_GCODE, HMI_SetBaudRate());
  #if BOTH(CASE_LIGHT_MENU, CASELIGHT_USES_BRIGHTNESS)
    // Apply Case light brightness
    caselight.brightness = HMI_data.CaseLight_Brightness;
    caselight.update_brightness();
  #endif
  #if BOTH(LED_CONTROL_MENU, HAS_COLOR_LEDS)
    // Apply Led Color
    leds.set_color(HMI_data.Led_Color);
  #endif
}

// Initialize or re-initialize the LCD
void MarlinUI::init_lcd() {
  TERN_(DEBUG_DWIN, SERIAL_ECHOLNPGM("DWIN_Startup"));
  DWINUI::init();
  DWIN_JPG_CacheTo1(Language_English);
  Encoder_Configuration();
}

void DWIN_InitScreen() {
  HMI_Init();   // draws boot screen
  DWINUI::onCursorDraw = Draw_Menu_Cursor;
  DWINUI::onCursorErase = Erase_Menu_Cursor;
  DWINUI::onTitleDraw = Draw_Title;
  DWINUI::onMenuDraw = Draw_Menu;
  DWIN_DrawStatusLine(nullptr);
  DWIN_Draw_Dashboard();
  Goto_Main_Menu();
}

void MarlinUI::update() {
  EachMomentUpdate();   // Status update
  HMI_SDCardUpdate();   // SD card update
  DWIN_HandleScreen();  // Rotary encoder update
}

void MarlinUI::refresh() { /* Nothing to see here */ }

#if HAS_LCD_BRIGHTNESS
  void MarlinUI::_set_brightness() { DWIN_LCD_Brightness(backlight ? brightness : 0); }
#endif

void MarlinUI::kill_screen(FSTR_P const lcd_error, FSTR_P const lcd_component) {
  DWIN_Draw_Popup(ICON_BLTouch, F("Printer killed:"), lcd_error);
  DWINUI::Draw_CenteredString(HMI_data.PopupTxt_Color, 270, F("Turn off the printer"));
  DWIN_UpdateLCD();
}

void DWIN_RebootScreen() {
  DWIN_Frame_Clear(Color_Bg_Black);
  DWIN_JPG_ShowAndCache(0);
  DWINUI::Draw_CenteredString(Color_White, 220, GET_TEXT_F(MSG_PLEASE_WAIT_REBOOT));
  DWIN_UpdateLCD();
  delay(500);
}

void DWIN_Redraw_screen() {
  Draw_Main_Area();
  hash_changed = true;
  DWIN_DrawStatusMessage();
  DWIN_Draw_Dashboard();
}

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  void DWIN_Popup_Pause(FSTR_P const fmsg, uint8_t button /*= 0*/) {
    HMI_SaveProcessID(button ? WaitResponse : NothingToDo);
    DWIN_Show_Popup(ICON_BLTouch, GET_TEXT_F(MSG_ADVANCED_PAUSE), fmsg, button);
  }

  void MarlinUI::pause_show_message(const PauseMessage message, const PauseMode mode/*=PAUSE_MODE_SAME*/, const uint8_t extruder/*=active_extruder*/) {
    //if (mode == PAUSE_MODE_SAME) return;
    pause_mode = mode;
    switch (message) {
      case PAUSE_MESSAGE_PARKING:  DWIN_Popup_Pause(GET_TEXT_F(MSG_PAUSE_PRINT_PARKING));    break;                                     // M125
      case PAUSE_MESSAGE_CHANGING: DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_INIT));   break;                                     // pause_print (M125, M600)
      case PAUSE_MESSAGE_WAITING:  DWIN_Popup_Pause(GET_TEXT_F(MSG_ADVANCED_PAUSE_WAITING), BTN_Continue); break;
      case PAUSE_MESSAGE_INSERT:   DWIN_Popup_Continue(ICON_BLTouch, GET_TEXT_F(MSG_ADVANCED_PAUSE), GET_TEXT_F(MSG_FILAMENT_CHANGE_INSERT)); break;
      case PAUSE_MESSAGE_LOAD:     DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_LOAD));   break;
      case PAUSE_MESSAGE_UNLOAD:   DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_UNLOAD)); break;                                     // Unload of pause and Unload of M702
      case PAUSE_MESSAGE_PURGE:
        #if ENABLED(ADVANCED_PAUSE_CONTINUOUS_PURGE)
          DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_CONT_PURGE));
        #else
          DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_PURGE));
        #endif
        break;
      case PAUSE_MESSAGE_OPTION:   Goto_FilamentPurge(); break;
      case PAUSE_MESSAGE_RESUME:   DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_RESUME)); break;
      case PAUSE_MESSAGE_HEAT:     DWIN_Popup_Pause(GET_TEXT_F(MSG_FILAMENT_CHANGE_HEAT), BTN_Continue);   break;
      case PAUSE_MESSAGE_HEATING:  LCD_MESSAGE(MSG_FILAMENT_CHANGE_HEATING); break;
      case PAUSE_MESSAGE_STATUS:   HMI_ReturnScreen(); break;                                                                         // Exit from Pause, Load and Unload
      default: break;
    }
  }

  void Draw_Popup_FilamentPurge() {
    DWIN_Draw_Popup(ICON_BLTouch, GET_TEXT_F(MSG_ADVANCED_PAUSE), F("Purge or Continue?"));
    DWINUI::Draw_Button(BTN_Confirm, 26, 280);
    DWINUI::Draw_Button(BTN_Continue, 146, 280);
    Draw_Select_Highlight(true);
  }

  void onClick_FilamentPurge() {
      if (HMI_flag.select_flag)
        pause_menu_response = PAUSE_RESPONSE_EXTRUDE_MORE;  // "Purge More" button
      else {
        HMI_SaveProcessID(NothingToDo);
        pause_menu_response = PAUSE_RESPONSE_RESUME_PRINT;  // "Continue" button
      }
    }

  void Goto_FilamentPurge() {
    pause_menu_response = PAUSE_RESPONSE_WAIT_FOR;
    Goto_Popup(Draw_Popup_FilamentPurge, onClick_FilamentPurge);
  }

#endif // ADVANCED_PAUSE_FEATURE

#if HAS_MESH
  void DWIN_MeshViewer() {
    if (!leveling_is_valid())
      DWIN_Popup_Continue(ICON_BLTouch, GET_TEXT_F(MSG_MESH_VIEWER), GET_TEXT_F(MSG_NO_VALID_MESH));
    else {
      HMI_SaveProcessID(WaitResponse);
      MeshViewer.Draw();
    }
  }
#endif // HAS_MESH

void DWIN_LockScreen() {
  if (checkkey != Locked) {
    lockScreen.rprocess = checkkey;
    checkkey = Locked;
    lockScreen.init();
  }
}

void DWIN_UnLockScreen() {
  if (checkkey == Locked) {
    checkkey = lockScreen.rprocess;
    Draw_Main_Area();
  }
}

void HMI_LockScreen() {
  EncoderState encoder_diffState = get_encoder_state();
  if (encoder_diffState == ENCODER_DIFF_NO) return;
  lockScreen.onEncoder(encoder_diffState);
  if (lockScreen.isUnlocked()) DWIN_UnLockScreen();
}


void Goto_ConfirmToPrint() {
  card.openAndPrintFile(card.filename);
  DWIN_Print_Started(true);
}

#if HAS_ESDIAG
  void Draw_EndStopDiag() {
    HMI_SaveProcessID(ESDiagProcess);
    ESDiag.Draw();
  }
#endif

//=============================================================================
// NEW MENU SUBSYSTEM
//=============================================================================

// Tool functions

#if ENABLED(EEPROM_SETTINGS)
  void WriteEeprom() {
    const bool success = settings.save();
    HMI_AudioFeedback(success);
  }

  void ReadEeprom() {
    const bool success = settings.load();
    DWIN_Redraw_screen();
    HMI_AudioFeedback(success);
  }

  void ResetEeprom() {
    settings.reset();
    DWIN_Redraw_screen();
    HMI_AudioFeedback();
  }
#endif

// Reset Printer
void RebootPrinter() {
  wait_for_heatup = wait_for_user = false;    // Stop waiting for heating/user
  thermalManager.disable_all_heaters();
  planner.finish_and_disable();
  DWIN_RebootScreen();
  hal.reboot();
}

void Goto_Info_Menu(){
  Draw_Info_Menu();
  HMI_SaveProcessID(WaitResponse);
}

void Goto_Move_Menu() {
  #if HAS_HOTEND
    gcode.process_subcommands_now(F("G92E0"));  // reset extruder position
    planner.synchronize();
  #endif
  Draw_Move_Menu();
}

void DisableMotors() { queue.inject(F("M84")); }

void AutoLev() { queue.inject(F("G28XYO\nG28Z\nG29")); }  // Force to get the current Z home position

void AutoHome() { queue.inject_P(G28_STR); }
void HomeX() { queue.inject(F("G28X")); }
void HomeY() { queue.inject(F("G28Y")); }
void HomeZ() { queue.inject(F("G28Z")); }

void SetHome() {
  // Apply workspace offset, making the current position 0,0,0
  queue.inject(F("G92 X0 Y0 Z0"));
  HMI_AudioFeedback();
}

#if HAS_ZOFFSET_ITEM
  bool printer_busy() { return planner.movesplanned() || printingIsActive(); }
  void ApplyZOffset() { TERN_(EEPROM_SETTINGS, settings.save()); }
  void LiveZOffset() {
    last_zoffset = dwin_zoffset;
    dwin_zoffset = MenuData.Value / 100.0f;
    #if EITHER(BABYSTEP_ZPROBE_OFFSET, JUST_BABYSTEP)
      if (BABYSTEP_ALLOWED()) babystep.add_mm(Z_AXIS, dwin_zoffset - last_zoffset);
    #endif
  }
  #if EITHER(HAS_BED_PROBE, BABYSTEPPING)
    void SetZOffset() {
      SetPFloatOnClick(Z_PROBE_OFFSET_RANGE_MIN, Z_PROBE_OFFSET_RANGE_MAX, 2, ApplyZOffset, LiveZOffset);
    }
  #endif

  void SetMoveZto0() {
    char cmd[48] = "";
    char str_1[5] = "", str_2[5] = "";
    sprintf_P(cmd, PSTR("G28XYO\nG28Z\nG0X%sY%sF5000\nM420S0\nG0Z0F300"),
      #if ENABLED(MESH_BED_LEVELING)
        dtostrf(0, 1, 1, str_1),
        dtostrf(0, 1, 1, str_2)
      #else
        dtostrf(X_CENTER, 1, 1, str_1),
        dtostrf(Y_CENTER, 1, 1, str_2)
      #endif
    );
    gcode.process_subcommands_now(cmd);
    planner.synchronize();
    LCD_MESSAGE_F("Now adjust Z Offset");
    HMI_AudioFeedback(true);
  }
#endif

#if HAS_PREHEAT
  void DoPreheat0() { ui.preheat_all(0); }
  void DoPreheat1() { ui.preheat_all(1); }
  void DoPreheat2() { ui.preheat_all(2); }
#endif

void DoCoolDown() { thermalManager.cooldown(); }

void SetLanguage() {
  HMI_ToggleLanguage();
  CurrentMenu = nullptr;  // Invalidate menu to full redraw
  Draw_Prepare_Menu();
}

void LiveMove() {
  *MenuData.P_Float = MenuData.Value / MINUNITMULT;
  if (!planner.is_full()) {
    planner.synchronize();
    planner.buffer_line(current_position, homing_feedrate(HMI_value.axis));
  }
}
void ApplyMoveE() {
  last_E = MenuData.Value / MINUNITMULT;
  if (!planner.is_full()) {
    planner.synchronize();
    planner.buffer_line(current_position, MMM_TO_MMS(FEEDRATE_E));
  }
}
void SetMoveX() { HMI_value.axis = X_AXIS; SetPFloatOnClick(X_MIN_POS, X_MAX_POS, UNITFDIGITS, planner.synchronize, LiveMove);}
void SetMoveY() { HMI_value.axis = Y_AXIS; SetPFloatOnClick(Y_MIN_POS, Y_MAX_POS, UNITFDIGITS, planner.synchronize, LiveMove);}
void SetMoveZ() { HMI_value.axis = Z_AXIS; SetPFloatOnClick(Z_MIN_POS, Z_MAX_POS, UNITFDIGITS, planner.synchronize, LiveMove);}

#if HAS_HOTEND
  void SetMoveE() {
    #if ENABLED(PREVENT_COLD_EXTRUSION)
      if (thermalManager.tooColdToExtrude(0))
        return DWIN_Popup_Confirm(ICON_TempTooLow, GET_TEXT_F(MSG_HOTEND_TOO_COLD), GET_TEXT_F(MSG_PLEASE_PREHEAT));
    #endif
    SetPFloatOnClick(last_E - (EXTRUDE_MAXLENGTH), last_E + (EXTRUDE_MAXLENGTH), UNITFDIGITS, ApplyMoveE);
  }
#endif

void SetPID(celsius_t t, heater_id_t h) {
  char cmd[48] = "";
  char str_1[5] = "", str_2[5] = "";
  sprintf_P(cmd, PSTR("G28OXY\nG0Z5F300\nG0X%sY%sF5000\nM84"),
    dtostrf(X_CENTER, 1, 1, str_1),
    dtostrf(Y_CENTER, 1, 1, str_2)
  );
  gcode.process_subcommands_now(cmd);
  planner.synchronize();
  thermalManager.PID_autotune(t, h, HMI_data.PidCycles, true);
}
#if HAS_HOTEND
  void HotendPID() { SetPID(HMI_data.HotendPidT, H_E0); }
#endif
#if HAS_HEATED_BED
  void BedPID() { SetPID(HMI_data.BedPidT, H_BED); }
#endif

#if ENABLED(POWER_LOSS_RECOVERY)
  void SetPwrLossr() {
    recovery.enable(!recovery.enabled);
    Draw_Chkb_Line(CurrentMenu->line(), recovery.enabled);
    DWIN_UpdateLCD();
  }
#endif

#if ENABLED(BAUD_RATE_GCODE)
  void HMI_SetBaudRate() {
    if (HMI_data.Baud115K) SetBaud115K(); else SetBaud250K();
  }
  void SetBaudRate() {
    HMI_SetBaudRate();
    Draw_Chkb_Line(CurrentMenu->line(), HMI_data.Baud115K);
    DWIN_UpdateLCD();
  }
  void SetBaud115K() { queue.inject(F("M575 P0 B115200")); HMI_data.Baud115K = true; }
  void SetBaud250K() { queue.inject(F("M575 P0 B250000")); HMI_data.Baud115K = false; }
#endif

#if HAS_LCD_BRIGHTNESS
  void ApplyBrightness() { ui.set_brightness(MenuData.Value); }
  void LiveBrightness() { DWIN_LCD_Brightness(MenuData.Value); }
  void SetBrightness() { SetIntOnClick(LCD_BRIGHTNESS_MIN, LCD_BRIGHTNESS_MAX, ui.brightness, ApplyBrightness, LiveBrightness); }
  void TurnOffBacklight() { HMI_SaveProcessID(WaitResponse); ui.set_brightness(0); DWIN_Redraw_screen(); }
#endif

#if ENABLED(CASE_LIGHT_MENU)
  void SetCaseLight() {
    caselight.on = !caselight.on;
    caselight.update_enabled();
    Draw_Chkb_Line(CurrentMenu->line(), caselight.on);
    DWIN_UpdateLCD();
  }
  #if ENABLED(CASELIGHT_USES_BRIGHTNESS)
    void LiveCaseLightBrightness() { HMI_data.CaseLight_Brightness = caselight.brightness = MenuData.Value; caselight.update_brightness(); }
    void SetCaseLightBrightness() { SetIntOnClick(0, 255, caselight.brightness, nullptr, LiveCaseLightBrightness); }
  #endif
#endif

#if ENABLED(LED_CONTROL_MENU)
  #if !BOTH(CASE_LIGHT_MENU, CASE_LIGHT_USE_NEOPIXEL)
    void SetLedStatus() {
      leds.toggle();
      Draw_Chkb_Line(CurrentMenu->line(), leds.lights_on);
      DWIN_UpdateLCD();
    }
  #endif
  #if ENABLED(HAS_COLOR_LEDS)
    void LiveLedColorR() { leds.color.r = MenuData.Value; HMI_data.Led_Color = leds.color; leds.update(); }
    void SetLedColorR() { SetIntOnClick(0, 255, leds.color.r, nullptr, LiveLedColorR); }
    void LiveLedColorG() { leds.color.g = MenuData.Value; HMI_data.Led_Color = leds.color; leds.update(); }
    void SetLedColorG() { SetIntOnClick(0, 255, leds.color.g, nullptr, LiveLedColorG); }
    void LiveLedColorB() { leds.color.b = MenuData.Value; HMI_data.Led_Color = leds.color; leds.update(); }
    void SetLedColorB() { SetIntOnClick(0, 255, leds.color.b, nullptr, LiveLedColorB); }
    #if HAS_WHITE_LED
      void LiveLedColorW() { leds.color.w = MenuData.Value; HMI_data.Led_Color = leds.color; leds.update(); }
      void SetLedColorW() { SetIntOnClick(0, 255, leds.color.w, nullptr, LiveLedColorW); }
    #endif
  #endif
#endif

#if ENABLED(SOUND_MENU_ITEM)
  void SetEnableSound() {
    ui.buzzer_enabled = !ui.buzzer_enabled;
    Draw_Chkb_Line(CurrentMenu->line(), ui.buzzer_enabled);
    DWIN_UpdateLCD();
  }
#endif

#if HAS_HOME_OFFSET
  void ApplyHomeOffset() { set_home_offset(HMI_value.axis, MenuData.Value / MINUNITMULT); }
  void SetHomeOffsetX() { HMI_value.axis = X_AXIS; SetPFloatOnClick(-50, 50, UNITFDIGITS, ApplyHomeOffset); }
  void SetHomeOffsetY() { HMI_value.axis = Y_AXIS; SetPFloatOnClick(-50, 50, UNITFDIGITS, ApplyHomeOffset); }
  void SetHomeOffsetZ() { HMI_value.axis = Z_AXIS; SetPFloatOnClick( -2,  2, UNITFDIGITS, ApplyHomeOffset); }
#endif

#if HAS_BED_PROBE
  void SetProbeOffsetX() { SetPFloatOnClick(-60, 60, UNITFDIGITS); }
  void SetProbeOffsetY() { SetPFloatOnClick(-60, 60, UNITFDIGITS); }
  void SetProbeOffsetZ() { SetPFloatOnClick(-10, 10, 2); }
  void ProbeTest() {
    LCD_MESSAGE(MSG_M48_TEST);
    queue.inject(F("G28O\nM48 P10"));
  }
  void ProbeStow() { probe.stow(); }
  void ProbeDeploy() { probe.deploy(); }

  #ifdef BLTOUCH_HS_MODE
    void SetHSMode() {
      bltouch.high_speed_mode = !bltouch.high_speed_mode;
      Draw_Chkb_Line(CurrentMenu->line(), bltouch.high_speed_mode);
      DWIN_UpdateLCD();
    }
  #endif

  #if BOTH(HAS_HEATED_BED, PREHEAT_BEFORE_LEVELING)
    void SetBedLevT() { SetPIntOnClick(BED_MINTEMP, BED_MAX_TARGET); }
  #endif

#endif

#if HAS_FILAMENT_SENSOR
  void SetRunoutEnable() {
    runout.reset();
    runout.enabled = !runout.enabled;
    Draw_Chkb_Line(CurrentMenu->line(), runout.enabled);
    DWIN_UpdateLCD();
  }
  #if HAS_FILAMENT_RUNOUT_DISTANCE
    void ApplyRunoutDistance() { runout.set_runout_distance(MenuData.Value / MINUNITMULT); }
    void SetRunoutDistance() { SetFloatOnClick(0, 999, UNITFDIGITS, runout.runout_distance(), ApplyRunoutDistance); }
  #endif
#endif

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  void SetFilLoad()   { SetPFloatOnClick(0, MAX_LOAD_UNLOAD, UNITFDIGITS); }
  void SetFilUnload() { SetPFloatOnClick(0, MAX_LOAD_UNLOAD, UNITFDIGITS); }
#endif

#if ENABLED(PREVENT_COLD_EXTRUSION)
  void ApplyExtMinT() { thermalManager.extrude_min_temp = HMI_data.ExtMinT; thermalManager.allow_cold_extrude = (HMI_data.ExtMinT == 0); }
  void SetExtMinT() { SetPIntOnClick(MIN_ETEMP, MAX_ETEMP, ApplyExtMinT); }
#endif

void RestoreDefaultsColors() {
  DWIN_SetColorDefaults();
  DWINUI::SetColors(HMI_data.Text_Color, HMI_data.Background_Color, HMI_data.StatusBg_Color);
  DWIN_Redraw_screen();
}

void SelColor() {
  MenuData.P_Int = (int16_t*)static_cast<MenuItemPtrClass*>(CurrentMenu->SelectedItem())->value;
  HMI_value.Color[0] = GetRColor(*MenuData.P_Int);  // Red
  HMI_value.Color[1] = GetGColor(*MenuData.P_Int);  // Green
  HMI_value.Color[2] = GetBColor(*MenuData.P_Int);  // Blue
  Draw_GetColor_Menu();
}

void LiveRGBColor() {
    HMI_value.Color[CurrentMenu->line() - 2] = MenuData.Value;
    uint16_t color = RGB(HMI_value.Color[0], HMI_value.Color[1], HMI_value.Color[2]);
    DWIN_Draw_Rectangle(1, color, 20, 315, DWIN_WIDTH - 20, 335);
}
void SetRGBColor() {
  const uint8_t color = CurrentMenu->SelectedItem()->icon;
  SetIntOnClick(0, (color == 1) ? 63 : 31, HMI_value.Color[color], nullptr, LiveRGBColor);
}

void DWIN_ApplyColor() {
  *MenuData.P_Int = RGB(HMI_value.Color[0], HMI_value.Color[1], HMI_value.Color[2]);
  DWINUI::SetColors(HMI_data.Text_Color, HMI_data.Background_Color, HMI_data.StatusBg_Color);
  Draw_SelectColors_Menu();
  hash_changed = true;
  LCD_MESSAGE_F(GET_TEXT_F(MSG_COLORS_APPLIED));
  DWIN_Draw_Dashboard();
}

void SetSpeed() { SetPIntOnClick(MIN_PRINT_SPEED, MAX_PRINT_SPEED); }

#if HAS_HOTEND
  void ApplyHotendTemp() { thermalManager.setTargetHotend(MenuData.Value, 0); }
  void SetHotendTemp() { SetIntOnClick(MIN_ETEMP, MAX_ETEMP, thermalManager.degTargetHotend(0), ApplyHotendTemp); }
#endif

#if HAS_HEATED_BED
  void ApplyBedTemp() { thermalManager.setTargetBed(MenuData.Value); }
  void SetBedTemp() { SetIntOnClick(BED_MINTEMP, BED_MAX_TARGET, thermalManager.degTargetBed(), ApplyBedTemp); }
#endif

#if HAS_FAN
  void ApplyFanSpeed() { thermalManager.set_fan_speed(0, MenuData.Value); }
  void SetFanSpeed() { SetIntOnClick(0, 255, thermalManager.fan_speed[0], ApplyFanSpeed); }
#endif

#if ENABLED(ADVANCED_PAUSE_FEATURE)

  void ChangeFilament() {
    HMI_SaveProcessID(NothingToDo);
    queue.inject(F("M600 B2"));
  }

  void ParkHead(){
    LCD_MESSAGE(MSG_FILAMENT_PARK_ENABLED);
    queue.inject(F("G28O\nG27"));
  }

  #if ENABLED(FILAMENT_LOAD_UNLOAD_GCODES)
    void UnloadFilament(){
      LCD_MESSAGE(MSG_FILAMENTUNLOAD);
      queue.inject(F("M702 Z20"));
    }

    void LoadFilament(){
      LCD_MESSAGE(MSG_FILAMENTLOAD);
      queue.inject(F("M701 Z20"));
    }
  #endif

#endif // ADVANCED_PAUSE_FEATURE

void ApplyFlow() { planner.refresh_e_factor(0); }
void SetFlow() { SetPIntOnClick(MIN_PRINT_FLOW, MAX_PRINT_FLOW, ApplyFlow); }

// Bed Tramming
TERN(HAS_ONESTEP_LEVELING, float, void) Tram(uint8_t point) {
  char cmd[100] = "";
  #if HAS_ONESTEP_LEVELING
    static bool inLev = false;
    float xpos = 0, ypos = 0, zval = 0, margin = 0;
    char str_1[6] = "", str_2[6] = "", str_3[6] = "";
    if (inLev) return NAN;
    margin = HMI_data.FullManualTramming ? 30 : PROBING_MARGIN;
  #else
    int16_t xpos = 0, ypos = 0;
    int16_t margin = 30;
  #endif

  switch (point) {
    case 0:
      LCD_MESSAGE(MSG_LEVBED_FL);
      xpos = ypos = margin;
      break;
    case 1:
      LCD_MESSAGE(MSG_LEVBED_FR);
      xpos = X_BED_SIZE - margin; ypos = margin;
      break;
    case 2:
      LCD_MESSAGE(MSG_LEVBED_BR);
      xpos = X_BED_SIZE - margin; ypos = Y_BED_SIZE - margin;
      break;
    case 3:
      LCD_MESSAGE(MSG_LEVBED_BL);
      xpos = margin; ypos = Y_BED_SIZE - margin;
      break;
    case 4:
      LCD_MESSAGE(MSG_LEVBED_C);
      xpos = X_BED_SIZE / 2; ypos = Y_BED_SIZE / 2;
      break;
  }

  planner.synchronize();

  #if HAS_ONESTEP_LEVELING

    if (HMI_data.FullManualTramming) {
      planner.synchronize();
      sprintf_P(cmd, PSTR("M420S0\nG28O\nG90\nG0Z5F300\nG0X%sY%sF5000\nG0Z0F300"),
        dtostrf(xpos, 1, 1, str_1),
        dtostrf(ypos, 1, 1, str_2)
      );
      queue.inject(cmd);
    }
    else {
      LIMIT(xpos, X_MIN_POS, (X_MAX_POS + probe.offset.x));
      LIMIT(ypos, Y_MIN_POS, (Y_MAX_POS + probe.offset.y));
      probe.stow();
      gcode.process_subcommands_now(F("M420S0\nG28O"));
      planner.synchronize();
      inLev = true;
      zval = probe.probe_at_point(xpos, ypos, PROBE_PT_STOW);
      if (isnan(zval))
        ui.set_status(F("Position Not Reachable, check offsets"));
      else {
        sprintf_P(cmd, PSTR("X:%s, Y:%s, Z:%s"),
          dtostrf(xpos, 1, 1, str_1),
          dtostrf(ypos, 1, 1, str_2),
          dtostrf(zval, 1, 2, str_3)
        );
        ui.set_status(cmd);
      }
      inLev = false;
    }
    return zval;

  #else

    sprintf_P(cmd, PSTR("M420S0\nG28O\nG90\nG0Z5F300\nG0X%iY%iF5000\nG0Z0F300"), xpos, ypos);
    queue.inject(cmd);

  #endif
}

void TramFL() { Tram(0); }
void TramFR() { Tram(1); }
void TramBR() { Tram(2); }
void TramBL() { Tram(3); }
void TramC () { Tram(4); }

#if HAS_ONESTEP_LEVELING

  void Trammingwizard() {
    bed_mesh_t zval = {0};
    if (HMI_data.FullManualTramming) {
      ui.set_status(F("Disable manual tramming"));
      return;
    }
    zval[0][0] = Tram(0);
    checkkey = NothingToDo;
    MeshViewer.DrawMesh(zval, 2, 2);
    zval[1][0] = Tram(1);
    MeshViewer.DrawMesh(zval, 2, 2);
    zval[1][1] = Tram(2);
    MeshViewer.DrawMesh(zval, 2, 2);
    zval[0][1] = Tram(3);
    MeshViewer.DrawMesh(zval, 2, 2);
    char str_1[6] = "", str_2[6] = "";
    ui.status_printf(0, F("Limits minZ: %s, maxZ: %s"),
      dtostrf(MeshViewer.min, 1, 2, str_1),
      dtostrf(MeshViewer.max, 1, 2, str_2)
    );
    if (ABS(MeshViewer.max - MeshViewer.min) < 0.05) {
      DWINUI::Draw_CenteredString(140,F("Corners leveled"));
      DWINUI::Draw_CenteredString(160,F("Tolerance achieved!"));
    }
    else {
      uint8_t p = 0;
      float d, max = 0;
      FSTR_P plabel;
      LOOP_L_N(x,2) LOOP_L_N(y,2) {
        d = ABS(zval[x][y] - MeshViewer.avg);
        if (max < d) {
          max = d;
          p = x + 2 * y;
        }
      }
      switch (p) {
        case 0b00 : plabel = GET_TEXT_F(MSG_LEVBED_FL); break;
        case 0b01 : plabel = GET_TEXT_F(MSG_LEVBED_FR); break;
        case 0b10 : plabel = GET_TEXT_F(MSG_LEVBED_BL); break;
        case 0b11 : plabel = GET_TEXT_F(MSG_LEVBED_BR); break;
        default   : plabel = F(""); break;
      }
      DWINUI::Draw_CenteredString(130, F("Corners not leveled"));
      DWINUI::Draw_CenteredString(150, F("Knob adjustment required"));
      DWINUI::Draw_CenteredString(Color_Green, 170, plabel);
    }
    DWINUI::Draw_Button(BTN_Continue, 86, 305);
    checkkey = Menu;
    HMI_SaveProcessID(WaitResponse);
  }

  void SetManualTramming() {
    HMI_data.FullManualTramming = !HMI_data.FullManualTramming;
    Draw_Chkb_Line(CurrentMenu->line(), HMI_data.FullManualTramming);
    DWIN_UpdateLCD();
  }

#endif // HAS_ONESTEP_LEVELING

#if ENABLED(MESH_BED_LEVELING)

  void ManualMeshStart(){
    LCD_MESSAGE(MSG_UBL_BUILD_MESH_MENU);
    gcode.process_subcommands_now(F("G28XYO\nG28Z\nM211S0\nG29S1"));
    planner.synchronize();
    #ifdef MANUAL_PROBE_START_Z
      const uint8_t line = CurrentMenu->line(MMeshMoveZItem->pos);
      DWINUI::Draw_Signed_Float(HMI_data.Text_Color, HMI_data.Background_Color, 3, 2, VALX - 2 * DWINUI::fontWidth(DWIN_FONT_MENU), MBASE(line), MANUAL_PROBE_START_Z);
    #endif
  }

  void LiveMeshMoveZ() {
    *MenuData.P_Float = MenuData.Value / POW(10, 2);
    if (!planner.is_full()) {
      planner.synchronize();
      planner.buffer_line(current_position, homing_feedrate(Z_AXIS));
    }
  }
  void SetMMeshMoveZ() { SetPFloatOnClick(-1, 1, 2, planner.synchronize, LiveMeshMoveZ);}

  void ManualMeshContinue(){
    gcode.process_subcommands_now(F("G29S2"));
    planner.synchronize();
    MMeshMoveZItem->draw(CurrentMenu->line(MMeshMoveZItem->pos));
  }

  void ManualMeshSave(){
    LCD_MESSAGE(MSG_UBL_STORAGE_MESH_MENU);
    queue.inject(F("M211S1\nM500"));
  }

#endif // MESH_BED_LEVELING

#if HAS_PREHEAT
  #if HAS_HOTEND
    void SetPreheatEndTemp() { SetPIntOnClick(MIN_ETEMP, MAX_ETEMP); }
  #endif
  #if HAS_HEATED_BED
    void SetPreheatBedTemp() { SetPIntOnClick(BED_MINTEMP, BED_MAX_TARGET); }
  #endif
  #if HAS_FAN
    void SetPreheatFanSpeed() { SetPIntOnClick(0, 255); }
  #endif
#endif

void ApplyMaxSpeed() { planner.set_max_feedrate(HMI_value.axis, MenuData.Value / MINUNITMULT); }
void SetMaxSpeedX() { HMI_value.axis = X_AXIS, SetFloatOnClick(MIN_MAXFEEDSPEED, max_feedrate_edit_values[X_AXIS], UNITFDIGITS, planner.settings.max_feedrate_mm_s[X_AXIS], ApplyMaxSpeed); }
void SetMaxSpeedY() { HMI_value.axis = Y_AXIS, SetFloatOnClick(MIN_MAXFEEDSPEED, max_feedrate_edit_values[Y_AXIS], UNITFDIGITS, planner.settings.max_feedrate_mm_s[Y_AXIS], ApplyMaxSpeed); }
void SetMaxSpeedZ() { HMI_value.axis = Z_AXIS, SetFloatOnClick(MIN_MAXFEEDSPEED, max_feedrate_edit_values[Z_AXIS], UNITFDIGITS, planner.settings.max_feedrate_mm_s[Z_AXIS], ApplyMaxSpeed); }
#if HAS_HOTEND
  void SetMaxSpeedE() { HMI_value.axis = E_AXIS; SetFloatOnClick(MIN_MAXFEEDSPEED, max_feedrate_edit_values[E_AXIS], UNITFDIGITS, planner.settings.max_feedrate_mm_s[E_AXIS], ApplyMaxSpeed); }
#endif

void ApplyMaxAccel() { planner.set_max_acceleration(HMI_value.axis, MenuData.Value); }
void SetMaxAccelX() { HMI_value.axis = X_AXIS, SetIntOnClick(MIN_MAXACCELERATION, max_acceleration_edit_values[X_AXIS], planner.settings.max_acceleration_mm_per_s2[X_AXIS], ApplyMaxAccel); }
void SetMaxAccelY() { HMI_value.axis = Y_AXIS, SetIntOnClick(MIN_MAXACCELERATION, max_acceleration_edit_values[Y_AXIS], planner.settings.max_acceleration_mm_per_s2[Y_AXIS], ApplyMaxAccel); }
void SetMaxAccelZ() { HMI_value.axis = Z_AXIS, SetIntOnClick(MIN_MAXACCELERATION, max_acceleration_edit_values[Z_AXIS], planner.settings.max_acceleration_mm_per_s2[Z_AXIS], ApplyMaxAccel); }
#if HAS_HOTEND
  void SetMaxAccelE() { HMI_value.axis = E_AXIS; SetIntOnClick(MIN_MAXACCELERATION, max_acceleration_edit_values[E_AXIS], planner.settings.max_acceleration_mm_per_s2[E_AXIS], ApplyMaxAccel); }
#endif

#if HAS_CLASSIC_JERK
  void ApplyMaxJerk() { planner.set_max_jerk(HMI_value.axis, MenuData.Value / MINUNITMULT); }
  void SetMaxJerkX() { HMI_value.axis = X_AXIS, SetFloatOnClick(MIN_MAXJERK, max_jerk_edit_values[X_AXIS], UNITFDIGITS, planner.max_jerk[X_AXIS], ApplyMaxJerk); }
  void SetMaxJerkY() { HMI_value.axis = Y_AXIS, SetFloatOnClick(MIN_MAXJERK, max_jerk_edit_values[Y_AXIS], UNITFDIGITS, planner.max_jerk[Y_AXIS], ApplyMaxJerk); }
  void SetMaxJerkZ() { HMI_value.axis = Z_AXIS, SetFloatOnClick(MIN_MAXJERK, max_jerk_edit_values[Z_AXIS], UNITFDIGITS, planner.max_jerk[Z_AXIS], ApplyMaxJerk); }
  #if HAS_HOTEND
    void SetMaxJerkE() { HMI_value.axis = E_AXIS; SetFloatOnClick(MIN_MAXJERK, max_jerk_edit_values[E_AXIS], UNITFDIGITS, planner.max_jerk[E_AXIS], ApplyMaxJerk); }
  #endif
#endif

void SetStepsX() { HMI_value.axis = X_AXIS, SetPFloatOnClick( MIN_STEP, MAX_STEP, UNITFDIGITS); }
void SetStepsY() { HMI_value.axis = Y_AXIS, SetPFloatOnClick( MIN_STEP, MAX_STEP, UNITFDIGITS); }
void SetStepsZ() { HMI_value.axis = Z_AXIS, SetPFloatOnClick( MIN_STEP, MAX_STEP, UNITFDIGITS); }
#if HAS_HOTEND
  void SetStepsE() { HMI_value.axis = E_AXIS; SetPFloatOnClick( MIN_STEP, MAX_STEP, UNITFDIGITS); }
  void SetHotendPidT() { SetPIntOnClick(MIN_ETEMP, MAX_ETEMP); }
#endif
#if HAS_HEATED_BED
  void SetBedPidT() { SetPIntOnClick(BED_MINTEMP, BED_MAX_TARGET); }
#endif

#if HAS_HOTEND || HAS_HEATED_BED
  void SetPidCycles() { SetPIntOnClick(3, 50); }
  void SetKp() { SetPFloatOnClick(0, 1000, 2); }
  void ApplyPIDi() {
    *MenuData.P_Float = scalePID_i(MenuData.Value / POW(10, 2));
    thermalManager.updatePID();
  }
  void ApplyPIDd() {
    *MenuData.P_Float = scalePID_d(MenuData.Value / POW(10, 2));
    thermalManager.updatePID();
  }
  void SetKi() {
    MenuData.P_Float = (float*)static_cast<MenuItemPtrClass*>(CurrentMenu->SelectedItem())->value;
    const float value = unscalePID_i(*MenuData.P_Float);
    SetFloatOnClick(0, 1000, 2, value, ApplyPIDi);
  }
  void SetKd() {
    MenuData.P_Float = (float*)static_cast<MenuItemPtrClass*>(CurrentMenu->SelectedItem())->value;
    const float value = unscalePID_d(*MenuData.P_Float);
    SetFloatOnClick(0, 1000, 2, value, ApplyPIDd);
  }
#endif

#if ENABLED(FWRETRACT)
  void SetRetractLength() { SetPFloatOnClick( 0, 10, UNITFDIGITS); };
  void SetRetractSpeed() { SetPFloatOnClick( 1, 90, UNITFDIGITS); };
  void SetZRaise() { SetPFloatOnClick( 0, 2, 2); };
  void SetRecoverSpeed() { SetPFloatOnClick( 1, 90, UNITFDIGITS); };
#endif

// Special Menuitem Drawing functions =================================================

void onDrawBack(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 129, 72, 156, 84);
  onDrawMenuItem(menuitem, line);
}

void onDrawTempSubMenu(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1,  57, 104,  84, 116);
  onDrawSubMenu(menuitem, line);
}

void onDrawMotionSubMenu(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1,  87, 104, 114, 116);
  onDrawSubMenu(menuitem, line);
}

#if ENABLED(EEPROM_SETTINGS)
  void onDrawWriteEeprom(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 117, 104, 172, 116);
    onDrawMenuItem(menuitem, line);
  }

  void onDrawReadEeprom(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 174, 103, 229, 116);
    onDrawMenuItem(menuitem, line);
  }

  void onDrawResetEeprom(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1,   1, 118,  56, 131);
    onDrawMenuItem(menuitem, line);
  }
#endif

void onDrawInfoSubMenu(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 231, 104, 258, 116);
  onDrawSubMenu(menuitem, line);
}

void onDrawMoveX(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 58, 118, 106, 132);
  onDrawPFloatMenu(menuitem, line);
}

void onDrawMoveY(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 109, 118, 157, 132);
  onDrawPFloatMenu(menuitem, line);
}

void onDrawMoveZ(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 160, 118, 209, 132);
  onDrawPFloatMenu(menuitem, line);
}

#if HAS_HOTEND
  void onDrawMoveE(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 212, 118, 253, 131);
    onDrawPFloatMenu(menuitem, line);
  }
#endif

void onDrawMoveSubMenu(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 159, 70, 200, 84);
  onDrawSubMenu(menuitem, line);
}

void onDrawDisableMotors(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 204, 70, 259, 82);
  onDrawMenuItem(menuitem, line);
}

void onDrawAutoHome(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 0, 89, 41, 101);
  onDrawMenuItem(menuitem, line);
}

#if HAS_ZOFFSET_ITEM
  #if EITHER(HAS_BED_PROBE, BABYSTEPPING)
    void onDrawZOffset(MenuItemClass* menuitem, int8_t line) {
      if (HMI_IsChinese()) menuitem->SetFrame(1, 174, 164, 223, 177);
      onDrawPFloat2Menu(menuitem, line);
    }
  #else
    void onDrawHomeOffset(MenuItemClass* menuitem, int8_t line) {
      if (HMI_IsChinese()) menuitem->SetFrame(1, 43, 89, 98, 101);
      onDrawMenuItem(menuitem, line);
    }
  #endif
#endif

#if HAS_HOTEND
  void onDrawPreheat1(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 100, 89, 151, 101);
    onDrawMenuItem(menuitem, line);
  }
  void onDrawPreheat2(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 180, 89, 233, 100);
    onDrawMenuItem(menuitem, line);
  }
#endif

#if HAS_PREHEAT
  void onDrawCooldown(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 1, 104,  56, 117);
    onDrawMenuItem(menuitem, line);
  }
#endif

void onDrawLanguage(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 239, 134, 266, 146);
  onDrawMenuItem(menuitem, line);
  DWINUI::Draw_String(VALX, MBASE(line), HMI_IsChinese() ? F("CN") : F("EN"));
}

#if ENABLED(POWER_LOSS_RECOVERY)
  void onDrawPwrLossR(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, recovery.enabled); }
#endif

#if ENABLED(BAUD_RATE_GCODE)
  void onDrawBaudrate(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, HMI_data.Baud115K); }
#endif

#if ENABLED(CASE_LIGHT_MENU)
  void onDrawCaseLight(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, caselight.on); }
#endif

#if ENABLED(LED_CONTROL_MENU)
  #if !BOTH(CASE_LIGHT_MENU, CASE_LIGHT_USE_NEOPIXEL)
    void onDrawLedStatus(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, leds.lights_on); }
  #endif
#endif

#if ENABLED(SOUND_MENU_ITEM)
  void onDrawEnableSound(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, ui.buzzer_enabled); }
#endif

#ifdef BLTOUCH_HS_MODE
  void onDrawHSMode(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, bltouch.high_speed_mode); }
#endif

void onDrawSelColorItem(MenuItemClass* menuitem, int8_t line) {
  const uint16_t color = *(uint16_t*)static_cast<MenuItemPtrClass*>(menuitem)->value;
  DWIN_Draw_Rectangle(0, HMI_data.Highlight_Color, ICOX + 1, MBASE(line) - 1 + 1, ICOX + 18, MBASE(line) - 1 + 18);
  DWIN_Draw_Rectangle(1, color, ICOX + 2, MBASE(line) - 1 + 2, ICOX + 17, MBASE(line) - 1 + 17);
  onDrawMenuItem(menuitem, line);
}

void onDrawGetColorItem(MenuItemClass* menuitem, int8_t line) {
  const uint8_t i = menuitem->icon;
  uint16_t color;
  switch (i) {
    case 0: color = RGB(31, 0, 0); break; // Red
    case 1: color = RGB(0, 63, 0); break; // Green
    case 2: color = RGB(0, 0, 31); break; // Blue
    default: color = 0; break;
  }
  DWIN_Draw_Rectangle(0, HMI_data.Highlight_Color, ICOX + 1, MBASE(line) - 1 + 1, ICOX + 18, MBASE(line) - 1 + 18);
  DWIN_Draw_Rectangle(1, color, ICOX + 2, MBASE(line) - 1 + 2, ICOX + 17, MBASE(line) - 1 + 17);
  DWINUI::Draw_String(LBLX, MBASE(line) - 1, menuitem->caption);
  Draw_Menu_IntValue(HMI_data.Background_Color, line, 4, HMI_value.Color[i]);
  DWIN_Draw_HLine(HMI_data.SplitLine_Color, 16, MYPOS(line + 1), 240);
}

#if HAS_FILAMENT_SENSOR
  void onDrawRunoutEnable(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, runout.enabled); }
#endif

void onDrawPIDi(MenuItemClass* menuitem, int8_t line) { onDrawFloatMenu(menuitem, line, 2, unscalePID_i(*(float*)static_cast<MenuItemPtrClass*>(menuitem)->value)); }
void onDrawPIDd(MenuItemClass* menuitem, int8_t line) { onDrawFloatMenu(menuitem, line, 2, unscalePID_d(*(float*)static_cast<MenuItemPtrClass*>(menuitem)->value)); }

void onDrawSpeedItem(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 116, 164, 171, 176);
  onDrawPIntMenu(menuitem, line);
}

#if HAS_HOTEND
  void onDrawHotendTemp(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 1, 134, 56, 146);
    onDrawPIntMenu(menuitem, line);
  }
#endif

#if HAS_HEATED_BED
  void onDrawBedTemp(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 58, 134, 113, 146);
    onDrawPIntMenu(menuitem, line);
  }
#endif

#if HAS_FAN
  void onDrawFanSpeed(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 115, 134, 170, 146);
    onDrawPInt8Menu(menuitem, line);
  }
#endif

void onDrawSteps(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) menuitem->SetFrame(1, 153, 148, 194, 161);
  onDrawSubMenu(menuitem, line);
}

#if ENABLED(MESH_BED_LEVELING)
  void onDrawMMeshMoveZ(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 160, 118, 209, 132);
    onDrawPFloat2Menu(menuitem, line);
  }
#endif

#if HAS_PREHEAT
  #if HAS_HOTEND
    void onDrawSetPreheatHotend(MenuItemClass* menuitem, int8_t line) {
      if (HMI_IsChinese()) menuitem->SetFrame(1, 1, 134, 56, 146);
      onDrawPIntMenu(menuitem, line);
    }
  #endif
  #if HAS_HEATED_BED
    void onDrawSetPreheatBed(MenuItemClass* menuitem, int8_t line) {
      if (HMI_IsChinese()) menuitem->SetFrame(1, 58, 134, 113, 146);
      onDrawPIntMenu(menuitem, line);
    }
  #endif
  #if HAS_FAN
    void onDrawSetPreheatFan(MenuItemClass* menuitem, int8_t line) {
      if (HMI_IsChinese()) menuitem->SetFrame(1, 115, 134, 170, 146);
      onDrawPIntMenu(menuitem, line);
    }
  #endif
  void onDrawPLAPreheatSubMenu(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 100, 89, 178, 101);
    onDrawSubMenu(menuitem,line);
  }
  void onDrawABSPreheatSubMenu(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) menuitem->SetFrame(1, 180, 89, 260, 100);
    onDrawSubMenu(menuitem,line);
  }
#endif // HAS_PREHEAT

void onDrawSpeed(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese())
    menuitem->SetFrame(1, 173, 133, 228, 147);
  onDrawSubMenu(menuitem, line);
}

void onDrawMaxSpeedX(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 228, 147);
    DWIN_Frame_AreaCopy(1, 229, 133, 236, 147, LBLX + 58, MBASE(line));     // X
  }
  onDrawPFloatMenu(menuitem, line);
}

void onDrawMaxSpeedY(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 228, 147);
    DWIN_Frame_AreaCopy(1, 1, 150, 7, 160, LBLX + 58, MBASE(line));         // Y
  }
  onDrawPFloatMenu(menuitem, line);
}

void onDrawMaxSpeedZ(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 228, 147);
    DWIN_Frame_AreaCopy(1, 9, 150, 16, 160, LBLX + 58, MBASE(line) + 3);    // Z
  }
  onDrawPFloatMenu(menuitem, line);
}

#if HAS_HOTEND
  void onDrawMaxSpeedE(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 173, 133, 228, 147);
      DWIN_Frame_AreaCopy(1, 18, 150, 25, 160, LBLX + 58, MBASE(line));     // E
    }
    onDrawPFloatMenu(menuitem, line);
  }
#endif

void onDrawAcc(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 200, 147);
    DWIN_Frame_AreaCopy(1, 28, 149, 69, 161, LBLX + 27, MBASE(line) + 1);   // ...Acceleration
  }
  onDrawSubMenu(menuitem, line);
}

void onDrawMaxAccelX(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 200, 147);
    DWIN_Frame_AreaCopy(1, 28,  149,  69, 161, LBLX + 27, MBASE(line));
    DWIN_Frame_AreaCopy(1, 229, 133, 236, 147, LBLX + 71, MBASE(line));     // X
  }
  onDrawPInt32Menu(menuitem, line);
}

void onDrawMaxAccelY(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 200, 147);
    DWIN_Frame_AreaCopy(1, 28, 149,  69, 161, LBLX + 27, MBASE(line));
    DWIN_Frame_AreaCopy(1,  1, 150,   7, 160, LBLX + 71, MBASE(line));      // Y
  }
  onDrawPInt32Menu(menuitem, line);
}

void onDrawMaxAccelZ(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 173, 133, 200, 147);
    DWIN_Frame_AreaCopy(1, 28, 149,  69, 161, LBLX + 27, MBASE(line));
    DWIN_Frame_AreaCopy(1,  9, 150,  16, 160, LBLX + 71, MBASE(line));      // Z
  }
  onDrawPInt32Menu(menuitem, line);
}

#if HAS_HOTEND
  void onDrawMaxAccelE(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 173, 133, 200, 147);
      DWIN_Frame_AreaCopy(1, 28, 149,  69, 161, LBLX + 27, MBASE(line));
      DWIN_Frame_AreaCopy(1, 18, 150,  25, 160, LBLX + 71, MBASE(line));    // E
    }
    onDrawPInt32Menu(menuitem, line);
  }
#endif

#if HAS_CLASSIC_JERK

  void onDrawJerk(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 173, 133, 200, 147);
      DWIN_Frame_AreaCopy(1, 1, 180, 28, 192, LBLX + 27, MBASE(line) + 1);  // ...
      DWIN_Frame_AreaCopy(1, 202, 133, 228, 147, LBLX + 54, MBASE(line));   // ...Jerk
    }
    onDrawSubMenu(menuitem, line);
  }

  void onDrawMaxJerkX(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 173, 133, 200, 147);
      DWIN_Frame_AreaCopy(1,   1, 180,  28, 192, LBLX + 27, MBASE(line));
      DWIN_Frame_AreaCopy(1, 202, 133, 228, 147, LBLX + 53, MBASE(line));
      DWIN_Frame_AreaCopy(1, 229, 133, 236, 147, LBLX + 83, MBASE(line));
    }
    onDrawPFloatMenu(menuitem, line);
  }

  void onDrawMaxJerkY(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 173, 133, 200, 147);
      DWIN_Frame_AreaCopy(1,   1, 180,  28, 192, LBLX + 27, MBASE(line));
      DWIN_Frame_AreaCopy(1, 202, 133, 228, 147, LBLX + 53, MBASE(line));
      DWIN_Frame_AreaCopy(1,   1, 150,   7, 160, LBLX + 83, MBASE(line));
    }
    onDrawPFloatMenu(menuitem, line);
  }

  void onDrawMaxJerkZ(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 173, 133, 200, 147);
      DWIN_Frame_AreaCopy(1,   1, 180,  28, 192, LBLX + 27, MBASE(line));
      DWIN_Frame_AreaCopy(1, 202, 133, 228, 147, LBLX + 53, MBASE(line));
      DWIN_Frame_AreaCopy(1,   9, 150,  16, 160, LBLX + 83, MBASE(line));
    }
    onDrawPFloatMenu(menuitem, line);
  }

  #if HAS_HOTEND
    void onDrawMaxJerkE(MenuItemClass* menuitem, int8_t line) {
      if (HMI_IsChinese()) {
        menuitem->SetFrame(1, 173, 133, 200, 147);
        DWIN_Frame_AreaCopy(1,   1, 180,  28, 192, LBLX + 27, MBASE(line));
        DWIN_Frame_AreaCopy(1, 202, 133, 228, 147, LBLX + 53, MBASE(line));
        DWIN_Frame_AreaCopy(1,  18, 150,  25, 160, LBLX + 83, MBASE(line));
      }
      onDrawPFloatMenu(menuitem, line);
    }
  #endif

#endif // HAS_CLASSIC_JERK

void onDrawStepsX(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 153, 148, 194, 161);
    DWIN_Frame_AreaCopy(1, 229, 133, 236, 147, LBLX + 44, MBASE(line));      // X
  }
  onDrawPFloatMenu(menuitem, line);
}

void onDrawStepsY(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 153, 148, 194, 161);
    DWIN_Frame_AreaCopy(1,   1, 150,   7, 160, LBLX + 44, MBASE(line));      // Y
  }
  onDrawPFloatMenu(menuitem, line);
}

void onDrawStepsZ(MenuItemClass* menuitem, int8_t line) {
  if (HMI_IsChinese()) {
    menuitem->SetFrame(1, 153, 148, 194, 161);
    DWIN_Frame_AreaCopy(1,   9, 150,  16, 160, LBLX + 44, MBASE(line));      // Z
  }
  onDrawPFloatMenu(menuitem, line);
}

#if HAS_HOTEND
  void onDrawStepsE(MenuItemClass* menuitem, int8_t line) {
    if (HMI_IsChinese()) {
      menuitem->SetFrame(1, 153, 148, 194, 161);
      DWIN_Frame_AreaCopy(1,  18, 150,  25, 160, LBLX + 44, MBASE(line));    // E
    }
    onDrawPFloatMenu(menuitem, line);
  }
#endif

#if HAS_ONESTEP_LEVELING
  void onDrawManualTramming(MenuItemClass* menuitem, int8_t line) { onDrawChkbMenu(menuitem, line, HMI_data.FullManualTramming); }
#endif

// Menu Creation and Drawing functions ======================================================

void SetMenuTitle(frame_rect_t cn, const __FlashStringHelper* fstr) {
  if (HMI_IsChinese() && (cn.w != 0))
    CurrentMenu->MenuTitle.SetFrame(cn.x, cn.y, cn.w, cn.h);
  else
    CurrentMenu->MenuTitle.SetCaption(fstr);
}

void Draw_Prepare_Menu() {
  checkkey = Menu;
  if (!PrepareMenu) PrepareMenu = new MenuClass();
  if (CurrentMenu != PrepareMenu) {
    CurrentMenu = PrepareMenu;
    SetMenuTitle({133, 1, 28, 13}, GET_TEXT_F(MSG_PREPARE));
    DWINUI::MenuItemsPrepare(13);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Goto_Main_Menu);
    #if ENABLED(ADVANCED_PAUSE_FEATURE)
      MENU_ITEM(ICON_FilMan, GET_TEXT_F(MSG_FILAMENT_MAN), onDrawSubMenu, Draw_FilamentMan_Menu);
    #endif
    MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_MOVE_AXIS), onDrawMoveSubMenu, Goto_Move_Menu);
    MENU_ITEM(ICON_Tram, GET_TEXT_F(MSG_BED_TRAMMING), onDrawSubMenu, Draw_Tramming_Menu);
    MENU_ITEM(ICON_CloseMotor, GET_TEXT_F(MSG_DISABLE_STEPPERS), onDrawDisableMotors, DisableMotors);
    #if ENABLED(INDIVIDUAL_AXIS_HOMING_SUBMENU)
      MENU_ITEM(ICON_Homing, GET_TEXT_F(MSG_HOMING), onDrawSubMenu, Draw_Homing_Menu);
    #else
      MENU_ITEM(ICON_Homing, GET_TEXT_F(MSG_AUTO_HOME), onDrawAutoHome, AutoHome);
    #endif
    #if ENABLED(MESH_BED_LEVELING)
      MENU_ITEM(ICON_ManualMesh, GET_TEXT_F(MSG_MANUAL_MESH), onDrawSubMenu, Draw_ManualMesh_Menu);
    #endif
    #if HAS_ONESTEP_LEVELING
      MENU_ITEM(ICON_Level, GET_TEXT_F(MSG_AUTO_MESH), onDrawMenuItem, AutoLev);
    #endif
    #if HAS_ZOFFSET_ITEM
      #if HAS_BED_PROBE
        MENU_ITEM(ICON_SetZOffset, GET_TEXT_F(MSG_PROBE_WIZARD), onDrawSubMenu, Draw_ZOffsetWiz_Menu);
      #elif ENABLED(BABYSTEPPING)
        EDIT_ITEM(ICON_Zoffset, GET_TEXT_F(MSG_ZPROBE_ZOFFSET), onDrawPFloat2Menu, SetZOffset, &BABY_Z_VAR);
      #else
        MENU_ITEM(ICON_SetHome, GET_TEXT_F(MSG_SET_HOME_OFFSETS), onDrawHomeOffset, SetHome);
      #endif
    #endif
    #if HAS_PREHEAT
      MENU_ITEM(ICON_PLAPreheat, GET_TEXT_F(MSG_PREHEAT_1), onDrawPreheat1, DoPreheat0);
      #if PREHEAT_COUNT > 1
        MENU_ITEM(ICON_ABSPreheat, PSTR("Preheat " PREHEAT_2_LABEL), onDrawPreheat2, DoPreheat1);
      #endif
      #if PREHEAT_COUNT > 2
        MENU_ITEM(ICON_CustomPreheat, GET_TEXT_F(MSG_PREHEAT_CUSTOM), onDrawMenuItem, DoPreheat2);
      #endif
    #endif
    MENU_ITEM(ICON_Cool, GET_TEXT_F(MSG_COOLDOWN), onDrawCooldown, DoCoolDown);
    MENU_ITEM(ICON_Language, PSTR(GET_TEXT_F(MSG_UI_LANGUAGE)), onDrawLanguage, SetLanguage);
  }
  ui.reset_status(true);
  CurrentMenu->draw();
}

void Draw_Tramming_Menu() {
  DWINUI::ClearMenuArea();
  checkkey = Menu;
  if (!TrammingMenu) TrammingMenu = new MenuClass();
  if (CurrentMenu != TrammingMenu) {
    CurrentMenu = TrammingMenu;
    SetMenuTitle({0}, GET_TEXT_F(MSG_BED_TRAMMING)); // TODO: Chinese, English "Bed Tramming" JPG
    DWINUI::MenuItemsPrepare(8);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Prepare_Menu);
    MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_LEVBED_FL), onDrawMenuItem, TramFL);
    MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_LEVBED_FR), onDrawMenuItem, TramFR);
    MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_LEVBED_BR), onDrawMenuItem, TramBR);
    MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_LEVBED_BL), onDrawMenuItem, TramBL);
    MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_LEVBED_C ), onDrawMenuItem, TramC );
    #if HAS_ONESTEP_LEVELING
      MENU_ITEM(ICON_ProbeSet, F("Bed tramming wizard"), onDrawMenuItem, Trammingwizard);
      MENU_ITEM(ICON_ProbeSet, GET_TEXT_F(MSG_BED_TRAMMING_MANUAL), onDrawManualTramming, SetManualTramming);
    #endif
  }
  CurrentMenu->draw();
}

void Draw_Control_Menu() {
  checkkey = Menu;
  if (!ControlMenu) ControlMenu = new MenuClass();
  if (CurrentMenu != ControlMenu) {
    CurrentMenu = ControlMenu;
    SetMenuTitle({103, 1, 28, 14}, GET_TEXT_F(MSG_CONTROL));
    DWINUI::MenuItemsPrepare(10);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Goto_Main_Menu);
    #if ENABLED(CASE_LIGHT_MENU)
      #if ENABLED(CASELIGHT_USES_BRIGHTNESS)
        MENU_ITEM(ICON_CaseLight, GET_TEXT_F(MSG_CASE_LIGHT), onDrawSubMenu, Draw_CaseLight_Menu);
      #else
        MENU_ITEM(ICON_CaseLight, GET_TEXT_F(MSG_CASE_LIGHT), onDrawCaseLight, SetCaseLight);
      #endif
    #endif
    #if ENABLED(LED_CONTROL_MENU)
      MENU_ITEM(ICON_LedControl, GET_TEXT_F(MSG_LED_CONTROL), onDrawSubMenu, Draw_LedControl_Menu);
    #endif
    MENU_ITEM(ICON_Temperature, GET_TEXT_F(MSG_TEMPERATURE), onDrawTempSubMenu, Draw_Temperature_Menu);
    MENU_ITEM(ICON_Motion, GET_TEXT_F(MSG_MOTION), onDrawMotionSubMenu, Draw_Motion_Menu);
    #if ENABLED(EEPROM_SETTINGS)
      MENU_ITEM(ICON_WriteEEPROM, GET_TEXT_F(MSG_STORE_EEPROM), onDrawWriteEeprom, WriteEeprom);
      MENU_ITEM(ICON_ReadEEPROM, GET_TEXT_F(MSG_LOAD_EEPROM), onDrawReadEeprom, ReadEeprom);
      MENU_ITEM(ICON_ResumeEEPROM, GET_TEXT_F(MSG_RESTORE_DEFAULTS), onDrawResetEeprom, ResetEeprom);
    #endif
    MENU_ITEM(ICON_Reboot, GET_TEXT_F(MSG_RESET_PRINTER), onDrawMenuItem, RebootPrinter);
    MENU_ITEM(ICON_Info, GET_TEXT_F(MSG_INFO_SCREEN), onDrawInfoSubMenu, Goto_Info_Menu);
  }
  ui.reset_status(true);
  CurrentMenu->draw();
}

void Draw_AdvancedSettings_Menu() {
  checkkey = Menu;
  if (!AdvancedSettings) AdvancedSettings = new MenuClass();
  if (CurrentMenu != AdvancedSettings) {
    CurrentMenu = AdvancedSettings;
    SetMenuTitle({0}, GET_TEXT_F(MSG_ADVANCED_SETTINGS)); // TODO: Chinese, English "Advanced Settings" JPG
    DWINUI::MenuItemsPrepare(17);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Goto_Main_Menu);
    #if ENABLED(EEPROM_SETTINGS)
      MENU_ITEM(ICON_WriteEEPROM, GET_TEXT_F(MSG_STORE_EEPROM), onDrawMenuItem, WriteEeprom);
    #endif
    #if HAS_HOME_OFFSET
      MENU_ITEM(ICON_HomeOffset, GET_TEXT_F(MSG_SET_HOME_OFFSETS), onDrawSubMenu, Draw_HomeOffset_Menu);
    #endif
    #if HAS_BED_PROBE
      MENU_ITEM(ICON_ProbeSet, GET_TEXT_F(MSG_ZPROBE_SETTINGS), onDrawSubMenu, Draw_ProbeSet_Menu);
    #endif
    #if HAS_HOTEND
      MENU_ITEM(ICON_PIDNozzle, F("Hotend PID Settings"), onDrawSubMenu, Draw_HotendPID_Menu);
    #endif
    #if HAS_HEATED_BED
      MENU_ITEM(ICON_PIDbed, F("Bed PID Settings"), onDrawSubMenu, Draw_BedPID_Menu);
    #endif
    #if HAS_FILAMENT_SENSOR
      MENU_ITEM(ICON_FilSet, GET_TEXT_F(MSG_FILAMENT_SET), onDrawSubMenu, Draw_FilSet_Menu);
    #endif
    #if ENABLED(POWER_LOSS_RECOVERY)
      MENU_ITEM(ICON_Pwrlossr, GET_TEXT_F(MSG_OUTAGE_RECOVERY), onDrawPwrLossR, SetPwrLossr);
    #endif
    #if ENABLED(BAUD_RATE_GCODE)
      MENU_ITEM(ICON_SetBaudRate, F("115K baud"), onDrawBaudrate, SetBaudRate);
    #endif
    #if HAS_LCD_BRIGHTNESS
      EDIT_ITEM(ICON_Brightness, GET_TEXT_F(MSG_BRIGHTNESS), onDrawPInt8Menu, SetBrightness, &ui.brightness);
    #endif
    MENU_ITEM(ICON_Scolor, GET_TEXT_F(MSG_COLORS_SELECT), onDrawSubMenu, Draw_SelectColors_Menu);
    #if ENABLED(SOUND_MENU_ITEM)
      MENU_ITEM(ICON_Sound, GET_TEXT_F(MSG_SOUND_ENABLE), onDrawEnableSound, SetEnableSound);
    #endif
    #if HAS_MESH
      MENU_ITEM(ICON_MeshViewer, GET_TEXT_F(MSG_MESH_VIEW), onDrawSubMenu, DWIN_MeshViewer);
    #endif
    #if HAS_ESDIAG
      MENU_ITEM(ICON_ESDiag, F("End-stops diag."), onDrawSubMenu, Draw_EndStopDiag);
    #endif
    #if ENABLED(PRINTCOUNTER)
      MENU_ITEM(ICON_PrintStats, GET_TEXT_F(MSG_INFO_STATS_MENU), onDrawSubMenu, Goto_PrintStats);
      MENU_ITEM(ICON_PrintStatsReset, GET_TEXT_F(MSG_INFO_PRINT_COUNT_RESET), onDrawSubMenu, PrintStats.Reset);
    #endif
    MENU_ITEM(ICON_Lock, GET_TEXT_F(MSG_LOCKSCREEN), onDrawMenuItem, DWIN_LockScreen);
  }
  ui.reset_status(true);
  CurrentMenu->draw();
}

void Draw_Move_Menu() {
  checkkey = Menu;
  if (!MoveMenu) MoveMenu = new MenuClass();
  if (CurrentMenu != MoveMenu) {
    CurrentMenu = MoveMenu;
    SetMenuTitle({192, 1, 42, 14}, GET_TEXT_F(MSG_MOVE_AXIS));
    DWINUI::MenuItemsPrepare(5);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Prepare_Menu);
    EDIT_ITEM(ICON_MoveX, GET_TEXT_F(MSG_MOVE_X), onDrawMoveX, SetMoveX, &current_position.x);
    EDIT_ITEM(ICON_MoveY, GET_TEXT_F(MSG_MOVE_Y), onDrawMoveY, SetMoveY, &current_position.y);
    EDIT_ITEM(ICON_MoveZ, GET_TEXT_F(MSG_MOVE_Z), onDrawMoveZ, SetMoveZ, &current_position.z);
    #if HAS_HOTEND
      EDIT_ITEM(ICON_Extruder, GET_TEXT_F(MSG_MOVE_E), onDrawMoveE, SetMoveE, &current_position.e);
    #endif
  }
  CurrentMenu->draw();
  if (!all_axes_trusted()) LCD_MESSAGE_F("WARNING: current position is unknown, home axes");
}

#if HAS_HOME_OFFSET
  void Draw_HomeOffset_Menu() {
    checkkey = Menu;
    if (!HomeOffMenu) HomeOffMenu = new MenuClass();
    if (CurrentMenu != HomeOffMenu) {
      CurrentMenu = HomeOffMenu;
      SetMenuTitle({0}, GET_TEXT_F(MSG_SET_HOME_OFFSETS)); // TODO: Chinese, English "Set Home Offsets" JPG
      DWINUI::MenuItemsPrepare(4);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_AdvancedSettings_Menu);
      EDIT_ITEM(ICON_HomeOffsetX, GET_TEXT_F(MSG_HOME_OFFSET_X), onDrawPFloatMenu, SetHomeOffsetX, &home_offset[X_AXIS]);
      EDIT_ITEM(ICON_HomeOffsetY, GET_TEXT_F(MSG_HOME_OFFSET_Y), onDrawPFloatMenu, SetHomeOffsetY, &home_offset[Y_AXIS]);
      EDIT_ITEM(ICON_HomeOffsetZ, GET_TEXT_F(MSG_HOME_OFFSET_Z), onDrawPFloatMenu, SetHomeOffsetZ, &home_offset[Z_AXIS]);
    }
    CurrentMenu->draw();
  }
#endif

#if HAS_BED_PROBE
  void Draw_ProbeSet_Menu() {
    checkkey = Menu;
    if (!ProbeSetMenu) ProbeSetMenu = new MenuClass();
    if (CurrentMenu != ProbeSetMenu) {
      CurrentMenu = ProbeSetMenu;
      SetMenuTitle({0}, GET_TEXT_F(MSG_ZPROBE_SETTINGS)); // TODO: Chinese, English "Probe Settings" JPG
      DWINUI::MenuItemsPrepare(9);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_AdvancedSettings_Menu);
      EDIT_ITEM(ICON_ProbeOffsetX, GET_TEXT_F(MSG_ZPROBE_XOFFSET), onDrawPFloatMenu, SetProbeOffsetX, &probe.offset.x);
      EDIT_ITEM(ICON_ProbeOffsetY, GET_TEXT_F(MSG_ZPROBE_YOFFSET), onDrawPFloatMenu, SetProbeOffsetY, &probe.offset.y);
      EDIT_ITEM(ICON_ProbeOffsetZ, GET_TEXT_F(MSG_ZPROBE_ZOFFSET), onDrawPFloat2Menu, SetProbeOffsetZ, &probe.offset.z);
      #if BOTH(HAS_HEATED_BED, PREHEAT_BEFORE_LEVELING)
        EDIT_ITEM(ICON_Temperature, GET_TEXT_F(MSG_UBL_SET_TEMP_BED), onDrawPIntMenu, SetBedLevT, &HMI_data.BedLevT);
      #endif
      #ifdef BLTOUCH_HS_MODE
        MENU_ITEM(ICON_HSMode, F("Enable HS mode"), onDrawHSMode, SetHSMode);
      #endif
      MENU_ITEM(ICON_ProbeTest, GET_TEXT_F(MSG_M48_TEST), onDrawMenuItem, ProbeTest);
      MENU_ITEM(ICON_ProbeStow, GET_TEXT_F(MSG_MANUAL_STOW), onDrawMenuItem, ProbeStow);
      MENU_ITEM(ICON_ProbeDeploy, GET_TEXT_F(MSG_MANUAL_DEPLOY), onDrawMenuItem, ProbeDeploy);
    }
    CurrentMenu->draw();
  }
#endif

#if HAS_FILAMENT_SENSOR
  void Draw_FilSet_Menu() {
    checkkey = Menu;
    if (!FilSetMenu) FilSetMenu = new MenuClass();
    if (CurrentMenu != FilSetMenu) {
      CurrentMenu = FilSetMenu;
      CurrentMenu->MenuTitle.SetCaption(GET_TEXT_F(MSG_FILAMENT_SET));
      DWINUI::MenuItemsPrepare(10);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawMenuItem, Draw_AdvancedSettings_Menu);
      #if HAS_FILAMENT_SENSOR
        MENU_ITEM(ICON_Runout, GET_TEXT_F(MSG_RUNOUT_ENABLE), onDrawRunoutEnable, SetRunoutEnable);
      #endif
      #if HAS_FILAMENT_RUNOUT_DISTANCE
        EDIT_ITEM(ICON_Runout, F("Runout Distance"), onDrawPFloatMenu, SetRunoutDistance, &runout.runout_distance());
      #endif
      #if ENABLED(PREVENT_COLD_EXTRUSION)
        EDIT_ITEM(ICON_ExtrudeMinT, F("Extrude Min Temp."), onDrawPIntMenu, SetExtMinT, &HMI_data.ExtMinT);
      #endif
      #if ENABLED(ADVANCED_PAUSE_FEATURE)
        EDIT_ITEM(ICON_FilLoad, GET_TEXT_F(MSG_FILAMENT_LOAD), onDrawPFloatMenu, SetFilLoad, &fc_settings[0].load_length);
        EDIT_ITEM(ICON_FilUnload, GET_TEXT_F(MSG_FILAMENT_UNLOAD), onDrawPFloatMenu, SetFilUnload, &fc_settings[0].unload_length);
      #endif
      #if ENABLED(FWRETRACT)
        EDIT_ITEM(ICON_FWRetLength, GET_TEXT_F(MSG_CONTROL_RETRACT), onDrawPFloatMenu, SetRetractLength, &fwretract.settings.retract_length);
        EDIT_ITEM(ICON_FWRetSpeed, GET_TEXT_F(MSG_SINGLENOZZLE_RETRACT_SPEED), onDrawPFloatMenu, SetRetractSpeed, &fwretract.settings.retract_feedrate_mm_s);
        EDIT_ITEM(ICON_FWRetZRaise, GET_TEXT_F(MSG_CONTROL_RETRACT_ZHOP), onDrawPFloat2Menu, SetZRaise, &fwretract.settings.retract_zraise);
        EDIT_ITEM(ICON_FWRecSpeed, GET_TEXT_F(MSG_SINGLENOZZLE_UNRETRACT_SPEED), onDrawPFloatMenu, SetRecoverSpeed, &fwretract.settings.retract_recover_feedrate_mm_s);
      #endif
    }
    CurrentMenu->draw();
  }
#endif // HAS_FILAMENT_SENSOR

void Draw_SelectColors_Menu() {
  checkkey = Menu;
  if (!SelectColorMenu) SelectColorMenu = new MenuClass();
  if (CurrentMenu != SelectColorMenu) {
    CurrentMenu = SelectColorMenu;
    SetMenuTitle({0}, GET_TEXT_F(MSG_COLORS_SELECT)); // TODO: Chinese, English "Select Color" JPG
    DWINUI::MenuItemsPrepare(20);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_AdvancedSettings_Menu);
    MENU_ITEM(ICON_StockConfiguration, GET_TEXT_F(MSG_RESTORE_DEFAULTS), onDrawMenuItem, RestoreDefaultsColors);
    EDIT_ITEM(0, "Screen Background", onDrawSelColorItem, SelColor, &HMI_data.Background_Color);
    EDIT_ITEM(0, "Cursor", onDrawSelColorItem, SelColor, &HMI_data.Cursor_color);
    EDIT_ITEM(0, "Title Background", onDrawSelColorItem, SelColor, &HMI_data.TitleBg_color);
    EDIT_ITEM(0, "Title Text", onDrawSelColorItem, SelColor, &HMI_data.TitleTxt_color);
    EDIT_ITEM(0, "Text", onDrawSelColorItem, SelColor, &HMI_data.Text_Color);
    EDIT_ITEM(0, "Selected", onDrawSelColorItem, SelColor, &HMI_data.Selected_Color);
    EDIT_ITEM(0, "Split Line", onDrawSelColorItem, SelColor, &HMI_data.SplitLine_Color);
    EDIT_ITEM(0, "Highlight", onDrawSelColorItem, SelColor, &HMI_data.Highlight_Color);
    EDIT_ITEM(0, "Status Background", onDrawSelColorItem, SelColor, &HMI_data.StatusBg_Color);
    EDIT_ITEM(0, "Status Text", onDrawSelColorItem, SelColor, &HMI_data.StatusTxt_Color);
    EDIT_ITEM(0, "Popup Background", onDrawSelColorItem, SelColor, &HMI_data.PopupBg_color);
    EDIT_ITEM(0, "Popup Text", onDrawSelColorItem, SelColor, &HMI_data.PopupTxt_Color);
    EDIT_ITEM(0, "Alert Background", onDrawSelColorItem, SelColor, &HMI_data.AlertBg_Color);
    EDIT_ITEM(0, "Alert Text", onDrawSelColorItem, SelColor, &HMI_data.AlertTxt_Color);
    EDIT_ITEM(0, "Percent Text", onDrawSelColorItem, SelColor, &HMI_data.PercentTxt_Color);
    EDIT_ITEM(0, "Bar Fill", onDrawSelColorItem, SelColor, &HMI_data.Barfill_Color);
    EDIT_ITEM(0, "Indicator value", onDrawSelColorItem, SelColor, &HMI_data.Indicator_Color);
    EDIT_ITEM(0, "Coordinate value", onDrawSelColorItem, SelColor, &HMI_data.Coordinate_Color);
  }
  CurrentMenu->draw();
}

void Draw_GetColor_Menu() {
  checkkey = Menu;
  if (!GetColorMenu) GetColorMenu = new MenuClass();
  if (CurrentMenu != GetColorMenu) {
    CurrentMenu = GetColorMenu;
    SetMenuTitle({0}, GET_TEXT_F(MSG_COLORS_GET)); // TODO: Chinese, English "Get Color" JPG
    DWINUI::MenuItemsPrepare(5);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, DWIN_ApplyColor);
    MENU_ITEM(ICON_Cancel, GET_TEXT_F(MSG_BUTTON_CANCEL), onDrawMenuItem, Draw_SelectColors_Menu);
    MENU_ITEM(0, GET_TEXT_F(MSG_COLORS_RED), onDrawGetColorItem, SetRGBColor);
    MENU_ITEM(1, GET_TEXT_F(MSG_COLORS_GREEN), onDrawGetColorItem, SetRGBColor);
    MENU_ITEM(2, GET_TEXT_F(MSG_COLORS_BLUE), onDrawGetColorItem, SetRGBColor);
  }
  CurrentMenu->draw();
  DWIN_Draw_Rectangle(1, *MenuData.P_Int, 20, 315, DWIN_WIDTH - 20, 335);
}

#if BOTH(CASE_LIGHT_MENU, CASELIGHT_USES_BRIGHTNESS)
    void Draw_CaseLight_Menu() {
      checkkey = Menu;
      if (!CaseLightMenu) CaseLightMenu = new MenuClass();
      if (CurrentMenu != CaseLightMenu) {
        CurrentMenu = CaseLightMenu;
        SetMenuTitle({0}, GET_TEXT_F(MSG_CASE_LIGHT)); // TODO: Chinese, English "Case Light" JPG
        DWINUI::MenuItemsPrepare(3);
        MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Control_Menu);
        MENU_ITEM(ICON_CaseLight, GET_TEXT_F(MSG_CASE_LIGHT), onDrawCaseLight, SetCaseLight);
        EDIT_ITEM(ICON_Brightness, GET_TEXT_F(MSG_CASE_LIGHT_BRIGHTNESS), onDrawPInt8Menu, SetCaseLightBrightness, &caselight.brightness);
      }
      CurrentMenu->draw();
    }
#endif

#if ENABLED(LED_CONTROL_MENU)
    void Draw_LedControl_Menu() {
      checkkey = Menu;
      if (!LedControlMenu) LedControlMenu = new MenuClass();
      if (CurrentMenu != LedControlMenu) {
        CurrentMenu = LedControlMenu;
        SetMenuTitle({0}, GET_TEXT_F(MSG_LED_CONTROL)); // TODO: Chinese, English "LED Control" JPG
        DWINUI::MenuItemsPrepare(6);
        MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Control_Menu);
        #if !BOTH(CASE_LIGHT_MENU, CASE_LIGHT_USE_NEOPIXEL)
          MENU_ITEM(ICON_LedControl, GET_TEXT_F(MSG_LEDS), onDrawLedStatus, SetLedStatus);
        #endif
        #if (HAS_COLOR_LEDS)
          EDIT_ITEM(ICON_LedControl, GET_TEXT_F(MSG_COLORS_RED), onDrawPInt8Menu, SetLedColorR, &leds.color.r);
          EDIT_ITEM(ICON_LedControl, GET_TEXT_F(MSG_COLORS_GREEN), onDrawPInt8Menu, SetLedColorG, &leds.color.g);
          EDIT_ITEM(ICON_LedControl, GET_TEXT_F(MSG_COLORS_BLUE), onDrawPInt8Menu, SetLedColorB, &leds.color.b);
          #if ENABLED(HAS_WHITE_LED)
            EDIT_ITEM(ICON_LedControl, GET_TEXT_F(MSG_COLORS_WHITE), onDrawPInt8Menu, SetLedColorW, &leds.color.w);
          #endif
        #endif
      }
      CurrentMenu->draw();
    }
#endif

void Draw_Tune_Menu() {
  checkkey = Menu;
  if (!TuneMenu) TuneMenu = new MenuClass();
  if (CurrentMenu != TuneMenu) {
    CurrentMenu = TuneMenu;
    SetMenuTitle({73, 2, 28, 12}, GET_TEXT_F(MSG_TUNE)); // TODO: Chinese, English "Tune" JPG
    DWINUI::MenuItemsPrepare(16);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Goto_PrintProcess);
    #if ENABLED(CASE_LIGHT_MENU)
      MENU_ITEM(ICON_CaseLight, GET_TEXT_F(MSG_CASE_LIGHT), onDrawCaseLight, SetCaseLight);
    #elif ENABLED(LED_CONTROL_MENU) && DISABLED(CASE_LIGHT_USE_NEOPIXEL)
      MENU_ITEM(ICON_LedControl, GET_TEXT_F(MSG_LEDS), onDrawLedStatus, SetLedStatus);
    #endif
    EDIT_ITEM(ICON_Speed, GET_TEXT_F(MSG_SPEED), onDrawSpeedItem, SetSpeed, &feedrate_percentage);
    #if HAS_HOTEND
      HotendTargetItem = EDIT_ITEM(ICON_HotendTemp, GET_TEXT_F(MSG_UBL_SET_TEMP_HOTEND), onDrawHotendTemp, SetHotendTemp, &thermalManager.temp_hotend[0].target);
    #endif
    #if HAS_HEATED_BED
      BedTargetItem = EDIT_ITEM(ICON_BedTemp, GET_TEXT_F(MSG_UBL_SET_TEMP_BED), onDrawBedTemp, SetBedTemp, &thermalManager.temp_bed.target);
    #endif
    #if HAS_FAN
      FanSpeedItem = EDIT_ITEM(ICON_FanSpeed, GET_TEXT_F(MSG_FAN_SPEED), onDrawFanSpeed, SetFanSpeed, &thermalManager.fan_speed[0]);
    #endif
    #if HAS_ZOFFSET_ITEM && EITHER(HAS_BED_PROBE, BABYSTEPPING)
      EDIT_ITEM(ICON_Zoffset, GET_TEXT_F(MSG_ZPROBE_ZOFFSET), onDrawZOffset, SetZOffset, &BABY_Z_VAR);
    #endif
    #if ENABLED(FWRETRACT)
      EDIT_ITEM(ICON_FWRetLength, GET_TEXT_F(MSG_CONTROL_RETRACT), onDrawPFloatMenu, SetRetractLength, &fwretract.settings.retract_length);
      EDIT_ITEM(ICON_FWRetSpeed, GET_TEXT_F(MSG_SINGLENOZZLE_RETRACT_SPEED), onDrawPFloatMenu, SetRetractSpeed, &fwretract.settings.retract_feedrate_mm_s);
      EDIT_ITEM(ICON_FWRetZRaise, GET_TEXT_F(MSG_CONTROL_RETRACT_ZHOP), onDrawPFloat2Menu, SetZRaise, &fwretract.settings.retract_zraise);
      EDIT_ITEM(ICON_FWRecSpeed, GET_TEXT_F(MSG_SINGLENOZZLE_UNRETRACT_SPEED), onDrawPFloatMenu, SetRecoverSpeed, &fwretract.settings.retract_recover_feedrate_mm_s);
    #endif
    EDIT_ITEM(ICON_Flow, GET_TEXT_F(MSG_FLOW), onDrawPIntMenu, SetFlow, &planner.flow_percentage[0]);
    #if ENABLED(ADVANCED_PAUSE_FEATURE)
      MENU_ITEM(ICON_FilMan, GET_TEXT_F(MSG_FILAMENTCHANGE), onDrawMenuItem, ChangeFilament);
    #endif
    MENU_ITEM(ICON_Lock, GET_TEXT_F(MSG_LOCKSCREEN), onDrawMenuItem, DWIN_LockScreen);
    #if HAS_LCD_BRIGHTNESS
      EDIT_ITEM(ICON_Brightness, GET_TEXT_F(MSG_BRIGHTNESS), onDrawPInt8Menu, SetBrightness, &ui.brightness);
      MENU_ITEM(ICON_Brightness, GET_TEXT_F(MSG_BRIGHTNESS_OFF), onDrawMenuItem, TurnOffBacklight);
    #endif
  }
  CurrentMenu->draw();
}

void Draw_Motion_Menu() {
  checkkey = Menu;
  if (!MotionMenu) MotionMenu = new MenuClass();
  if (CurrentMenu != MotionMenu) {
    CurrentMenu = MotionMenu;
    SetMenuTitle({1, 16, 28, 13}, GET_TEXT_F(MSG_MOTION)); // TODO: Chinese, English "Motion" JPG
    DWINUI::MenuItemsPrepare(6);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Control_Menu);
    MENU_ITEM(ICON_MaxSpeed, GET_TEXT_F(MSG_SPEED), onDrawSpeed, Draw_MaxSpeed_Menu);
    MENU_ITEM(ICON_MaxAccelerated, GET_TEXT_F(MSG_ACCELERATION), onDrawAcc, Draw_MaxAccel_Menu);
    #if HAS_CLASSIC_JERK
      MENU_ITEM(ICON_MaxJerk, GET_TEXT_F(MSG_JERK), onDrawJerk, Draw_MaxJerk_Menu);
    #endif
    MENU_ITEM(ICON_Step, GET_TEXT_F(MSG_STEPS_PER_MM), onDrawSteps, Draw_Steps_Menu);
    EDIT_ITEM(ICON_Flow, GET_TEXT_F(MSG_FLOW), onDrawPIntMenu, SetFlow, &planner.flow_percentage[0]);
  }
  CurrentMenu->draw();
}

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  void Draw_FilamentMan_Menu() {
    checkkey = Menu;
    if (!FilamentMenu) FilamentMenu = new MenuClass();
    if (CurrentMenu != FilamentMenu) {
      CurrentMenu = FilamentMenu;
      SetMenuTitle({0}, GET_TEXT_F(MSG_FILAMENT_MAN)); // TODO: Chinese, English "Filament Management" JPG
      DWINUI::MenuItemsPrepare(5);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Prepare_Menu);
      MENU_ITEM(ICON_Park, GET_TEXT_F(MSG_FILAMENT_PARK_ENABLED), onDrawMenuItem, ParkHead);
      MENU_ITEM(ICON_FilMan, GET_TEXT_F(MSG_FILAMENTCHANGE), onDrawMenuItem, ChangeFilament);
      #if ENABLED(FILAMENT_LOAD_UNLOAD_GCODES)
        MENU_ITEM(ICON_FilUnload, GET_TEXT_F(MSG_FILAMENTUNLOAD), onDrawMenuItem, UnloadFilament);
        MENU_ITEM(ICON_FilLoad, GET_TEXT_F(MSG_FILAMENTLOAD), onDrawMenuItem, LoadFilament);
      #endif
    }
    CurrentMenu->draw();
  }
#endif

#if ENABLED(MESH_BED_LEVELING)
  void Draw_ManualMesh_Menu() {
    checkkey = Menu;
    if (!ManualMesh) ManualMesh = new MenuClass();
    if (CurrentMenu != ManualMesh) {
      CurrentMenu = ManualMesh;
      SetMenuTitle({0}, GET_TEXT_F(MSG_MANUAL_MESH)); // TODO: Chinese, English "Manual Mesh Leveling" JPG
      DWINUI::MenuItemsPrepare(6);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Prepare_Menu);
      MENU_ITEM(ICON_ManualMesh, GET_TEXT_F(MSG_LEVEL_BED), onDrawMenuItem, ManualMeshStart);
      MMeshMoveZItem = EDIT_ITEM(ICON_Zoffset, GET_TEXT_F(MSG_MOVE_Z), onDrawMMeshMoveZ, SetMMeshMoveZ, &current_position.z);
      MENU_ITEM(ICON_Axis, GET_TEXT_F(MSG_UBL_CONTINUE_MESH), onDrawMenuItem, ManualMeshContinue);
      MENU_ITEM(ICON_MeshViewer, GET_TEXT_F(MSG_MESH_VIEW), onDrawSubMenu, DWIN_MeshViewer);
      MENU_ITEM(ICON_MeshSave, GET_TEXT_F(MSG_UBL_SAVE_MESH), onDrawMenuItem, ManualMeshSave);
    }
    CurrentMenu->draw();
  }
#endif

#if HAS_PREHEAT

  void Draw_Preheat_Menu(frame_rect_t cn, const __FlashStringHelper* fstr) {
    checkkey = Menu;
    if (CurrentMenu != PreheatMenu) {
      CurrentMenu = PreheatMenu;
      SetMenuTitle(cn, fstr);
      DWINUI::MenuItemsPrepare(5);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Temperature_Menu);
      #if HAS_HOTEND
        EDIT_ITEM(ICON_SetEndTemp, GET_TEXT_F(MSG_UBL_SET_TEMP_HOTEND), onDrawSetPreheatHotend, SetPreheatEndTemp, &ui.material_preset[HMI_value.Preheat].hotend_temp);
      #endif
      #if HAS_HEATED_BED
        EDIT_ITEM(ICON_SetBedTemp, GET_TEXT_F(MSG_UBL_SET_TEMP_BED), onDrawSetPreheatBed, SetPreheatBedTemp, &ui.material_preset[HMI_value.Preheat].bed_temp);
      #endif
      #if HAS_FAN
        EDIT_ITEM(ICON_FanSpeed, GET_TEXT_F(MSG_FAN_SPEED), onDrawSetPreheatFan, SetPreheatFanSpeed, &ui.material_preset[HMI_value.Preheat].fan_speed);
      #endif
      #if ENABLED(EEPROM_SETTINGS)
        MENU_ITEM(ICON_WriteEEPROM, GET_TEXT_F(MSG_STORE_EEPROM), onDrawWriteEeprom, WriteEeprom);
      #endif
    }
    CurrentMenu->draw();
  }

  void Draw_Preheat1_Menu() {
    HMI_value.Preheat = 0;
    if (!PreheatMenu) PreheatMenu = new MenuClass();
    Draw_Preheat_Menu({59, 16, 81, 14}, F(PREHEAT_1_LABEL " Preheat Settings")); // TODO: English "PLA Settings" JPG
  }

  void Draw_Preheat2_Menu() {
    HMI_value.Preheat = 1;
    if (!PreheatMenu) PreheatMenu = new MenuClass();
    Draw_Preheat_Menu({142, 16, 82, 14}, F(PREHEAT_2_LABEL " Preheat Settings"));  // TODO: English "ABS Settings" JPG
  }

  #ifdef PREHEAT_3_LABEL
    void Draw_Preheat3_Menu() {
      HMI_value.Preheat = 2;
      if (!PreheatMenu) PreheatMenu = new MenuClass();
      #define PREHEAT_3_TITLE PREHEAT_3_LABEL " Preheat Set."
      Draw_Preheat_Menu({0}, F(PREHEAT_3_TITLE));  // TODO: Chinese, English "Custom Preheat Settings" JPG
    }
  #endif

#endif // HAS_PREHEAT

void Draw_Temperature_Menu() {
  checkkey = Menu;
  if (!TemperatureMenu) TemperatureMenu = new MenuClass();
  if (CurrentMenu != TemperatureMenu) {
    CurrentMenu = TemperatureMenu;
    SetMenuTitle({236, 2, 28, 12}, GET_TEXT_F(MSG_TEMPERATURE));
    DWINUI::MenuItemsPrepare(7);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Control_Menu);
    #if HAS_HOTEND
      HotendTargetItem = EDIT_ITEM(ICON_SetEndTemp, GET_TEXT_F(MSG_UBL_SET_TEMP_HOTEND), onDrawHotendTemp, SetHotendTemp, &thermalManager.temp_hotend[0].target);
    #endif
    #if HAS_HEATED_BED
      BedTargetItem = EDIT_ITEM(ICON_SetBedTemp, GET_TEXT_F(MSG_UBL_SET_TEMP_BED), onDrawBedTemp, SetBedTemp, &thermalManager.temp_bed.target);
    #endif
    #if HAS_FAN
      FanSpeedItem = EDIT_ITEM(ICON_FanSpeed, GET_TEXT_F(MSG_FAN_SPEED), onDrawFanSpeed, SetFanSpeed, &thermalManager.fan_speed[0]);
    #endif
    #if HAS_HOTEND
      MENU_ITEM(ICON_SetPLAPreheat, F(PREHEAT_1_LABEL " Preheat Settings"), onDrawPLAPreheatSubMenu, Draw_Preheat1_Menu);
      MENU_ITEM(ICON_SetABSPreheat, F(PREHEAT_2_LABEL " Preheat Settings"), onDrawABSPreheatSubMenu, Draw_Preheat2_Menu);
      #ifdef PREHEAT_3_LABEL
        MENU_ITEM(ICON_SetCustomPreheat, PREHEAT_3_TITLE, onDrawSubMenu, Draw_Preheat3_Menu);
      #endif
    #endif
  }
  CurrentMenu->draw();
}

void Draw_MaxSpeed_Menu() {
  checkkey = Menu;
  if (!MaxSpeedMenu) MaxSpeedMenu = new MenuClass();
  if (CurrentMenu != MaxSpeedMenu) {
    CurrentMenu = MaxSpeedMenu;
    SetMenuTitle({1, 16, 28, 13}, GET_TEXT_F(MSG_MAXSPEED));
    DWINUI::MenuItemsPrepare(5);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Motion_Menu);
    EDIT_ITEM(ICON_MaxSpeedX, GET_TEXT_F(MSG_MAXSPEED_X), onDrawMaxSpeedX, SetMaxSpeedX, &planner.settings.max_feedrate_mm_s[X_AXIS]);
    EDIT_ITEM(ICON_MaxSpeedY, GET_TEXT_F(MSG_MAXSPEED_Y), onDrawMaxSpeedY, SetMaxSpeedY, &planner.settings.max_feedrate_mm_s[Y_AXIS]);
    EDIT_ITEM(ICON_MaxSpeedZ, GET_TEXT_F(MSG_MAXSPEED_Z), onDrawMaxSpeedZ, SetMaxSpeedZ, &planner.settings.max_feedrate_mm_s[Z_AXIS]);
    #if HAS_HOTEND
      EDIT_ITEM(ICON_MaxSpeedE, GET_TEXT_F(MSG_MAXSPEED_E), onDrawMaxSpeedE, SetMaxSpeedE, &planner.settings.max_feedrate_mm_s[E_AXIS]);
    #endif
  }
  CurrentMenu->draw();
}

void Draw_MaxAccel_Menu() {
  checkkey = Menu;
  if (!MaxAccelMenu) MaxAccelMenu = new MenuClass();
  if (CurrentMenu != MaxAccelMenu) {
    CurrentMenu = MaxAccelMenu;
    SetMenuTitle({1, 16, 28, 13}, GET_TEXT_F(MSG_ACCELERATION));
    DWINUI::MenuItemsPrepare(5);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Motion_Menu);
    EDIT_ITEM(ICON_MaxAccX, GET_TEXT_F(MSG_AMAX_A), onDrawMaxAccelX, SetMaxAccelX, &planner.settings.max_acceleration_mm_per_s2[X_AXIS]);
    EDIT_ITEM(ICON_MaxAccY, GET_TEXT_F(MSG_AMAX_B), onDrawMaxAccelY, SetMaxAccelY, &planner.settings.max_acceleration_mm_per_s2[Y_AXIS]);
    EDIT_ITEM(ICON_MaxAccZ, GET_TEXT_F(MSG_AMAX_C), onDrawMaxAccelZ, SetMaxAccelZ, &planner.settings.max_acceleration_mm_per_s2[Z_AXIS]);
    #if HAS_HOTEND
      EDIT_ITEM(ICON_MaxAccE, GET_TEXT_F(MSG_AMAX_E), onDrawMaxAccelE, SetMaxAccelE, &planner.settings.max_acceleration_mm_per_s2[E_AXIS]);
    #endif
  }
  CurrentMenu->draw();
}

#if HAS_CLASSIC_JERK
  void Draw_MaxJerk_Menu() {
    checkkey = Menu;
    if (!MaxJerkMenu) MaxJerkMenu = new MenuClass();
    if (CurrentMenu != MaxJerkMenu) {
      CurrentMenu = MaxJerkMenu;
      SetMenuTitle({1, 16, 28, 13}, GET_TEXT_F(MSG_JERK));
      DWINUI::MenuItemsPrepare(5);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Motion_Menu);
      EDIT_ITEM(ICON_MaxSpeedJerkX, GET_TEXT_F(MSG_VA_JERK), onDrawMaxJerkX, SetMaxJerkX, &planner.max_jerk[X_AXIS]);
      EDIT_ITEM(ICON_MaxSpeedJerkY, GET_TEXT_F(MSG_VB_JERK), onDrawMaxJerkY, SetMaxJerkY, &planner.max_jerk[Y_AXIS]);
      EDIT_ITEM(ICON_MaxSpeedJerkZ, GET_TEXT_F(MSG_VC_JERK), onDrawMaxJerkZ, SetMaxJerkZ, &planner.max_jerk[Z_AXIS]);
      #if HAS_HOTEND
        EDIT_ITEM(ICON_MaxSpeedJerkE, GET_TEXT_F(MSG_VE_JERK), onDrawMaxJerkE, SetMaxJerkE, &planner.max_jerk[E_AXIS]);
      #endif
    }
    CurrentMenu->draw();
  }
#endif

void Draw_Steps_Menu() {
  checkkey = Menu;
  if (!StepsMenu) StepsMenu = new MenuClass();
  if (CurrentMenu != StepsMenu) {
    CurrentMenu = StepsMenu;
    SetMenuTitle({1, 16, 28, 13}, GET_TEXT_F(MSG_STEPS_PER_MM));
    DWINUI::MenuItemsPrepare(5);
    MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawBack, Draw_Motion_Menu);
    EDIT_ITEM(ICON_StepX, GET_TEXT_F(MSG_A_STEPS), onDrawStepsX, SetStepsX, &planner.settings.axis_steps_per_mm[X_AXIS]);
    EDIT_ITEM(ICON_StepY, GET_TEXT_F(MSG_B_STEPS), onDrawStepsY, SetStepsY, &planner.settings.axis_steps_per_mm[Y_AXIS]);
    EDIT_ITEM(ICON_StepZ, GET_TEXT_F(MSG_C_STEPS), onDrawStepsZ, SetStepsZ, &planner.settings.axis_steps_per_mm[Z_AXIS]);
    #if HAS_HOTEND
      EDIT_ITEM(ICON_StepE, GET_TEXT_F(MSG_E_STEPS), onDrawStepsE, SetStepsE, &planner.settings.axis_steps_per_mm[E_AXIS]);
    #endif
  }
  CurrentMenu->draw();
}

#if HAS_HOTEND
  void Draw_HotendPID_Menu() {
    checkkey = Menu;
    if (!HotendPIDMenu) HotendPIDMenu = new MenuClass();
    if (CurrentMenu != HotendPIDMenu) {
      CurrentMenu = HotendPIDMenu;
      CurrentMenu->MenuTitle.SetCaption(F("Hotend PID Settings"));
      DWINUI::MenuItemsPrepare(8);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawMenuItem, Draw_AdvancedSettings_Menu);
      MENU_ITEM(ICON_PIDNozzle, F("Hotend PID"), onDrawMenuItem, HotendPID);
      EDIT_ITEM(ICON_PIDValue, F("Set" STR_KP), onDrawPFloat2Menu, SetKp, &thermalManager.temp_hotend[0].pid.Kp);
      EDIT_ITEM(ICON_PIDValue, F("Set" STR_KI), onDrawPIDi, SetKi, &thermalManager.temp_hotend[0].pid.Ki);
      EDIT_ITEM(ICON_PIDValue, F("Set" STR_KD), onDrawPIDd, SetKd, &thermalManager.temp_hotend[0].pid.Kd);
      EDIT_ITEM(ICON_Temperature, GET_TEXT_F(MSG_TEMPERATURE), onDrawPIntMenu, SetHotendPidT, &HMI_data.HotendPidT);
      EDIT_ITEM(ICON_PIDcycles, GET_TEXT_F(MSG_PID_CYCLE), onDrawPIntMenu, SetPidCycles, &HMI_data.PidCycles);
      #if ENABLED(EEPROM_SETTINGS)
        MENU_ITEM(ICON_WriteEEPROM, GET_TEXT_F(MSG_STORE_EEPROM), onDrawMenuItem, WriteEeprom);
      #endif
    }
    CurrentMenu->draw();
  }
#endif

#if HAS_HEATED_BED
  void Draw_BedPID_Menu() {
    checkkey = Menu;
    if (!BedPIDMenu) BedPIDMenu = new MenuClass();
    if (CurrentMenu != BedPIDMenu) {
      CurrentMenu = BedPIDMenu;
      CurrentMenu->MenuTitle.SetCaption(F("Bed PID Settings"));
      DWINUI::MenuItemsPrepare(8);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawMenuItem, Draw_AdvancedSettings_Menu);
      MENU_ITEM(ICON_PIDNozzle, F("Bed PID"), onDrawMenuItem,BedPID);
      EDIT_ITEM(ICON_PIDValue, F("Set" STR_KP), onDrawPFloat2Menu, SetKp, &thermalManager.temp_bed.pid.Kp);
      EDIT_ITEM(ICON_PIDValue, F("Set" STR_KI), onDrawPIDi, SetKi, &thermalManager.temp_bed.pid.Ki);
      EDIT_ITEM(ICON_PIDValue, F("Set" STR_KD), onDrawPIDd, SetKd, &thermalManager.temp_bed.pid.Kd);
      EDIT_ITEM(ICON_Temperature, GET_TEXT_F(MSG_TEMPERATURE), onDrawPIntMenu, SetBedPidT, &HMI_data.BedPidT);
      EDIT_ITEM(ICON_PIDcycles, GET_TEXT_F(MSG_PID_CYCLE), onDrawPIntMenu, SetPidCycles, &HMI_data.PidCycles);
      #if ENABLED(EEPROM_SETTINGS)
        MENU_ITEM(ICON_WriteEEPROM, GET_TEXT_F(MSG_STORE_EEPROM), onDrawMenuItem, WriteEeprom);
      #endif
    }
    CurrentMenu->draw();
  }
#endif

#if HAS_BED_PROBE
  void Draw_ZOffsetWiz_Menu() {
    checkkey = Menu;
    if (!ZOffsetWizMenu) ZOffsetWizMenu = new MenuClass();
    if (CurrentMenu != ZOffsetWizMenu) {
      CurrentMenu = ZOffsetWizMenu;
      CurrentMenu->MenuTitle.SetCaption(GET_TEXT_F(MSG_PROBE_WIZARD));
      DWINUI::MenuItemsPrepare(4);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawMenuItem, Draw_Prepare_Menu);
      MENU_ITEM(ICON_Homing, GET_TEXT_F(MSG_AUTO_HOME), onDrawMenuItem, AutoHome);
      MENU_ITEM(ICON_MoveZ0, F("Move Z to Home"), onDrawMenuItem, SetMoveZto0);
      EDIT_ITEM(ICON_Zoffset, GET_TEXT_F(MSG_ZPROBE_ZOFFSET), onDrawPFloat2Menu, SetZOffset, &BABY_Z_VAR);
    }
    CurrentMenu->draw();
    if (!axis_is_trusted(Z_AXIS)) LCD_MESSAGE_F("WARNING: Z position unknown, move Z to home");
  }
#endif

#if ENABLED(INDIVIDUAL_AXIS_HOMING_SUBMENU)
  void Draw_Homing_Menu() {
    checkkey = Menu;
    if (!HomingMenu) HomingMenu = new MenuClass();
    if (CurrentMenu != HomingMenu) {
      CurrentMenu = HomingMenu;
      CurrentMenu->MenuTitle.SetCaption(GET_TEXT_F(MSG_HOMING));
      DWINUI::MenuItemsPrepare(5);
      MENU_ITEM(ICON_Back, GET_TEXT_F(MSG_BUTTON_BACK), onDrawMenuItem, Draw_Prepare_Menu);
      MENU_ITEM(ICON_Homing, GET_TEXT_F(MSG_AUTO_HOME), onDrawMenuItem, AutoHome);
      MENU_ITEM(ICON_HomeX, GET_TEXT_F(MSG_AUTO_HOME_X), onDrawMenuItem, HomeX);
      MENU_ITEM(ICON_HomeY, GET_TEXT_F(MSG_AUTO_HOME_Y), onDrawMenuItem, HomeY);
      MENU_ITEM(ICON_HomeZ, GET_TEXT_F(MSG_AUTO_HOME_Z), onDrawMenuItem, HomeZ);
    }
    CurrentMenu->draw();
  }
#endif

#endif // DWIN_LCD_PROUI
