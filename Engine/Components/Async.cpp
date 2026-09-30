/**
 *  Async.cpp
 *  ONScripter-RU
 *
 *  Asynchronuous execution management and threading support.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Components/Async.hpp"
#include "Engine/Core/ONScripter.hpp"
#include "Engine/Core/Parser.hpp"
#include "Engine/Media/Controller.hpp"
#include "Engine/Layers/Subtitle.hpp"
#include "Support/FileDefs.hpp"

AsyncController async;

// Must be called during ONS initialization to once-only initialize mutexes etc
int AsyncController::ownInit() {
	/* Create all mutexes */
	mutexes.init();

	for (AsyncInstructionQueue *qPtr : queueCollection) {
		qPtr->init();
	}

	startEventQueue();

	return 0;
}

int AsyncController::ownDeinit() {
	endThreads();
	for (AsyncInstructionQueue *qPtr : queueCollection)
		qPtr->destroy();
	mutexes.deinit();
	return 0;
}

AsyncController::AsyncController()
    : BaseController(this),
      imageCacheQueue("imageCacheQueue"),
      soundCacheQueue("soundCacheQueue"),
      loadImageQueue("loadImageQueue", false /*don't quit!*/),
      imageAssetQueue("imageAssetQueue", false),
      loadPacketArraysQueue("loadPacketArraysQueue", false),
      loadFramesQueue{{"loadVideoFramesQueue", false},
                      {"loadAudioFramesQueue", false},
                      {"loadSubtitleFramesQueue", false}},
      playSoundQueue("playSoundQueue", false),
      eventQueueQueue("eventQueueQueue", false, false /*needs no instructions*/) {
	imageCacheQueue.threadLoopFunction                                  = imageCacheThreadLoop;
	soundCacheQueue.threadLoopFunction                                  = soundCacheThreadLoop;
	loadImageQueue.threadLoopFunction                                   = loadImageThreadLoop;
	imageAssetQueue.threadLoopFunction                                  = imageAssetThreadLoop;
	loadPacketArraysQueue.threadLoopFunction                            = loadPacketArraysThreadLoop;
	loadFramesQueue[MediaProcController::VideoEntry].threadLoopFunction = loadVideoFramesThreadLoop;
	loadFramesQueue[MediaProcController::AudioEntry].threadLoopFunction = loadAudioFramesThreadLoop;
	loadFramesQueue[MediaProcController::SubsEntry].threadLoopFunction  = loadSubtitleFramesThreadLoop;
	playSoundQueue.threadLoopFunction                                   = playSoundThreadLoop;
	eventQueueQueue.threadLoopFunction                                  = eventQueueThreadLoop;

	queueCollection.push_back(&imageCacheQueue);
	queueCollection.push_back(&soundCacheQueue);
	queueCollection.push_back(&loadImageQueue);
	queueCollection.push_back(&imageAssetQueue);
	queueCollection.push_back(&loadFramesQueue[MediaProcController::VideoEntry]);
	queueCollection.push_back(&loadFramesQueue[MediaProcController::AudioEntry]);
	queueCollection.push_back(&loadFramesQueue[MediaProcController::SubsEntry]);
	queueCollection.push_back(&loadPacketArraysQueue);
	queueCollection.push_back(&playSoundQueue);
	queueCollection.push_back(&eventQueueQueue);
}

void AsyncController::endThreads() {
	threadShutdownRequested = true;
	mainThreadWork.notify();

	for (AsyncInstructionQueue *qPtr : queueCollection) {
		sendToLog(LogLevel::Info, "[Info] AsyncController is going to kill %s-based thread\n", qPtr->name);
		qPtr->threadStopFunction(qPtr);
	}

	threadShutdownRequested = false;
}

void AsyncController::queue(std::unique_ptr<AsyncInstruction> inst, bool priority) {
	AsyncInstructionQueue *instQueue = inst->getInstructionQueue();
	// Runs thread if it is not already running
	SDL_AtomicLock(&instQueue->lock);
	if (instQueue->thread && !instQueue->running) {
		SDL_Thread *finishedThread = instQueue->thread;
		instQueue->thread          = nullptr;
		SDL_AtomicUnlock(&instQueue->lock);
		SDL_WaitThread(finishedThread, nullptr);
		SDL_AtomicLock(&instQueue->lock);
	}
	if (priority)
		instQueue->q.push_front(std::move(inst));
	else
		instQueue->q.push_back(std::move(inst));
	if (!instQueue->quitOnEmpty)
		SDL_SemPost(instQueue->instructionsWaiting);
	if (!instQueue->running) {
		instQueue->running = true;
		instQueue->thread = SDL_CreateThread(instQueue->threadLoopFunction,
		                                     instQueue->name,
		                                     instQueue->q.back()->ac);
		if (!instQueue->thread) {
			instQueue->running = false;
			SDL_AtomicUnlock(&instQueue->lock);
			throw std::runtime_error(std::string("Could not start async thread '") +
			                         instQueue->name + "': " + SDL_GetError());
		}
	}
	SDL_AtomicUnlock(&instQueue->lock);
}

