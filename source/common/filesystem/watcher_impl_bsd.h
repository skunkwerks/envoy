#pragma once

#include "common/common/linked_object.h"

#include "envoy/event/dispatcher.h"
#include "envoy/filesystem/filesystem.h"

namespace Filesystem {

/**
 * Implementation of Watcher that uses kqueue. kqueue API is way more elegant than inotify
 * and allows multiple files monitoring.
 */
class WatcherImpl : public Watcher {
public:
  WatcherImpl(Event::Dispatcher& dispatcher);
  ~WatcherImpl();

  // Filesystem::Watcher
  void addWatch(const std::string& path, uint32_t events, OnChangedCb cb) override;

private:
  struct FileWatch : LinkedObject<FileWatch> {
    int fd_;
    uint32_t events_;
    std::string file_;
    OnChangedCb callback_;
  };

  typedef std::unique_ptr<FileWatch> FileWatchPtr;

  void onKqueueEvent();

  int queue_;
  std::list<FileWatchPtr> watches_;
  Event::FileEventPtr kqueue_event_;
};

} // Filesystem
