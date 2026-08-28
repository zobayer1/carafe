#pragma once

#include <csignal>
#include <pthread.h>

#include <gtest/gtest.h>
#include <sys/time.h>

namespace carafe::test {

// A handler can report back only through a global of this type.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline volatile std::sig_atomic_t alarms_delivered = 0;

extern "C" inline void count_alarm(int /*signal*/) {
    alarms_delivered = alarms_delivered + 1;
}

// A thread created while SIGALRM is blocked inherits that, so only this one is ever interrupted.
inline void mask_alarm(int how) {
    sigset_t alarm_only;
    sigemptyset(&alarm_only);
    sigaddset(&alarm_only, SIGALRM);
    EXPECT_EQ(pthread_sigmask(how, &alarm_only, nullptr), 0);
}

// One-shot SIGALRM, handler and timer restored on the way out. No SA_RESTART, so the kernel hands the interrupted call
// back as EINTR instead of resuming it.
class AlarmIn {
public:
    explicit AlarmIn(suseconds_t micros) {
        struct sigaction handler{};
        handler.sa_handler = count_alarm;
        handler.sa_flags = 0;
        sigemptyset(&handler.sa_mask);
        EXPECT_EQ(::sigaction(SIGALRM, &handler, &previous_), 0);

        alarms_delivered = 0;
        itimerval timer{};
        timer.it_value.tv_usec = micros;
        EXPECT_EQ(::setitimer(ITIMER_REAL, &timer, nullptr), 0);
    }

    ~AlarmIn() {
        const itimerval disarm{};
        ::setitimer(ITIMER_REAL, &disarm, nullptr);
        ::sigaction(SIGALRM, &previous_, nullptr);
    }

    AlarmIn(const AlarmIn&) = delete;
    AlarmIn& operator=(const AlarmIn&) = delete;
    AlarmIn(AlarmIn&&) = delete;
    AlarmIn& operator=(AlarmIn&&) = delete;

private:
    struct sigaction previous_{};
};

}  // namespace carafe::test