// Main genericized async loop function
int AsyncController::asyncLoop(AsyncInstructionQueue &queue) {
	while (true) {
		if (threadShutdownRequested)
			break;

		if (!queue.quitOnEmpty && queue.hasQueue) {
			SDL_SemWait(queue.instructionsWaiting);
		}

		if (threadShutdownRequested)
			break;

		SDL_AtomicLock(&queue.lock);
		if (!queue.q.empty()) {
			std::unique_ptr<AsyncInstruction> inst;
			AsyncInstruction *ptr;

			if (queue.hasQueue) {
				inst = std::move(queue.q.front());
				queue.q.pop_front();
				ptr = inst.get();
			} else {
				ptr = queue.q.front().get();
			}

			SDL_AtomicUnlock(&queue.lock);

			//WARNING: It is assumed that queue is not accessed at this step
			try {
				ptr->execute(); // Do the actual work
			} catch (ThreadTerminate &) {
				SDL_SemPost(queue.resultsWaiting);
				mainThreadWork.notify();
				break;
			}

			SDL_AtomicLock(&queue.lock);
			if (!queue.quitOnEmpty && queue.hasQueue) {
				SDL_SemPost(queue.resultsWaiting);
				mainThreadWork.notify();
			}
			if (threadShutdownRequested || (queue.q.empty() && queue.quitOnEmpty)) {
				// Publish completion under the same lock used by queue(). Otherwise
				// a producer can append work to a thread that is about to exit.
				queue.running = false;
				SDL_AtomicUnlock(&queue.lock);
				return 0;
			}
		}
		SDL_AtomicUnlock(&queue.lock);
	}
	queue.running = false;
	return 0;
}

/* ---------------- Async Instruction Queue  ----------------- */

void AsyncInstructionQueue::init() {
	instructionsWaiting = SDL_CreateSemaphore(0);
	resultsWaiting      = SDL_CreateSemaphore(0);
	if (!instructionsWaiting || !resultsWaiting) {
		destroy();
		throw std::runtime_error(std::string("Could not initialize async queue '") +
		                         name + "': " + SDL_GetError());
	}
}

void AsyncInstructionQueue::destroy() {
	if (thread)
		throw std::logic_error(std::string("Destroying running async queue '") + name + "'");
	if (instructionsWaiting) {
		SDL_DestroySemaphore(instructionsWaiting);
		instructionsWaiting = nullptr;
	}
	if (resultsWaiting) {
		SDL_DestroySemaphore(resultsWaiting);
		resultsWaiting = nullptr;
	}
}

void defaultThreadEnd(AsyncInstructionQueue *qPtr) {
	// It might be suspended on a semaphore waiting for an instruction. If so, wake it up so it can exit.
	if (!qPtr->quitOnEmpty && qPtr->instructionsWaiting)
		SDL_SemPost(qPtr->instructionsWaiting);

	// Retain the SDL thread handle until its resources have been joined.
	SDL_AtomicLock(&qPtr->lock);
	SDL_Thread *thread = qPtr->thread;
	SDL_AtomicUnlock(&qPtr->lock);
	if (thread)
		SDL_WaitThread(thread, nullptr);

	// Reset queue state without invalidating semaphore pointers that other code owns.
	SDL_AtomicLock(&qPtr->lock);
	if (qPtr->thread == thread)
		qPtr->thread = nullptr;
	qPtr->running = false;
	qPtr->q.clear();
	while (qPtr->instructionsWaiting && onsTryWaitSemaphore(qPtr->instructionsWaiting)) {}
	while (qPtr->resultsWaiting && onsTryWaitSemaphore(qPtr->resultsWaiting)) {}
	SDL_AtomicUnlock(&qPtr->lock);
}

/* ---------------- Virtual Mutexes ----------------- */

void VirtualMutexes::init() {
	//currently empty
}

