// Controllers, as the games read them: XInput's view (XINPUT_STATE) of every
// pad. Shared by BF1943 and Bad Company (both build this runtime).
//
//  - Windows: XInput serves Xbox pads (and anything that looks like one, e.g.
//    Steam Input's or DS4Windows' virtual pads). SDL3's gamepad API serves
//    all the others (DualShock 3/4, DualSense, Switch Pro, Steam Deck, generic
//    HID pads), into the users XInput leaves free, so no pad counts twice.
//  - Linux and macOS (BF1943_CLIENT): SDL3 serves all of them (XInput does not
//    exist there); the window's event loop feeds hot-plug (OnSdlEvent).
//  - The Linux console server has no pads.
#pragma once
#include <cstdint>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#else
#include "kernel.h"
#include "posix_input.h"
#endif

namespace rt::pad {

// The pad's make, for the button prompts (PC settings: BUTTON PROMPTS = AUTO).
enum class Kind : int { kNone, kXbox360, kXboxSeries, kPs3, kPs5, kSteamDeck, kSwitch, kGeneric };

// Starts the SDL side (Windows: its own thread; once, safe to call again).
void Init();
// XInput's functions over every pad (user 0..3). ERROR_SUCCESS (0) or
// ERROR_DEVICE_NOT_CONNECTED (1167).
DWORD GetState(DWORD user, XINPUT_STATE* state);
DWORD SetState(DWORD user, XINPUT_VIBRATION* vibration);
DWORD GetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps);
// The pad that was used last (else the first connected); kNone with none connected.
Kind ActiveKind();
// A controller is connected (any user, XInput or SDL). No side effects: the
// keyboard and mouse setting reads it every poll.
bool Connected();
// A short name for the log and the PC settings ("DualSense", "Xbox controller").
const char* ActiveName();
// The device the player last used, for BUTTON PROMPTS = AUTO: kNone for the
// keyboard and mouse (also before any input), else the pad's make. A pad
// counts on a real press (GetState: a button, a trigger, a stick past a dead
// zone, newly), the keyboard and mouse on a key, a click or a move (xam.cpp).
Kind UsedKind();
void NoteKeyboardMouse();
void NotePad(DWORD user, const XINPUT_GAMEPAD& gamepad, Kind kind);
// The set the menus' pad prompts use: the BUTTON PROMPTS setting (0 auto, 1 keys,
// 2 Xbox 360, 3 Xbox Series, 4 PS3, 5 PS5, 6 Steam Deck; g_prompt_style), AUTO
// the pad last used (else the connected pad's make); never kNone (no pad, or a
// plain pad: Xbox 360).
Kind PromptKind(int style);
// A button's name in the prompt set of a kind: "a" "b" "x" "y" "lb" "rb" "start" "back".
// PlayStation pads: the cross, circle, square and triangle read X, O, [], /\ in text.
const wchar_t* ButtonLabel(Kind kind, const char* button);

}  // namespace rt::pad

#if defined(BF1943_CLIENT) || defined(BF_SDL_PADS)
union SDL_Event;
namespace rt::pad {
// A game window's SDL event (hot-plug); Linux and macOS, where the window's
// loop runs it. (Windows has a thread of its own.)
void OnSdlEvent(const SDL_Event& event);
}  // namespace rt::pad
#endif
