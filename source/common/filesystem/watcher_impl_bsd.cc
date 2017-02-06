#include "watcher_impl_bsd.h"

#include "envoy/common/exception.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/file_event.h"

#include "common/common/assert.h"
#include "common/common/utility.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>

namespace Filesystem {

WatcherImpl::WatcherImpl(Event::Dispatcher& dispatcher)
    : queue_(kqueue()),
      kqueue_event_(dispatcher.createFileEvent(queue_, [this](uint32_t events) -> void {
        if (events & Event::FileReadyType::Read) {
          onKqueueEvent();
        }
      })) {}

WatcherImpl::~WatcherImpl() {
  close(queue_);

  for (FileWatchPtr &file : watches_) {
    close(file->fd_);
    file->removeFromList(watches_);
  }
}

void WatcherImpl::addWatch(const std::string& path, uint32_t events, Watcher::OnChangedCb cb) {
  int watch_fd = open(path.c_str(), O_RDONLY);
  if (watch_fd == -1) {
   throw EnvoyException(fmt::format("invalid watch path {}", path));
  }

  FileWatchPtr watch(new FileWatch());
  watch->fd_ = watch_fd;
  watch->file_ = path;
  watch->callback_ = cb;

  struct kevent event;
  EV_SET(&event, watch_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, events, 0, watch.get());

  if (kevent(queue_, &event, 1, NULL, 0, NULL) == -1) {
    throw EnvoyException(
        fmt::format("unable to add filesystem watch for file {}: {}", path, strerror(errno)));
  }

  if (event.flags & EV_ERROR) {
    throw EnvoyException(
        fmt::format("unable to add filesystem watch for file {}: {}", path, strerror(event.data)));
  }

  //log_debug("added watch for file: '{}' fd: {}", path, watch_fd);
  watch->moveIntoList(std::move(watch), watches_);
}

void WatcherImpl::onKqueueEvent() {
  struct kevent event = {};

  int nevents = kevent(queue_, NULL, 0, &event, 1, NULL);
  if (nevents == -1 || event.udata == nullptr) {
    return;
  }

  auto file = static_cast<FileWatch*>(event.udata);
  uint32_t events = 0;
  if (event.fflags & NOTE_RENAME) {
    events |= Events::MovedTo;
  }

  if (file->events_ & event.fflags) {
    //log_debug("matched callback: file: {}", file->file_);
    file->callback_(events);
  }

  //log_debug("notification: fd: {} flags: {:x} file: {}", file->fd_, event.fflags, file->file_);

  if (event.fflags & NOTE_DELETE) {
    close(file->fd_);
  }
}

} // Filesystem
