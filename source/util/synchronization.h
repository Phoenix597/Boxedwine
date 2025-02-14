#ifndef __SYNCHRONIZATION_H__
#define __SYNCHRONIZATION_H__

class BoxedWineCondition;

class BoxedWineConditionChild {
public:
    BoxedWineConditionChild(BoxedWineCondition* cond, const std::function<void(void)>& doneWaitingCallback) : cond(cond), doneWaitingCallback(doneWaitingCallback) {}
    BoxedWineCondition* cond;
    std::function<void(void)> doneWaitingCallback;
};

#ifdef BOXEDWINE_MULTI_THREADED
class BoxedWineCondition {
public:
    BoxedWineCondition(BString name);
    BoxedWineCondition();

    bool tryLock();
    void lock();
    void signal();
    void signalAll();
    void wait(std::unique_lock<std::mutex>& lock);
    void waitWithTimeout(std::unique_lock<std::mutex>& lock, U32 ms);
    void unlock();
    void setParentCondition(BoxedWineCondition* parent);

    const BString name;

    std::mutex m;
    std::condition_variable c;
    U32 lockOwner;
    BoxedWineCondition* parent;
};

class BoxedWineCriticalSectionCond {
public:
    BoxedWineCriticalSectionCond(BoxedWineCondition* cond);
    ~BoxedWineCriticalSectionCond();

private:
    BoxedWineCondition* cond;
};

#define BOXEDWINE_CRITICAL_SECTION static std::mutex csMutex; const std::lock_guard<std::mutex> lock(csMutex);
#define BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(csMutex) const std::lock_guard<std::recursive_mutex> lock(csMutex);
#define BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(csCond) std::unique_lock<std::mutex> boxedWineCriticalSection(csCond.m);

#define BOXEDWINE_MUTEX std::recursive_mutex
#define BOXEDWINE_MUTEX_LOCK(mutex) mutex.lock()
#define BOXEDWINE_MUTEX_TRY_LOCK(mutex) mutex.try_lock()
#define BOXEDWINE_MUTEX_UNLOCK(mutex) mutex.unlock()

#define BOXEDWINE_CONDITION BoxedWineCondition
#define BOXEDWINE_CONDITION_SIGNAL(cond) cond.signal()
#define BOXEDWINE_CONDITION_SIGNAL_ALL(cond) cond.signalAll()
#define BOXEDWINE_CONDITION_WAIT(cond) cond.wait(boxedWineCriticalSection)
#define BOXEDWINE_CONDITION_WAIT_TIMEOUT(cond, t) cond.waitWithTimeout(boxedWineCriticalSection, t)
#define BOXEDWINE_CONDITION_SET_PARENT(cond, parent) cond.setParentCondition(parent)

#define BoxedWineConditionTimer BoxedWineCondition
#else
#define BOXEDWINE_CRITICAL_SECTION
#define BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(csMutex)
#define BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(csCond)

typedef void* BOXEDWINE_MUTEX;
#define BOXEDWINE_MUTEX_LOCK(mutex)
#define BOXEDWINE_MUTEX_TRY_LOCK(mutex) true
#define BOXEDWINE_MUTEX_UNLOCK(mutex)

class KThread;
class BoxedWineCondition;

class BoxedWineConditionTimer : public KTimer { 
public:
    BoxedWineConditionTimer() : cond(0){};
    bool run();

    BoxedWineCondition* cond;
};

class BoxedWineCondition {
public:
    BoxedWineCondition(BString name);
    ~BoxedWineCondition();

    void signal();
    void signalAll();
    U32 wait();
    U32 waitWithTimeout(U32 ms);
    U32 waitCount();

    void setParentCondition(BoxedWineCondition* parent);

    const BString name;
    BoxedWineCondition* parent;
private:
    KList<KThread*> waitingThreads;    

    friend BoxedWineConditionTimer;
    void signalThread(bool all);
};

typedef BoxedWineCondition BOXEDWINE_CONDITION;
#define BOXEDWINE_CONDITION_LOCK(cond)
#define BOXEDWINE_CONDITION_UNLOCK(cond)
#define BOXEDWINE_CONDITION_SIGNAL(cond) (cond).signal()
#define BOXEDWINE_CONDITION_SIGNAL_ALL(cond) (cond).signalAll()
#define BOXEDWINE_CONDITION_WAIT(cond) return (cond).wait()
#define BOXEDWINE_CONDITION_WAIT_TIMEOUT(cond, ms) return (cond).waitWithTimeout(ms)
#define BOXEDWINE_CONDITION_SET_PARENT(cond, parent) cond.setParentCondition(parent)

#endif

#endif