void VirtualMutexes::deinit() {
	SDL_AtomicLock(&access_mutex);
	for (auto &[ptr, mutex] : mutexes) {
		(void)ptr;
		SDL_DestroyMutex(mutex);
	}
	mutexes.clear();
	for (auto &[id, semaphore] : semaphores) {
		(void)id;
		SDL_DestroySemaphore(semaphore);
	}
	semaphores.clear();
	SDL_AtomicUnlock(&access_mutex);
}

void VirtualMutexes::setMutex(void *ptr) {
	SDL_mutex *m = nullptr;
	SDL_AtomicLock(&access_mutex);
	if (!ptr) {
		SDL_AtomicUnlock(&access_mutex);
		throw std::runtime_error("Resource is dead");
	}

	auto it = mutexes.find(ptr);
	if (it != mutexes.end()) {
		m = it->second;
	} else {
		m = SDL_CreateMutex();
		mutexes.emplace(ptr, m);
	}
	SDL_AtomicUnlock(&access_mutex);
	SDL_mutexP(m);
}

void VirtualMutexes::unsetMutex(void *ptr) {
	SDL_mutex *m = nullptr;
	SDL_AtomicLock(&access_mutex);
	auto it = mutexes.find(ptr);
	if (it != mutexes.end()) {
		m = it->second;
	} else {
		SDL_AtomicUnlock(&access_mutex);
		throw std::runtime_error("Amen, uncreated mutex was released into heavens");
	}
	SDL_AtomicUnlock(&access_mutex);
	SDL_mutexV(m);
}

void VirtualMutexes::debugJoin(int debug1, int debug2) {
	SDL_sem *s1 = nullptr;
	SDL_sem *s2 = nullptr;
	SDL_AtomicLock(&access_mutex);
	auto it = semaphores.find(debug1);
	if (it != semaphores.end()) {
		s1 = it->second;
		auto it2 = semaphores.find(debug2);
		if (it2 != semaphores.end()) {
			s2 = it2->second;
		} else {
			s2 = SDL_CreateSemaphore(0);
			semaphores.emplace(debug2, s2);
		}
	} else {
		s1 = SDL_CreateSemaphore(0);
		s2 = SDL_CreateSemaphore(0);
		semaphores.emplace(debug1, s1);
		semaphores.emplace(debug2, s2);
	}
	SDL_AtomicUnlock(&access_mutex);
	SDL_SemPost(s2);                     //<- important part
	int r = onsWaitSemaphoreTimeoutResult(s1, 100); //<- important part
	if (r == SDL_MUTEX_TIMEDOUT) {
		SDL_SemTryWait(s2); // take it away again, but don't block if the other thread just consumed it in a case of really bad timing
	}
}

/* ---------------- Load image cache instruction ----------------- */

void LoadImageCacheInstruction::execute() {
	ons.loadImageIntoCache(id, filename, allow_rgb, true);
}

AsyncInstructionQueue *LoadImageCacheInstruction::getInstructionQueue() {
	return &ac->imageCacheQueue;
}

void AsyncController::cacheImage(int id, const std::string &filename, bool allow_rgb) {
	std::string stringForNewThread(filename.data(), filename.length()); // avoids possible COW issues
	queue(std::make_unique<LoadImageCacheInstruction>(this, id, stringForNewThread, allow_rgb));
}

int imageCacheThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->imageCacheQueue);
}

/* ---------------- Load sound cache instruction ----------------- */

void LoadSoundCacheInstruction::execute() {
	ons.loadSoundIntoCache(id, filename, true);
}

AsyncInstructionQueue *LoadSoundCacheInstruction::getInstructionQueue() {
	return &ac->soundCacheQueue;
}

void AsyncController::cacheSound(int id, const std::string &filename) {
	std::string stringForNewThread(filename.data(), filename.length()); // avoids possible COW issues
	queue(std::make_unique<LoadSoundCacheInstruction>(this, id, stringForNewThread));
}

int soundCacheThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->soundCacheQueue);
}

/* ----------------- Load image instruction ----------------- */

void PrepareImageInstruction::execute() {
	ons.prepareImageAsset(job);
}

AsyncInstructionQueue *PrepareImageInstruction::getInstructionQueue() {
	return &ac->imageAssetQueue;
}

