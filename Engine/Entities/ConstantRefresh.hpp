/**
 *  ConstantRefresh.hpp
 *  ONScripter-RU.
 *
 *  Constant refresh support and its actions.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "External/Compatibility.hpp"
#include "Engine/Components/DynamicProperty.hpp"
#include "Support/KeyState.hpp"
#include "Support/Clock.hpp"
#include "Support/WorkScheduler.hpp"

#include "Support/SDLCompat.hpp"

#include <unordered_set>
#include <deque>
#include <memory>
#include <iostream>

const int ONS_UPKEEP_EVENT{SDL_USEREVENT + 2};
const int ONS_EVENT_BATCH_END{SDL_USEREVENT + 3};
const int ONS_CHUNK_EVENT{SDL_USEREVENT + 4};

enum {
	REFRESH_NONE_MODE        = 0,
	REFRESH_NORMAL_MODE      = 1,
	REFRESH_SAYA_MODE        = 2,
	REFRESH_WINDOW_MODE      = 4,  //show textwindow background
	REFRESH_TEXT_MODE        = 8,  //show textwindow text
	REFRESH_CURSOR_MODE      = 16, //show textwindow cursor
	CONSTANT_REFRESH_MODE    = 32,
	REFRESH_BEFORESCENE_MODE = 64, // refresh based on ai->old_ai
	REFRESH_SOMETHING        = REFRESH_NORMAL_MODE |
	                    REFRESH_SAYA_MODE |
	                    REFRESH_WINDOW_MODE |
	                    REFRESH_TEXT_MODE |
	                    REFRESH_CURSOR_MODE
};

inline bool isInputEvent(Uint32 eventType) {
	switch (eventType) {
		case SDL_MOUSEWHEEL:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
		case ONS_MULTIGESTURE_EVENT:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		case SDL_JOYHATMOTION:
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
		case SDL_JOYAXISMOTION:
#if ONS_USE_SDL3
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
#endif
			return true;
		default:
			return false;
	}
}

extern std::deque<std::function<void()>> postponedEventChanges;     // contains fns to make changes to global state, put to while processing each event
extern std::unordered_set<const char *> postponedEventChangeLabels; // contains unique labels to prevent multiple adding of events that should be run only once
void addToPostponedEventChanges(std::function<void()> f);
void addToPostponedEventChanges(const char *str, std::function<void()> f);

// abstract base class
class ConstantRefreshAction {
public:
	Clock clock;
	bool terminated{false};
	bool createdDuringDialogueInline{false};
	int event_mode{0};
	virtual int eventMode() {
		return event_mode;
	}
	virtual bool expired() = 0;
	virtual void run() {}
	virtual uint64_t nextUpdateNanos() { return 0; }
	virtual void advance(uint64_t ns) {
		clock.tickNanos(ns);
	}
	virtual void onExpired();
	virtual void terminate() {
		terminated = true;
	}
	virtual bool suspendsMainScript() {
		return !createdDuringDialogueInline;
	}
	virtual bool suspendsDialogue() {
		return createdDuringDialogueInline;
	}
	virtual bool handlesEvent(Uint32) const {
		return false;
	}
	virtual void initialize();
	virtual ~ConstantRefreshAction() = default;

protected:
	ConstantRefreshAction() {}
};

std::vector<std::shared_ptr<ConstantRefreshAction>> getConstantRefreshActions();
std::shared_ptr<ConstantRefreshAction> currentAction(unsigned int handler);
bool isWaitingForUserInput();
bool isWaitingForUserInterrupt();

template <class T>
class TypedConstantRefreshAction : public ConstantRefreshAction {
public:
	static T *create() {
		T *ret{new T()};
		ret->initialize();
		return ret;
	}
	static bool isCurrent(unsigned int handler) {
		auto cur = currentAction(handler);
		return !!cur && dynamic_cast<T *>(cur.get());
	}
};

template <class T>
static std::deque<std::shared_ptr<ConstantRefreshAction>> fetchConstantRefreshActions() {
	static_assert(std::is_base_of<ConstantRefreshAction, T>::value, "fetchEvents assertion failure: event type must extend ConstantRefreshAction");
	std::deque<std::shared_ptr<ConstantRefreshAction>> ret;
	for (const auto &a : getConstantRefreshActions()) {
		if (dynamic_cast<T *>(a.get())) {
			ret.push_back(a);
		}
	}
	return ret;
}

template <class T>
class AbstractWaitAction : public TypedConstantRefreshAction<T> {
public:
	int advanceProperties{0};
	uint64_t nextUpdateNanos() override { return this->clock.remainingNanos(); }
	bool handlesEvent(Uint32 eventType) const override {
		return isInputEvent(eventType);
	}
	bool expired() override {
		return this->clock.expired();
	}
	void onExpired() override {
		ConstantRefreshAction::onExpired();
		dynamicProperties.advance(advanceProperties); // advance the time we skipped
		dynamicProperties.apply();
	}
};

class WaitAction : public AbstractWaitAction<WaitAction> {};
class DelayAction : public AbstractWaitAction<DelayAction> {};
class WaitTimerAction : public AbstractWaitAction<WaitTimerAction> {};

class WaitVoiceAction : public TypedConstantRefreshAction<WaitVoiceAction> {
	bool countDownStarted{false};

public:
	bool handlesEvent(Uint32 eventType) const override {
		return isInputEvent(eventType) || eventType == ONS_CHUNK_EVENT;
	}
	uint32_t voiceDelayMs{0};
	bool expired() override;
	uint64_t nextUpdateNanos() override { return countDownStarted ? clock.remainingNanos() : WorkSchedule::Never; }
};

class QueuedSoundAction : public TypedConstantRefreshAction<QueuedSoundAction> {
	bool countDownStarted{false};

public:
	int32_t ch{-1};
	uint32_t soundDelayMs{0};
	// Keep this as a raw callback because queued sounds only need a plain completion hook.
	void (*func)(){nullptr};
	bool suspendsMainScript() override {
		return false;
	}
	bool suspendsDialogue() override {
		return false;
	}
	bool expired() override;
	uint64_t nextUpdateNanos() override { return countDownStarted ? clock.remainingNanos() : WorkSchedule::Never; }
	void onExpired() override;
};

class ButtonWaitAction : public AbstractWaitAction<ButtonWaitAction> {
public:
	uint32_t button_timer_start{0};
	std::shared_ptr<void> variableInfo;
	ButtonState buttonState;
	bool del_flag{false};
	bool timer_set{false};
	uint64_t nextUpdateNanos() override { return timer_set ? clock.remainingNanos() : WorkSchedule::Never; }
	bool voiced_txtbtnwait{false};
	bool final_voiced_txtbtnwait{false};
	bool expired() override {
		return timer_set && clock.expired();
	}
	void onExpired() override;
	bool handlesEvent(Uint32 eventType) const override {
		//FIXME: design-wise there should be some condition (WAIT_VOICE_MODE?)
		return isInputEvent(eventType) || eventType == SDL_MOUSEMOTION || eventType == ONS_CHUNK_EVENT;
	}
};

class ButtonMonitorAction : public TypedConstantRefreshAction<ButtonMonitorAction> {
public:
	bool suspendsMainScript() override {
		return false;
	}
	bool suspendsDialogue() override {
		return false;
	}
	bool handlesEvent(Uint32 eventType) const override {
		//FIXME: design-wise there should be some condition (WAIT_VOICE_MODE?)
		return isInputEvent(eventType) || eventType == SDL_MOUSEMOTION || eventType == ONS_CHUNK_EVENT;
	}
	bool expired() override { return false; }
	uint64_t nextUpdateNanos() override { return WorkSchedule::Never; }
	void keepAlive() { terminated = false; }
	ButtonState buttonState;
};
