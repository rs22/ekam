// ekam -- http://code.google.com/p/ekam
// Copyright (c) 2010 Kenton Varda and contributors.  All rights reserved.
// Portions copyright Google, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of the ekam project nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "Driver.h"

#include <queue>
#include <tr1/memory>
#include <stdexcept>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "Debug.h"
#include "EventGroup.h"

namespace ekam {

class Driver::ActionDriver : public BuildContext, public EventGroup::ExceptionHandler {
public:
  ActionDriver(Driver* driver, OwnedPtr<Action>* actionToAdopt, File* tmploc,
               OwnedPtr<Dashboard::Task>* taskToAdopt);
  ~ActionDriver();

  void start();

  // implements BuildContext -------------------------------------------------------------
  File* findProvider(EntityId id, const std::string& title);
  File* findOptionalProvider(EntityId id);

  void provide(File* file, const std::vector<EntityId>& entities);
  void log(const std::string& text);

  void newOutput(const std::string& basename, OwnedPtr<File>* output);

  void success();
  void passed();
  void failed();

  // implements ExceptionHandler ---------------------------------------------------------
  void threwException(const std::exception& e);
  void threwUnknownException();

private:
  class StartCallback : public EventManager::Callback {
  public:
    StartCallback(ActionDriver* action) : actionDriver(action) {}
    ~StartCallback() {}

    // implements Callback ---------------------------------------------------------------
    void run() {
      actionDriver->action->start(&actionDriver->eventGroup, actionDriver);
    }

  private:
    ActionDriver* actionDriver;
  };

  class DoneCallback : public EventManager::Callback {
  public:
    DoneCallback(ActionDriver* action) : action(action) {}
    ~DoneCallback() {}

    // implements Callback ---------------------------------------------------------------
    void run() {
      Driver* driver = action->driver;
      action->returned();  // may delete action
      driver->startSomeActions();
    }

  private:
    ActionDriver* action;
  };


  Driver* driver;
  OwnedPtr<Action> action;
  OwnedPtr<File> tmpdir;
  OwnedPtr<Dashboard::Task> dashboardTask;

  enum {
    PENDING,
    RUNNING,
    SUCCEEDED,
    PASSED,
    FAILED
  } state;

  EventGroup eventGroup;

  typedef std::tr1::unordered_map<EntityId, std::string, EntityId::HashFunc> MissingDependencyMap;
  MissingDependencyMap missingDependencies;

  OwnedPtrMap<pid_t, ProcessExitCallback> processExitCallbacks;

  OwnedPtrVector<File> outputs;

  struct Provision {
    OwnedPtr<File> file;
    std::vector<EntityId> entities;
  };

  OwnedPtrVector<Provision> provisions;

  void ensureRunning();
  void queueDoneCallback();
  void returned();