void AsyncController::prepareImage(std::shared_ptr<ImageAssetJob> job, bool priority) {
	SDL_AtomicLock(&imageAssetQueue.lock);
	std::erase_if(imageAssetQueue.q, [](const auto &instruction) {
		return static_cast<PrepareImageInstruction *>(instruction.get())->job->cancelled.load(std::memory_order_relaxed);
	});
	SDL_AtomicUnlock(&imageAssetQueue.lock);
	queue(std::make_unique<PrepareImageInstruction>(this, std::move(job)), priority);
}

int imageAssetThreadLoop(void *arg) {
	auto *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->imageAssetQueue);
}

void LoadImageInstruction::execute() {
	ons.buildAIImage(aiPtr);
}

AsyncInstructionQueue *LoadImageInstruction::getInstructionQueue() {
	return &ac->loadImageQueue;
}

void AsyncController::loadImage(AnimationInfo *ai) {
	queue(std::make_unique<LoadImageInstruction>(this, ai));
}

int loadImageThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->loadImageQueue);
}

/* -------------- Load packet arrays instruction -------------- */

void LoadPacketArraysInstruction::execute() {
	media.demultiplexStreams();
}

AsyncInstructionQueue *LoadPacketArraysInstruction::getInstructionQueue() {
	return &ac->loadPacketArraysQueue;
}

void AsyncController::loadPacketArrays() {
	queue(std::make_unique<LoadPacketArraysInstruction>(this));
}

int loadPacketArraysThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->loadPacketArraysQueue);
}

/* -------------- Load video frame instruction -------------- */

void LoadVideoFramesInstruction::execute() {
	media.decodeFrames(MediaProcController::VideoEntry);
}

AsyncInstructionQueue *LoadVideoFramesInstruction::getInstructionQueue() {
	return &ac->loadFramesQueue[MediaProcController::VideoEntry];
}

void AsyncController::loadVideoFrames() {
	queue(std::make_unique<LoadVideoFramesInstruction>(this));
}

int loadVideoFramesThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->loadFramesQueue[MediaProcController::VideoEntry]);
}

/* -------------- Load audio frame instruction -------------- */

void LoadAudioFramesInstruction::execute() {
	media.decodeFrames(MediaProcController::AudioEntry);
}

AsyncInstructionQueue *LoadAudioFramesInstruction::getInstructionQueue() {
	return &ac->loadFramesQueue[MediaProcController::AudioEntry];
}

void AsyncController::loadAudioFrames() {
	queue(std::make_unique<LoadAudioFramesInstruction>(this));
}

int loadAudioFramesThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->loadFramesQueue[MediaProcController::AudioEntry]);
}

/* -------------- Load subtitle frame instruction -------------- */

void LoadSubtitleFramesInstruction::execute() {
	sl->doDecoding();
}

AsyncInstructionQueue *LoadSubtitleFramesInstruction::getInstructionQueue() {
	return &ac->loadFramesQueue[MediaProcController::SubsEntry];
}

void AsyncController::loadSubtitleFrames(SubtitleLayer *sl) {
	queue(std::make_unique<LoadSubtitleFramesInstruction>(this, sl));
}

int loadSubtitleFramesThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->loadFramesQueue[MediaProcController::SubsEntry]);
}

/* -------------- Play sound instruction -------------- */

void PlaySoundInstruction::execute() {
	auto r = static_cast<uintptr_t>(ons.playSound(filename.c_str(), format, loop_flag, channel));
	SDL_AtomicLock(&ac->playSoundQueue.resultsLock);
	ac->playSoundQueue.results.push_back(reinterpret_cast<void *>(r));
	SDL_AtomicUnlock(&ac->playSoundQueue.resultsLock);
}

AsyncInstructionQueue *PlaySoundInstruction::getInstructionQueue() {
	return &ac->playSoundQueue;
}

void AsyncController::playSound(const char *filename, int format, bool loop_flag, int channel) {
	queue(std::make_unique<PlaySoundInstruction>(this, filename, format, loop_flag, channel));
}

int playSoundThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->playSoundQueue);
}

/* -------------- Event Queue instruction -------------- */

void EventQueueInstruction::execute() {
	ons.fetchEventsToQueue();
}

AsyncInstructionQueue *EventQueueInstruction::getInstructionQueue() {
	return &ac->eventQueueQueue;
}

void AsyncController::startEventQueue() {
	queue(std::make_unique<EventQueueInstruction>(this));
}

int eventQueueThreadLoop(void *arg) {
	AsyncController *ac = static_cast<AsyncController *>(arg);
	return ac->asyncLoop(ac->eventQueueQueue);
}
