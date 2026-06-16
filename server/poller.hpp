#ifndef POLLER_HPP
#define POLLER_HPP

#include <vector>

// An I/O readiness event returned by Poller::wait().
struct PollEvent {
    int  fd;
    bool readable;
    bool writable;
};

// Abstract I/O multiplexer. Concrete implementations: KqueuePoller (macOS),
// EpollPoller (Linux).  Select-based fallback is not provided — use select_mode.cpp
// directly if neither kqueue nor epoll is available.
class Poller {
public:
    virtual ~Poller() = default;

    virtual void add(int fd, bool want_read, bool want_write) = 0;
    virtual void modify(int fd, bool want_read, bool want_write) = 0;
    virtual void remove(int fd) = 0;

    // Block until at least one fd is ready or timeout_ms elapses.
    // Returns the set of ready fds (may be empty on timeout).
    virtual std::vector<PollEvent> wait(int timeout_ms) = 0;
};

#ifdef __APPLE__
#  include <sys/event.h>
#  include <unistd.h>

class KqueuePoller : public Poller {
    int kq_;

public:
    KqueuePoller() : kq_(kqueue()) {}
    ~KqueuePoller() override { if (kq_ >= 0) close(kq_); }

    void add(int fd, bool want_read, bool want_write) override {
        change(fd, want_read, want_write, EV_ADD | EV_ENABLE);
    }

    void modify(int fd, bool want_read, bool want_write) override {
        // Disable both filters first, then re-enable the ones we want.
        struct kevent kev[2];
        EV_SET(&kev[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        // Ignore errors — the filter might not be registered yet.
        kevent(kq_, kev, 2, nullptr, 0, nullptr);
        change(fd, want_read, want_write, EV_ADD | EV_ENABLE);
    }

    void remove(int fd) override {
        struct kevent kev[2];
        EV_SET(&kev[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(kq_, kev, 2, nullptr, 0, nullptr);
    }

    std::vector<PollEvent> wait(int timeout_ms) override {
        struct kevent events[64];
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;

        int n = kevent(kq_, nullptr, 0, events, 64, &ts);
        std::vector<PollEvent> out;
        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);
            bool r = (events[i].filter == EVFILT_READ);
            bool w = (events[i].filter == EVFILT_WRITE);
            // Merge read/write into a single PollEvent per fd.
            bool found = false;
            for (auto& ev : out) {
                if (ev.fd == fd) { ev.readable |= r; ev.writable |= w; found = true; break; }
            }
            if (!found) out.push_back({fd, r, w});
        }
        return out;
    }

private:
    void change(int fd, bool want_read, bool want_write, int flags) {
        struct kevent kev[2];
        int n = 0;
        if (want_read)  EV_SET(&kev[n++], fd, EVFILT_READ,  flags, 0, 0, nullptr);
        if (want_write) EV_SET(&kev[n++], fd, EVFILT_WRITE, flags, 0, 0, nullptr);
        if (n > 0) kevent(kq_, kev, n, nullptr, 0, nullptr);
    }
};

using DefaultPoller = KqueuePoller;

#elif defined(__linux__)
#  include <sys/epoll.h>
#  include <unistd.h>

class EpollPoller : public Poller {
    int epfd_;

    static uint32_t events_mask(bool want_read, bool want_write) {
        uint32_t mask = EPOLLET;  // edge-triggered
        if (want_read)  mask |= EPOLLIN;
        if (want_write) mask |= EPOLLOUT;
        return mask;
    }

public:
    EpollPoller() : epfd_(epoll_create1(EPOLL_CLOEXEC)) {}
    ~EpollPoller() override { if (epfd_ >= 0) close(epfd_); }

    void add(int fd, bool want_read, bool want_write) override {
        struct epoll_event ev{};
        ev.events = events_mask(want_read, want_write);
        ev.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }

    void modify(int fd, bool want_read, bool want_write) override {
        struct epoll_event ev{};
        ev.events = events_mask(want_read, want_write);
        ev.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }

    void remove(int fd) override {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    std::vector<PollEvent> wait(int timeout_ms) override {
        struct epoll_event events[64];
        int n = epoll_wait(epfd_, events, 64, timeout_ms);
        std::vector<PollEvent> out;
        for (int i = 0; i < n; ++i) {
            bool r = (events[i].events & EPOLLIN)  != 0;
            bool w = (events[i].events & EPOLLOUT) != 0;
            out.push_back({events[i].data.fd, r, w});
        }
        return out;
    }
};

using DefaultPoller = EpollPoller;

#endif  // __APPLE__ / __linux__

#endif  // POLLER_HPP