  friend class Driver;
};

Driver::ActionDriver::ActionDriver(Driver* driver, OwnedPtr<Action>* actionToAdopt, File* tmploc,
                                   OwnedPtr<Dashboard::Task>* taskToAdopt)
    : driver(driver), state(PENDING), eventGroup(driver->eventManager, this) {
  action.adopt(actionToAdopt);
  tmploc->parent(&this->tmpdir);
  dashboardTask.adopt(taskToAdopt);
}
Driver::ActionDriver::~ActionDriver() {
  eventGroup.cancelAll();
}

void Driver::ActionDriver::start() {
  if (state != PENDING) {
    DEBUG_ERROR << "State must be PENDING here.";
  }
  state = RUNNING;
  dashboardTask->setState(Dashboard::RUNNING);

  OwnedPtr<EventManager::Callback> callback;
  callback.allocateSubclass<StartCallback>(this);
  eventGroup.runAsynchronously(&callback);
}

File* Driver::ActionDriver::findProvider(EntityId id, const std::string& title) {
  ensureRunning();
  File* result = findOptionalProvider(id);
  if (result == NULL) {
    missingDependencies[id] = title;
  }
  return result;
}

File* Driver::ActionDriver::findOptionalProvider(EntityId id) {
  ensureRunning();
  EntityMap::const_iterator iter = driver->entityMap.find(id);
  if (iter == driver->entityMap.end()) {
    return NULL;
  } else {
    return iter->second;
  }
}

void Driver::ActionDriver::provide(File* file, const std::vector<EntityId>& entities) {
  ensureRunning();

  OwnedPtr<Provision> provision;
  provision.allocate();
  file->clone(&provision->file);
  provision->entities = entities;

  provisions.adoptBack(&provision);
}

void Driver::ActionDriver::log(const std::string& text) {
  ensureRunning();
  dashboardTask->addOutput(text);
}

void Driver::ActionDriver::newOutput(const std::string& basename, OwnedPtr<File>* output) {
  ensureRunning();
  OwnedPtr<File> file;
  tmpdir->relative(basename, &file);
  file->clone(output);
  outputs.adoptBack(&file);
}

void Driver::ActionDriver::success() {
  ensureRunning();

  if (!missingDependencies.empty()) {
    throw std::runtime_error("Action reported success despite missing dependencies.");
  }

  state = SUCCEEDED;
  queueDoneCallback();
}

void Driver::ActionDriver::passed() {
  ensureRunning();

  if (!missingDependencies.empty()) {
    throw std::runtime_error("Action reported success despite missing dependencies.");
  }

  state = PASSED;
  queueDoneCallback();
}

void Driver::ActionDriver::failed() {
  ensureRunning();
  state = FAILED;
  queueDoneCallback();
}

void Driver::ActionDriver::ensureRunning() {
  if (state != RUNNING) {
    throw std::runtime_error("Action is not running.");
  }
}

void Driver::ActionDriver::queueDoneCallback() {
  OwnedPtr<EventManager::Callback> callback;
  callback.allocateSubclass<DoneCallback>(this);
  driver->eventManager->runAsynchronously(&callback);
}

void Driver::ActionDriver::threwException(const std::exception& e) {
  dashboardTask->addOutput(std::string("uncaught exception: ") + e.what() + "\n");
  if (state == RUNNING) {
    failed();
  }
}

void Driver::ActionDriver::threwUnknownException() {
  dashboardTask->addOutput("uncaught exception of unknown type\n");
  if (state == RUNNING) {
    failed();
  }
}

void Driver::ActionDriver::returned() {
  if (state == PENDING) {
    DEBUG_ERROR << "State should not be PENDING here.";
  }

  OwnedPtr<ActionDriver> self;
  for (int i = 0; i < driver->activeActions.size(); i++) {
    if (driver->activeActions.get(i) == this) {
      driver->activeActions.releaseAndShift(i, &self);
      break;
    }
  }

  if (!missingDependencies.empty()) {
    // Failed due to missing dependencies.

    // Reset state to PENDING.
    state = PENDING;
    eventGroup.cancelAll();
    provisions.clear();
    outputs.clear();
    dashboardTask->setState(Dashboard::BLOCKED);

    // Insert self into the blocked actions map.
    driver->blockedActionPtrs.adopt(this, &self);
    for (MissingDependencyMap::const_iterator iter = missingDependencies.begin();
         iter != missingDependencies.end(); ++iter) {
      driver->blockedActions.insert(std::make_pair(iter->first, this));
    }
  } else {
    if (state == SUCCEEDED || state == PASSED) {
      dashboardTask->setState(state == PASSED ? Dashboard::PASSED : Dashboard::SUCCESS);

      // Register providers.
      for (int i = 0; i < provisions.size(); i++) {
        File* file = provisions.get(i)->file.get();
        driver->filePtrs.adopt(file, &provisions.get(i)->file);

        for (std::vector<EntityId>::const_iterator iter = provisions.get(i)->entities.begin();
             iter != provisions.get(i)->entities.end(); ++iter) {
          driver->entityMap[*iter] = file;

          // Unblock blocked actions.
          std::pair<BlockedActionMap::const_iterator, BlockedActionMap::const_iterator>
              range = driver->blockedActions.equal_range(*iter);
          for (BlockedActionMap::const_iterator iter2 = range.first;
               iter2 != range.second; ++iter2) {
            iter2->second->missingDependencies.erase(*iter);
            if (iter2->second->missingDependencies.empty()) {
              // No more missing deps.  Promote to runnable.
              OwnedPtr<ActionDriver> action;
              if (driver->blockedActionPtrs.release(iter2->second, &action)) {
                driver->pendingActions.adoptBack(&action);
              } else {
                DEBUG_ERROR << "Action not in blockedActionPtrs?";
              }
            }
          }
          driver->blockedActions.erase(range.first, range.second);

          // Fire triggers.
          std::pair<TriggerMap::const_iterator, TriggerMap::const_iterator>
              triggerRange = driver->triggers.equal_range(*iter);
          for (TriggerMap::const_iterator iter2 = triggerRange.first;
               iter2 != triggerRange.second; ++iter2) {
            OwnedPtr<Action> triggeredAction;
            if (iter2->second->tryMakeAction(*iter, file, &triggeredAction)) {
              driver->queueNewAction(&triggeredAction, file, file);
            }
          }

        }
      }

      // Enqueue new actions based on output files.
      for (int i = 0; i < outputs.size(); i++) {
        driver->scanForActions(outputs.get(i), outputs.get(i));
      }
    } else {
      dashboardTask->setState(Dashboard::FAILED);
    }
  }
}

// =======================================================================================

Driver::Driver(EventManager* eventManager, Dashboard* dashboard, File* src, File* tmp,
               int maxConcurrentActions)
    : eventManager(eventManager), dashboard(dashboard), src(src), tmp(tmp),
      maxConcurrentActions(maxConcurrentActions) {}

Driver::~Driver() {
  // Error out all blocked tasks.
  for (OwnedPtrMap<ActionDriver*, ActionDriver>::Iterator iter(blockedActionPtrs); iter.next();) {
    iter.value()->dashboardTask->setState(Dashboard::FAILED);
  }
}

void Driver::addActionFactory(const std::string& name, ActionFactory* factory) {
  actionFactories[name] = factory;

  std::vector<EntityId> triggerEntities;
  factory->enumerateTriggerEntities(std::back_inserter(triggerEntities));
  for (unsigned int i = 0; i < triggerEntities.size(); i++) {
    triggers.insert(std::make_pair(triggerEntities[i], factory));
  }
}

void Driver::start() {
  scanForActions(src, tmp);
  startSomeActions();
}

void Driver::startSomeActions() {
  while (activeActions.size() < maxConcurrentActions && !pendingActions.empty()) {
    OwnedPtr<ActionDriver> actionDriver;
    pendingActions.releaseBack(&actionDriver);
    ActionDriver* ptr = actionDriver.get();
    activeActions.adoptBack(&actionDriver);
    try {
      ptr->start();
    } catch (const std::exception& e) {
      ptr->threwException(e);
    } catch (...) {
      ptr->threwUnknownException();
    }
  }
}

namespace {
struct SrcTmpPair {
  OwnedPtr<File> srcFile;
  OwnedPtr<File> tmpLocation;
};
}  // namespace

void Driver::scanForActions(File* src, File* tmp) {
  OwnedPtrVector<SrcTmpPair> fileQueue;

  {
    OwnedPtr<SrcTmpPair> root;
    root.allocate();
    src->clone(&root->srcFile);
    tmp->clone(&root->tmpLocation);
    fileQueue.adoptBack(&root);
  }

  while (!fileQueue.empty()) {
    OwnedPtr<SrcTmpPair> current;
    fileQueue.releaseBack(&current);

    if (current->srcFile->isDirectory()) {
      if (!current->tmpLocation->isDirectory()) {
        current->tmpLocation->createDirectory();
      }

      OwnedPtrVector<File> list;
      current->srcFile->list(list.appender());
      for (int i = 0; i < list.size(); i++) {
        OwnedPtr<SrcTmpPair> newPair;
        newPair.allocate();
        list.release(i, &newPair->srcFile);
        current->tmpLocation->relative(newPair->srcFile->basename(), &newPair->tmpLocation);
        fileQueue.adoptBack(&newPair);
      }
    } else {
      for (ActionFactoryMap::const_iterator iter = actionFactories.begin();
           iter != actionFactories.end(); ++iter) {
        ActionFactory* factory = iter->second;
        OwnedPtr<Action> action;
        if (factory->tryMakeAction(current->srcFile.get(), &action)) {
          queueNewAction(&action, current->srcFile.get(), current->tmpLocation.get());
        }
      }
    }
  }
}

void Driver::queueNewAction(OwnedPtr<Action>* actionToAdopt, File* file, File* tmpLocation) {
  OwnedPtr<Dashboard::Task> task;
  dashboard->beginTask((*actionToAdopt)->getVerb(), file->displayName(), &task);

  OwnedPtr<ActionDriver> actionDriver;
  actionDriver.allocate(this, actionToAdopt, tmpLocation, &task);

  pendingActions.adoptBack(&actionDriver);
}

}  // namespace ekam