/****************************************************************************
Copyright (c) 2010 cocos2d-x.org
Copyright (c) 2017-2018 Xiamen Yaji Software Co., Ltd.

https://axmol.dev/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/
#include "axmol/base/InputSystem.h"
#include "axmol/base/Director.h"
#include "axmol/base/Scheduler.h"
#include "axmol/base/EventType.h"
#include "axmol/base/CustomEvent.h"
#include "axmol/base/EventDispatcher.h"
#include "axmol/platform/Application.h"
#include "axmol/platform/android/RenderView-android.h"
#include "axmol/base/text_utils.h"
#include "axmol/platform/android/jni/JniHelper.h"
#include "axmol/rhi/GraphicsCore.h"
#include "axmol/renderer/TextureCache.h"
#include "axmol/tlx/static_vector.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>

using namespace ax;

static ANativeWindow* s_nativeWindow;

static constexpr int AX_MAX_TOUCHES = 10;

ANativeWindow* axmolGetANativeWindow()
{
    return s_nativeWindow;
}

static void axmolDispatchContextLost(bool isWarmStart)
{
#if AX_ENABLE_RESTART_APPLICATION_ON_CONTEXT_LOST
    auto director = ax::Director::getInstance();
    ax::CustomEvent recreatedEvent(EVENT_APP_RESTARTING);
    director->getEventDispatcher()->dispatchEvent(&recreatedEvent, true);

    //  Pop to root scene, replace with an empty scene, and clear all cached data before restarting
    director->popToRootScene();
    auto rootScene = Scene::create();
    director->replaceScene(rootScene);
    director->purgeCachedData();

    JniHelper::callStaticVoidMethod("dev/axmol/lib/AxmolEngine", "restartProcess");
#endif

    if (isWarmStart)
    {
        auto director = ax::Director::getInstance();
        ax::CustomEvent warmStartEvent(EVENT_APP_WARM_START);
        director->getEventDispatcher()->dispatchEvent(&warmStartEvent, true);
    }
}

#define KEYCODE_BACK        0x04
#define KEYCODE_MENU        0x52
#define KEYCODE_DPAD_UP     0x13
#define KEYCODE_DPAD_DOWN   0x14
#define KEYCODE_DPAD_LEFT   0x15
#define KEYCODE_DPAD_RIGHT  0x16
#define KEYCODE_ENTER       0x42
#define KEYCODE_PLAY        0x7e
#define KEYCODE_DPAD_CENTER 0x17

static std::unordered_map<int, ax::KeyboardEvent::KeyCode> g_keyCodeMap = {
    {KEYCODE_BACK, ax::KeyboardEvent::KeyCode::KEY_ESCAPE},
    {KEYCODE_MENU, ax::KeyboardEvent::KeyCode::KEY_MENU},
    {KEYCODE_DPAD_UP, ax::KeyboardEvent::KeyCode::KEY_DPAD_UP},
    {KEYCODE_DPAD_DOWN, ax::KeyboardEvent::KeyCode::KEY_DPAD_DOWN},
    {KEYCODE_DPAD_LEFT, ax::KeyboardEvent::KeyCode::KEY_DPAD_LEFT},
    {KEYCODE_DPAD_RIGHT, ax::KeyboardEvent::KeyCode::KEY_DPAD_RIGHT},
    {KEYCODE_ENTER, ax::KeyboardEvent::KeyCode::KEY_ENTER},
    {KEYCODE_PLAY, ax::KeyboardEvent::KeyCode::KEY_PLAY},
    {KEYCODE_DPAD_CENTER, ax::KeyboardEvent::KeyCode::KEY_DPAD_CENTER},

};

struct TouchPoint
{
    intptr_t id;
    float x;
    float y;
    float pressure;
};

extern "C" {

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeOnSurfaceCreated(JNIEnv* env,
                                                                             jclass,
                                                                             jobject surface,
                                                                             jint w,
                                                                             jint h,
                                                                             jboolean isWarmStart)
{
    if (s_nativeWindow)
        ANativeWindow_release(s_nativeWindow);

    s_nativeWindow = ANativeWindow_fromSurface(env, surface);
    if (s_nativeWindow == nullptr)
    {
        AXLOGW(
            "ANativeWindow_fromSurface failed: surface={}, "
            "windowSize=({} x {}), threadId={}",
            fmt::ptr(surface), static_cast<int>(w), static_cast<int>(h), (long)gettid());
        return;
    }

    AXLOGI(
        "ANativeWindow_fromSurface success: window={}, "
        "size=({} x {})",
        fmt::ptr(s_nativeWindow), static_cast<int>(w), static_cast<int>(h));

    auto director   = ax::Director::getInstance();
    auto renderView = director->getRenderView();
    if (!renderView)
    {
        renderView = ax::RenderView::createWithRect("axmol3",
                                                    Rect{ax::Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}});
        director->setRenderView(renderView);

        auto axmolApp = ax::ApplicationCore::getInstance();
        axmolApp->run();
    }
    else
    {
        if (rhi::GraphicsCore::isVulkan())
        {
            static_cast<ax::RenderView*>(renderView)->recreateVkSurface(true);
        }
        else
        {
            axdrv->resetState();
            ax::CustomEvent recreatedEvent(EVENT_RENDERER_RECREATED);
            director->getEventDispatcher()->dispatchEvent(&recreatedEvent, true);
            director->setRenderDefaults();
#if AX_ENABLE_CONTEXT_LOSS_RECOVERY
            ax::VolatileTextureMgr::reloadAllTextures();
            axmolDispatchContextLost(isWarmStart);
#endif
        }
    }
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeOnSurfaceChanged(JNIEnv*, jclass, jint w, jint h)
{
    auto director   = ax::Director::getInstance();
    auto renderView = director->getRenderView();
    if (renderView)
    {
        renderView->updateRenderSurface(w, h, ax::RenderView::AllUpdates);
    }
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeStepFrame(JNIEnv*, jclass)
{
    ax::Director::getInstance()->renderFrame();
}

JNIEXPORT void JNICALL
Java_dev_axmol_lib_AxmolPlayer_nativeTouchBegin(JNIEnv* env, jclass, jint id, jfloat x, jfloat y, jfloat pressure)
{
    auto director = ax::Director::getInstance();

    director->postTask(
        [pos = Vec2{x, y}, state = ax::PointerInputState{.id       = static_cast<intptr_t>(id),
                                                         .pressure = static_cast<float>(pressure),
                                                         .type     = ax::PointerType::Touch}]() {
        ax::InputSystem::getInstance()->handlePointerDown(pos, state);
    },
        Director::TaskTiming::FrameBoundary);
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeOnWindowFocusChanged(JNIEnv* env,
                                                                                 jobject thiz,
                                                                                 jboolean has_focus)
{
    if (!has_focus)
    {
        ax::InputSystem::getInstance()->resetInput();
    }
}

JNIEXPORT void JNICALL
Java_dev_axmol_lib_AxmolPlayer_nativeTouchEnd(JNIEnv* env, jclass, jint id, jfloat x, jfloat y, jfloat pressure)
{
    auto director = ax::Director::getInstance();

    director->postTask(
        [pos = Vec2{x, y}, state = ax::PointerInputState{.id       = static_cast<intptr_t>(id),
                                                         .pressure = static_cast<float>(pressure),
                                                         .type     = ax::PointerType::Touch}]() {
        ax::InputSystem::getInstance()->handlePointerUp(pos, state);
    },
        Director::TaskTiming::FrameBoundary);
}

// ==============================================================================
// 2. Multi-Pointer Events (Zero-Copy)
// ==============================================================================
JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeTouchesMove(JNIEnv* env,
                                                                        jclass,
                                                                        jintArray jIds,
                                                                        jfloatArray jXs,
                                                                        jfloatArray jYs,
                                                                        jfloatArray jPressures,
                                                                        jint size)
{
    if (size <= 0)
        return;

    jint* ids         = (jint*)env->GetPrimitiveArrayCritical(jIds, nullptr);
    jfloat* xs        = (jfloat*)env->GetPrimitiveArrayCritical(jXs, nullptr);
    jfloat* ys        = (jfloat*)env->GetPrimitiveArrayCritical(jYs, nullptr);
    jfloat* pressures = (jfloat*)env->GetPrimitiveArrayCritical(jPressures, nullptr);

    size = std::min(size, AX_MAX_TOUCHES);

    tlx::static_vector<TouchPoint, AX_MAX_TOUCHES> touchPoints;
    if (ids && xs && ys && pressures)
    {
        for (int i = 0; i < size; ++i)
            touchPoints.push_back(
                {.id = static_cast<intptr_t>(ids[i]), .x = xs[i], .y = ys[i], .pressure = pressures[i]});
    }

    if (pressures)
        env->ReleasePrimitiveArrayCritical(jPressures, pressures, JNI_ABORT);
    if (ys)
        env->ReleasePrimitiveArrayCritical(jYs, ys, JNI_ABORT);
    if (xs)
        env->ReleasePrimitiveArrayCritical(jXs, xs, JNI_ABORT);
    if (ids)
        env->ReleasePrimitiveArrayCritical(jIds, ids, JNI_ABORT);

    ax::Director::getInstance()->postTask([touchPoints = std::move(touchPoints)]() {
        auto inputSys = ax::InputSystem::getInstance();
        for (auto& touchPoint : touchPoints)
        {
            auto state = ax::PointerInputState{
                .id = touchPoint.id, .pressure = touchPoint.pressure, .type = ax::PointerType::Touch};
            inputSys->handlePointerMove(Vec2(touchPoint.x, touchPoint.y), state);
        }
    }, Director::TaskTiming::FrameBoundary);
}



// @see https://android.googlesource.com/platform/frameworks/base/+/master/core/java/android/view/KeyEvent.java
#define KEYCODE_UNKNOWN 0
#define KEYCODE_SOFT_LEFT 1
#define KEYCODE_SOFT_RIGHT 2
#define KEYCODE_HOME 3
#define KEYCODE_BACK 4
#define KEYCODE_CALL 5
#define KEYCODE_ENDCALL 6
#define KEYCODE_0 7
#define KEYCODE_1 8
#define KEYCODE_2 9
#define KEYCODE_3 10
#define KEYCODE_4 11
#define KEYCODE_5 12
#define KEYCODE_6 13
#define KEYCODE_7 14
#define KEYCODE_8 15
#define KEYCODE_9 16
#define KEYCODE_STAR 17
#define KEYCODE_POUND 18
#define KEYCODE_DPAD_UP 19
#define KEYCODE_DPAD_DOWN 20
#define KEYCODE_DPAD_LEFT 21
#define KEYCODE_DPAD_RIGHT 22
#define KEYCODE_DPAD_CENTER 23
#define KEYCODE_VOLUME_UP 24
#define KEYCODE_VOLUME_DOWN 25
#define KEYCODE_POWER 26
#define KEYCODE_CAMERA 27
#define KEYCODE_CLEAR 28
#define KEYCODE_A 29
#define KEYCODE_B 30
#define KEYCODE_C 31
#define KEYCODE_D 32
#define KEYCODE_E 33
#define KEYCODE_F 34
#define KEYCODE_G 35
#define KEYCODE_H 36
#define KEYCODE_I 37
#define KEYCODE_J 38
#define KEYCODE_K 39
#define KEYCODE_L 40
#define KEYCODE_M 41
#define KEYCODE_N 42
#define KEYCODE_O 43
#define KEYCODE_P 44
#define KEYCODE_Q 45
#define KEYCODE_R 46
#define KEYCODE_S 47
#define KEYCODE_T 48
#define KEYCODE_U 49
#define KEYCODE_V 50
#define KEYCODE_W 51
#define KEYCODE_X 52
#define KEYCODE_Y 53
#define KEYCODE_Z 54
#define KEYCODE_COMMA 55
#define KEYCODE_PERIOD 56
#define KEYCODE_ALT_LEFT 57
#define KEYCODE_ALT_RIGHT 58
#define KEYCODE_SHIFT_LEFT 59
#define KEYCODE_SHIFT_RIGHT 60
#define KEYCODE_TAB 61
#define KEYCODE_SPACE 62
#define KEYCODE_SYM 63
#define KEYCODE_EXPLORER 64
#define KEYCODE_ENVELOPE 65
#define KEYCODE_ENTER 66
#define KEYCODE_DEL 67
#define KEYCODE_GRAVE 68
#define KEYCODE_MINUS 69
#define KEYCODE_EQUALS 70
#define KEYCODE_LEFT_BRACKET 71
#define KEYCODE_RIGHT_BRACKET 72
#define KEYCODE_BACKSLASH 73
#define KEYCODE_SEMICOLON 74
#define KEYCODE_APOSTROPHE 75
#define KEYCODE_SLASH 76
#define KEYCODE_AT 77
#define KEYCODE_NUM 78
#define KEYCODE_HEADSETHOOK 79
#define KEYCODE_FOCUS 80;   // *Camera* focu
#define KEYCODE_PLUS 81
#define KEYCODE_MENU 82
#define KEYCODE_NOTIFICATION 83
#define KEYCODE_SEARCH 84
#define KEYCODE_MEDIA_PLAY_PAUSE85 define KEYCODE_MEDIA_STOP 86
#define KEYCODE_MEDIA_NEXT 87
#define KEYCODE_MEDIA_PREVIOUS 88
#define KEYCODE_MEDIA_REWIND 89
#define KEYCODE_MEDIA_FAST_FORWARD 90
#define KEYCODE_MUTE 91
#define KEYCODE_PAGE_UP 92
#define KEYCODE_PAGE_DOWN 93
#define KEYCODE_PICTSYMBOLS 94;   // switch symbol-sets (Emoji,Kao-moji
#define KEYCODE_SWITCH_CHARSET 95;   // switch char-sets (Kanji,Katakana
#define KEYCODE_BUTTON_A 96
#define KEYCODE_BUTTON_B 97
#define KEYCODE_BUTTON_C 98
#define KEYCODE_BUTTON_X 99
#define KEYCODE_BUTTON_Y 100
#define KEYCODE_BUTTON_Z 101
#define KEYCODE_BUTTON_L1 102
#define KEYCODE_BUTTON_R1 103
#define KEYCODE_BUTTON_L2 104
#define KEYCODE_BUTTON_R2 105
#define KEYCODE_BUTTON_THUMBL 106
#define KEYCODE_BUTTON_THUMBR 107
#define KEYCODE_BUTTON_START 108
#define KEYCODE_BUTTON_SELECT 109
#define KEYCODE_BUTTON_MODE 110
#define KEYCODE_ESCAPE 111
#define KEYCODE_FORWARD_DEL 112
#define KEYCODE_CTRL_LEFT 113
#define KEYCODE_CTRL_RIGHT 114
#define KEYCODE_CAPS_LOCK 115
#define KEYCODE_SCROLL_LOCK 116
#define KEYCODE_META_LEFT 117
#define KEYCODE_META_RIGHT 118
#define KEYCODE_FUNCTION 119
#define KEYCODE_SYSRQ 120
#define KEYCODE_BREAK 121
#define KEYCODE_MOVE_HOME 122
#define KEYCODE_MOVE_END 123
#define KEYCODE_INSERT 124
#define KEYCODE_FORWARD 125
#define KEYCODE_MEDIA_PLAY 126
#define KEYCODE_MEDIA_PAUSE 127
#define KEYCODE_MEDIA_CLOSE 128
#define KEYCODE_MEDIA_EJECT 129
#define KEYCODE_MEDIA_RECORD 130
#define KEYCODE_F1 131
#define KEYCODE_F2 132
#define KEYCODE_F3 133
#define KEYCODE_F4 134
#define KEYCODE_F5 135
#define KEYCODE_F6 136
#define KEYCODE_F7 137
#define KEYCODE_F8 138
#define KEYCODE_F9 139
#define KEYCODE_F10 140
#define KEYCODE_F11 141
#define KEYCODE_F12 142
#define KEYCODE_NUM_LOCK 143
#define KEYCODE_NUMPAD_0 144
#define KEYCODE_NUMPAD_1 145
#define KEYCODE_NUMPAD_2 146
#define KEYCODE_NUMPAD_3 147
#define KEYCODE_NUMPAD_4 148
#define KEYCODE_NUMPAD_5 149
#define KEYCODE_NUMPAD_6 150
#define KEYCODE_NUMPAD_7 151
#define KEYCODE_NUMPAD_8 152
#define KEYCODE_NUMPAD_9 153
#define KEYCODE_NUMPAD_DIVIDE 154
#define KEYCODE_NUMPAD_MULTIPLY 155
#define KEYCODE_NUMPAD_SUBTRACT 156
#define KEYCODE_NUMPAD_ADD 157
#define KEYCODE_NUMPAD_DOT 158
#define KEYCODE_NUMPAD_COMMA 159
#define KEYCODE_NUMPAD_ENTER 160
#define KEYCODE_NUMPAD_EQUALS 161
#define KEYCODE_NUMPAD_LEFT_PAREN 162
#define KEYCODE_NUMPAD_RIGHT_PAREN 163
#define KEYCODE_VOLUME_MUTE 164
#define KEYCODE_INFO 165
#define KEYCODE_CHANNEL_UP 166
#define KEYCODE_CHANNEL_DOWN 167
#define KEYCODE_ZOOM_IN 168
#define KEYCODE_ZOOM_OUT 169
#define KEYCODE_TV 170
#define KEYCODE_WINDOW 171
#define KEYCODE_GUIDE 172
#define KEYCODE_DVR 173
#define KEYCODE_BOOKMARK 174
#define KEYCODE_CAPTIONS 175
#define KEYCODE_SETTINGS 176
#define KEYCODE_TV_POWER 177
#define KEYCODE_TV_INPUT 178
#define KEYCODE_STB_POWER 179
#define KEYCODE_STB_INPUT 180
#define KEYCODE_AVR_POWER 181
#define KEYCODE_AVR_INPUT 182
#define KEYCODE_PROG_RED 183
#define KEYCODE_PROG_GREEN 184
#define KEYCODE_PROG_YELLOW 185
#define KEYCODE_PROG_BLUE 186
#define KEYCODE_APP_SWITCH 187
#define KEYCODE_BUTTON_1 188
#define KEYCODE_BUTTON_2 189
#define KEYCODE_BUTTON_3 190
#define KEYCODE_BUTTON_4 191
#define KEYCODE_BUTTON_5 192
#define KEYCODE_BUTTON_6 193
#define KEYCODE_BUTTON_7 194
#define KEYCODE_BUTTON_8 195
#define KEYCODE_BUTTON_9 196
#define KEYCODE_BUTTON_10 197
#define KEYCODE_BUTTON_11 198
#define KEYCODE_BUTTON_12 199
#define KEYCODE_BUTTON_13 200
#define KEYCODE_BUTTON_14 201
#define KEYCODE_BUTTON_15 202
#define KEYCODE_BUTTON_16 203
#define KEYCODE_LANGUAGE_SWITCH 204
#define KEYCODE_MANNER_MODE 205
#define KEYCODE_3D_MODE 206
#define KEYCODE_CONTACTS 207
#define KEYCODE_CALENDAR 208
#define KEYCODE_MUSIC 209
#define KEYCODE_CALCULATOR 210
#define KEYCODE_ZENKAKU_HANKAKU 211
#define KEYCODE_EISU 212
#define KEYCODE_MUHENKAN 213
#define KEYCODE_HENKAN 214
#define KEYCODE_KATAKANA_HIRAGANA 215
#define KEYCODE_YEN 216
#define KEYCODE_RO 217
#define KEYCODE_KANA 218
#define KEYCODE_ASSIST 219
#define KEYCODE_BRIGHTNESS_DOWN 220
#define KEYCODE_BRIGHTNESS_UP 221
#define KEYCODE_MEDIA_AUDIO_TRACK 222
#define KEYCODE_SLEEP 223
#define KEYCODE_WAKEUP 224
#define KEYCODE_PAIRING 225
#define KEYCODE_MEDIA_TOP_MENU 226
#define KEYCODE_11 227
#define KEYCODE_12 228
#define KEYCODE_LAST_CHANNEL 229
#define KEYCODE_TV_DATA_SERVICE 230
#define KEYCODE_VOICE_ASSIST 231
#define KEYCODE_TV_RADIO_SERVICE 232
#define KEYCODE_TV_TELETEXT 233
#define KEYCODE_TV_NUMBER_ENTRY 234
#define KEYCODE_TV_TERRESTRIAL_ANALOG 235
#define KEYCODE_TV_TERRESTRIAL_DIGITAL 236
#define KEYCODE_TV_SATELLITE 237
#define KEYCODE_TV_SATELLITE_BS 238
#define KEYCODE_TV_SATELLITE_CS 239
#define KEYCODE_TV_SATELLITE_SERVICE 240
#define KEYCODE_TV_NETWORK 241
#define KEYCODE_TV_ANTENNA_CABLE 242
#define KEYCODE_TV_INPUT_HDMI_1 243
#define KEYCODE_TV_INPUT_HDMI_2 244
#define KEYCODE_TV_INPUT_HDMI_3 245
#define KEYCODE_TV_INPUT_HDMI_4 246
#define KEYCODE_TV_INPUT_COMPOSITE_1 247
#define KEYCODE_TV_INPUT_COMPOSITE_2 248
#define KEYCODE_TV_INPUT_COMPONENT_1 249
#define KEYCODE_TV_INPUT_COMPONENT_2 250
#define KEYCODE_TV_INPUT_VGA_1 251
#define KEYCODE_TV_AUDIO_DESCRIPTION 252
#define KEYCODE_TV_AUDIO_DESCRIPTION_MIX_UP 253
#define KEYCODE_TV_AUDIO_DESCRIPTION_MIX_DOWN 254
#define KEYCODE_TV_ZOOM_MODE 255
#define KEYCODE_TV_CONTENTS_MENU 256
#define KEYCODE_TV_MEDIA_CONTEXT_MENU 257
#define KEYCODE_TV_TIMER_PROGRAMMING 258
#define KEYCODE_HELP 259
#define KEYCODE_NAVIGATE_PREVIOUS 260
#define KEYCODE_NAVIGATE_NEXT 261
#define KEYCODE_NAVIGATE_IN 262
#define KEYCODE_NAVIGATE_OUT 263
#define KEYCODE_STEM_PRIMARY 264
#define KEYCODE_STEM_1 265
#define KEYCODE_STEM_2 266
#define KEYCODE_STEM_3 267
#define KEYCODE_DPAD_UP_LEFT 268
#define KEYCODE_DPAD_DOWN_LEFT 269
#define KEYCODE_DPAD_UP_RIGHT 270
#define KEYCODE_DPAD_DOWN_RIGHT 271
#define KEYCODE_MEDIA_SKIP_FORWARD 272
#define KEYCODE_MEDIA_SKIP_BACKWARD 273
#define KEYCODE_MEDIA_STEP_FORWARD 274
#define KEYCODE_MEDIA_STEP_BACKWARD 275
#define KEYCODE_SOFT_SLEEP 276
#define KEYCODE_CUT 277
#define KEYCODE_COPY 278
#define KEYCODE_PASTE 279
#define KEYCODE_SYSTEM_NAVIGATION_UP 280
#define KEYCODE_SYSTEM_NAVIGATION_DOWN 281
#define KEYCODE_SYSTEM_NAVIGATION_LEFT 282
#define KEYCODE_SYSTEM_NAVIGATION_RIGHT 283
#define KEYCODE_ALL_APPS 284
#define KEYCODE_REFRESH 285
#define KEYCODE_THUMBS_UP 286
#define KEYCODE_THUMBS_DOWN 287
#define KEYCODE_PROFILE_SWITCH 288
#define KEYCODE_VIDEO_APP_1 289
#define KEYCODE_VIDEO_APP_2 290
#define KEYCODE_VIDEO_APP_3 291
#define KEYCODE_VIDEO_APP_4 292
#define KEYCODE_VIDEO_APP_5 293
#define KEYCODE_VIDEO_APP_6 294
#define KEYCODE_VIDEO_APP_7 295
#define KEYCODE_VIDEO_APP_8 296
#define KEYCODE_FEATURED_APP_1 297
#define KEYCODE_FEATURED_APP_2 298
#define KEYCODE_FEATURED_APP_3 299
#define KEYCODE_FEATURED_APP_4 300
#define KEYCODE_DEMO_APP_1 301
#define KEYCODE_DEMO_APP_2 302
#define KEYCODE_DEMO_APP_3 303
#define KEYCODE_DEMO_APP_4 304
#define KEYCODE_KEYBOARD_BACKLIGHT_DOWN 305
#define KEYCODE_KEYBOARD_BACKLIGHT_UP 306
#define KEYCODE_KEYBOARD_BACKLIGHT_TOGGLE 307
#define KEYCODE_STYLUS_BUTTON_PRIMARY 308
#define KEYCODE_STYLUS_BUTTON_SECONDARY 309
#define KEYCODE_STYLUS_BUTTON_TERTIARY 310
#define KEYCODE_STYLUS_BUTTON_TAIL 311
#define KEYCODE_RECENT_APPS 312
#define KEYCODE_MACRO_1 313
#define KEYCODE_MACRO_2 314
#define KEYCODE_MACRO_3 315
#define KEYCODE_MACRO_4 316
#define KEYCODE_EMOJI_PICKER 317
#define KEYCODE_SCREENSHOT 318
#define KEYCODE_DICTATE 319
#define KEYCODE_NEW 320
#define KEYCODE_CLOSE 321
#define KEYCODE_DO_NOT_DISTURB 322
#define KEYCODE_PRINT 323
#define KEYCODE_LOCK 324
#define KEYCODE_FULLSCREEN 325
#define KEYCODE_F13 326
#define KEYCODE_F14 327
#define KEYCODE_F15 328
#define KEYCODE_F16 329
#define KEYCODE_F17 330
#define KEYCODE_F18 331
#define KEYCODE_F19 332
#define KEYCODE_F20 333
#define KEYCODE_F21 334
#define KEYCODE_F22 335
#define KEYCODE_F23 336
#define KEYCODE_F24 337


static std::unordered_map<int, ax::EventKeyboard::KeyCode> g_keyCodeMap = {
    {KEYCODE_BACK,           ax::EventKeyboard::KeyCode::KEY_ESCAPE},
    {KEYCODE_MENU,           ax::EventKeyboard::KeyCode::KEY_MENU},
    {KEYCODE_DPAD_UP,        ax::EventKeyboard::KeyCode::KEY_DPAD_UP},
    {KEYCODE_DPAD_DOWN,      ax::EventKeyboard::KeyCode::KEY_DPAD_DOWN},
    {KEYCODE_DPAD_LEFT,      ax::EventKeyboard::KeyCode::KEY_DPAD_LEFT},
    {KEYCODE_DPAD_RIGHT,     ax::EventKeyboard::KeyCode::KEY_DPAD_RIGHT},
    {KEYCODE_DPAD_CENTER,    ax::EventKeyboard::KeyCode::KEY_DPAD_CENTER},
    {KEYCODE_ENTER,          ax::EventKeyboard::KeyCode::KEY_ENTER},
    {KEYCODE_MEDIA_PLAY,     ax::EventKeyboard::KeyCode::KEY_PLAY},
    {KEYCODE_VOLUME_UP,      ax::EventKeyboard::KeyCode::KEY_NONE}, // optional
    {KEYCODE_VOLUME_DOWN,    ax::EventKeyboard::KeyCode::KEY_NONE}, // optional
    {KEYCODE_SPACE,          ax::EventKeyboard::KeyCode::KEY_SPACE},
    {KEYCODE_TAB,            ax::EventKeyboard::KeyCode::KEY_TAB},
    {KEYCODE_SHIFT_LEFT,     ax::EventKeyboard::KeyCode::KEY_LEFT_SHIFT},
    {KEYCODE_SHIFT_RIGHT,    ax::EventKeyboard::KeyCode::KEY_RIGHT_SHIFT},
    {KEYCODE_ALT_LEFT,       ax::EventKeyboard::KeyCode::KEY_LEFT_ALT},
    {KEYCODE_ALT_RIGHT,      ax::EventKeyboard::KeyCode::KEY_RIGHT_ALT},
    {KEYCODE_CTRL_LEFT,      ax::EventKeyboard::KeyCode::KEY_LEFT_CTRL},
    {KEYCODE_CTRL_RIGHT,     ax::EventKeyboard::KeyCode::KEY_RIGHT_CTRL},
    {KEYCODE_ESCAPE,         ax::EventKeyboard::KeyCode::KEY_ESCAPE},
    {KEYCODE_F1,             ax::EventKeyboard::KeyCode::KEY_F1},
    {KEYCODE_F2,             ax::EventKeyboard::KeyCode::KEY_F2},
    {KEYCODE_F3,             ax::EventKeyboard::KeyCode::KEY_F3},
    {KEYCODE_F4,             ax::EventKeyboard::KeyCode::KEY_F4},
    {KEYCODE_F5,             ax::EventKeyboard::KeyCode::KEY_F5},
    {KEYCODE_F6,             ax::EventKeyboard::KeyCode::KEY_F6},
    {KEYCODE_F7,             ax::EventKeyboard::KeyCode::KEY_F7},
    {KEYCODE_F8,             ax::EventKeyboard::KeyCode::KEY_F8},
    {KEYCODE_F9,             ax::EventKeyboard::KeyCode::KEY_F9},
    {KEYCODE_F10,            ax::EventKeyboard::KeyCode::KEY_F10},
    {KEYCODE_F11,            ax::EventKeyboard::KeyCode::KEY_F11},
    {KEYCODE_F12,            ax::EventKeyboard::KeyCode::KEY_F12},
    {KEYCODE_0,              ax::EventKeyboard::KeyCode::KEY_0},
    {KEYCODE_1,              ax::EventKeyboard::KeyCode::KEY_1},
    {KEYCODE_2,              ax::EventKeyboard::KeyCode::KEY_2},
    {KEYCODE_3,              ax::EventKeyboard::KeyCode::KEY_3},
    {KEYCODE_4,              ax::EventKeyboard::KeyCode::KEY_4},
    {KEYCODE_5,              ax::EventKeyboard::KeyCode::KEY_5},
    {KEYCODE_6,              ax::EventKeyboard::KeyCode::KEY_6},
    {KEYCODE_7,              ax::EventKeyboard::KeyCode::KEY_7},
    {KEYCODE_8,              ax::EventKeyboard::KeyCode::KEY_8},
    {KEYCODE_9,              ax::EventKeyboard::KeyCode::KEY_9},
    {KEYCODE_A,              ax::EventKeyboard::KeyCode::KEY_A},
    {KEYCODE_B,              ax::EventKeyboard::KeyCode::KEY_B},
    {KEYCODE_C,              ax::EventKeyboard::KeyCode::KEY_C},
    {KEYCODE_D,              ax::EventKeyboard::KeyCode::KEY_D},
    {KEYCODE_E,              ax::EventKeyboard::KeyCode::KEY_E},
    {KEYCODE_F,              ax::EventKeyboard::KeyCode::KEY_F},
    {KEYCODE_G,              ax::EventKeyboard::KeyCode::KEY_G},
    {KEYCODE_H,              ax::EventKeyboard::KeyCode::KEY_H},
    {KEYCODE_I,              ax::EventKeyboard::KeyCode::KEY_I},
    {KEYCODE_J,              ax::EventKeyboard::KeyCode::KEY_J},
    {KEYCODE_K,              ax::EventKeyboard::KeyCode::KEY_K},
    {KEYCODE_L,              ax::EventKeyboard::KeyCode::KEY_L},
    {KEYCODE_M,              ax::EventKeyboard::KeyCode::KEY_M},
    {KEYCODE_N,              ax::EventKeyboard::KeyCode::KEY_N},
    {KEYCODE_O,              ax::EventKeyboard::KeyCode::KEY_O},
    {KEYCODE_P,              ax::EventKeyboard::KeyCode::KEY_P},
    {KEYCODE_Q,              ax::EventKeyboard::KeyCode::KEY_Q},
    {KEYCODE_R,              ax::EventKeyboard::KeyCode::KEY_R},
    {KEYCODE_S,              ax::EventKeyboard::KeyCode::KEY_S},
    {KEYCODE_T,              ax::EventKeyboard::KeyCode::KEY_T},
    {KEYCODE_U,              ax::EventKeyboard::KeyCode::KEY_U},
    {KEYCODE_V,              ax::EventKeyboard::KeyCode::KEY_V},
    {KEYCODE_W,              ax::EventKeyboard::KeyCode::KEY_W},
    {KEYCODE_X,              ax::EventKeyboard::KeyCode::KEY_X},
    {KEYCODE_Y,              ax::EventKeyboard::KeyCode::KEY_Y},
    {KEYCODE_Z,              ax::EventKeyboard::KeyCode::KEY_Z},
};



JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeTouchesCancel(JNIEnv* env,
                                                                          jclass,
                                                                          jintArray jIds,
                                                                          jfloatArray jXs,
                                                                          jfloatArray jYs,
                                                                          jfloatArray jPressures,
                                                                          jint size)
{
    if (size <= 0)
        return;

    jint* ids         = (jint*)env->GetPrimitiveArrayCritical(jIds, nullptr);
    jfloat* xs        = (jfloat*)env->GetPrimitiveArrayCritical(jXs, nullptr);
    jfloat* ys        = (jfloat*)env->GetPrimitiveArrayCritical(jYs, nullptr);
    jfloat* pressures = (jfloat*)env->GetPrimitiveArrayCritical(jPressures, nullptr);

    size = std::min(size, AX_MAX_TOUCHES);

    tlx::static_vector<TouchPoint, AX_MAX_TOUCHES> touchPoints;
    if (ids && xs && ys && pressures)
    {
        for (int i = 0; i < size; ++i)
            touchPoints.push_back(
                {.id = static_cast<intptr_t>(ids[i]), .x = xs[i], .y = ys[i], .pressure = pressures[i]});
    }

    if (pressures)
        env->ReleasePrimitiveArrayCritical(jPressures, pressures, JNI_ABORT);
    if (ys)
        env->ReleasePrimitiveArrayCritical(jYs, ys, JNI_ABORT);
    if (xs)
        env->ReleasePrimitiveArrayCritical(jXs, xs, JNI_ABORT);
    if (ids)
        env->ReleasePrimitiveArrayCritical(jIds, ids, JNI_ABORT);

    ax::Director::getInstance()->postTask([touchPoints = std::move(touchPoints)]() {
        auto inputSys = ax::InputSystem::getInstance();
        for (auto& touchPoint : touchPoints)
        {
            auto state = ax::PointerInputState{
                .id = touchPoint.id, .pressure = touchPoint.pressure, .type = ax::PointerType::Touch};
            inputSys->handlePointerCancel(Vec2(touchPoint.x, touchPoint.y), state);
        }
    }, Director::TaskTiming::FrameBoundary);
}


JNIEXPORT jboolean JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeKeyEvent(JNIEnv*,
                                                                         jclass,
                                                                         jint keyCode,
                                                                         jboolean isPressed)
{
    auto iterKeyCode = g_keyCodeMap.find(keyCode);
    if (iterKeyCode == g_keyCodeMap.end())
    {
        return JNI_FALSE;
    }

    ax::InputSystem::getInstance()->handleKeyEvent(iterKeyCode->second,
                                                   isPressed ? InputPhase::KeyDown : InputPhase::KeyUp);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeOnPause(JNIEnv*, jclass)
{
    if (Director::getInstance()->getRenderView())
    {
        Application::getInstance()->applicationDidEnterBackground();
        ax::CustomEvent backgroundEvent(EVENT_COME_TO_BACKGROUND);
        ax::Director::getInstance()->getEventDispatcher()->dispatchEvent(&backgroundEvent, true);
    }
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeOnResume(JNIEnv*, jclass)
{
    if (Director::getInstance()->getRenderView())
    {
        Application::getInstance()->applicationWillEnterForeground();
        ax::CustomEvent foregroundEvent(EVENT_COME_TO_FOREGROUND);
        ax::Director::getInstance()->getEventDispatcher()->dispatchEvent(&foregroundEvent, true);
    }
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeInsertText(JNIEnv* env, jclass, jstring text)
{
    std::string strValue = ax::text_utils::getStringUTFCharsJNI(env, text);
    ax::InputSystem::getInstance()->dispatchInsertText(strValue);
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeDeleteBackward(JNIEnv*, jclass, jint numChars)
{
    ax::InputSystem::getInstance()->dispatchDeleteBackward(static_cast<unsigned int>(numChars));
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeSoftInputShow(JNIEnv* env,
                                                                          jclass,
                                                                          jfloat x,
                                                                          jfloat y,
                                                                          jfloat width,
                                                                          jfloat height,
                                                                          jfloat duration)
{
    if (auto inputSys = ax::InputSystem::getInstance())
    {
        inputSys->onPlatformKeyboardWillShow(x, y, width, height, duration);
        inputSys->onPlatformKeyboardDidShow();
    }
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativeSoftInputHide(JNIEnv*, jclass, float duration)
{
    if (auto inputSys = ax::InputSystem::getInstance())
    {
        inputSys->onPlatformKeyboardWillHide(duration);
        inputSys->onPlatformKeyboardDidHide();
    }
}

JNIEXPORT void JNICALL Java_dev_axmol_lib_AxmolPlayer_nativePerformEditAction(JNIEnv*, jclass, int action)
{
    ax::InputSystem::getInstance()->dispatchPerformEditAction(static_cast<ax::EditAction>(action));
}
}
