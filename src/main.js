let binding;
try {
  binding = require('../build/Debug/pathwatcher.node');
} catch (err) {
  binding = require('../build/Release/pathwatcher.node');
}
const { Emitter, Disposable, CompositeDisposable } = require('event-kit');
const fs = require('fs');
const { NativeWatcherRegistry } = require('./native-watcher-registry');
const path = require('path');

let initialized = false;

// Ensures a path that refers to a directory ends with a path separator.
function sep (dirPath) {
  if (dirPath.endsWith(path.sep)) return dirPath;
  return `${dirPath}${path.sep}`;
}

function isDirectory (somePath) {
  let stats = fs.statSync(somePath);
  return stats.isDirectory();
}

// function wait (ms) {
//   return new Promise(r => setTimeout(r, ms));
// }

// function normalizePath(rawPath) {
//   if (rawPath.endsWith(path.sep)) return rawPath;
//   return rawPath + path.sep;
// }

// function pathsAreEqual(pathA, pathB) {
//   return normalizePath(pathA) == normalizePath(pathB);
// }

// function equalsOrDescendsFromPath(filePath, possibleParentPath) {
//   if (pathsAreEqual(filePath, possibleParentPath)) return true;
//   filePath = normalizePath(filePath);
//   possibleParentPath = normalizePath(possibleParentPath);
//   return filePath?.startsWith(possibleParentPath);
// }

let NativeWatcherId = 1;

class NativeWatcher {
  // Holds _active_ `NativeWatcher` instances. A `NativeWatcher` is active if
  // at least one consumer has subscribed to it via `onDidChange`; it becomes
  // inactive whenever its last consumer unsubscribes.
  static INSTANCES = new Map();

  // Given a path, returns whatever existing active `NativeWatcher` is already
  // watching that path, or creates one if it doesn’t yet exist.
  static findOrCreate (normalizedPath) {
    for (let instance of this.INSTANCES.values()) {
      if (instance.normalizedPath === normalizedPath) {
        return instance;
      }
    }
    return new NativeWatcher(normalizedPath);
  }

  // Returns the number of active `NativeWatcher` instances.
  static get instanceCount() {
    return this.INSTANCES.size;
  }

  constructor(normalizedPath) {
    this.id = NativeWatcherId++;
    this.normalizedPath = normalizedPath;
    this.emitter = new Emitter();
    this.subs = new CompositeDisposable();

    this.running = false;
  }

  get path () {
    return this.normalizedPath;
  }

  start () {
    if (this.running) return;
    this.handle = binding.watch(this.normalizedPath);
    NativeWatcher.INSTANCES.set(this.handle, this);
    this.running = true;
    this.emitter.emit('did-start');
  }

  onDidStart (callback) {
    return this.emitter.on('did-start', callback);
  }

  onDidChange (callback) {
    this.start();

    let sub = this.emitter.on('did-change', callback);
    return new Disposable(() => {
      sub.dispose();
      if (this.emitter.listenerCountForEventName('did-change') === 0) {
        this.stop();
      }
    });
  }

  onShouldDetach (callback) {
    return this.emitter.on('should-detach', callback);
  }

  onWillStop (callback) {
    return this.emitter.on('will-stop', callback);
  }

  onDidStop () {
    return this.emitter.on('did-stop', callback);
  }

  onDidError (callback) {
    return this.emitter.on('did-error', callback);
  }

  reattachTo (replacement, watchedPath, options) {
    if (replacement === this) return;
    this.emitter.emit('should-detach', { replacement, watchedPath, options });
  }

  stop (shutdown = false) {
    // console.log('Stopping NativeListener', this.handle, this.running);
    if (this.running) {
      this.emitter.emit('will-stop', shutdown);
      binding.unwatch(this.handle);
      this.running = false;
      this.emitter.emit('did-stop', shutdown);
    }

    NativeWatcher.INSTANCES.delete(this.handle);

    // console.log('Remaining instances:', NativeWatcher.INSTANCES.size, [...NativeWatcher.INSTANCES.keys()]);
  }

  dispose () {
    this.emitter.dispose();
  }

  onEvent (event) {
    // console.log('NativeWatcher#onEvent!', event);
    // console.log('onEvent!', event);
    this.emitter.emit('did-change', event);
  }

  onError (err) {
    this.emitter.emit('did-error', err);
  }
}

class WatcherError extends Error {
  constructor(message, code) {
    super(message);
    this.name = 'WatcherError';
    this.code = code;
  }
}

let PathWatcherId = 10;

class PathWatcher {
  constructor (registry, watchedPath) {
    this.id = PathWatcherId++;
    this.watchedPath = watchedPath;
    this.registry = registry;

    this.normalizePath = null;
    this.native = null;
    this.changeCallbacks = new Map();

    this.emitter = new Emitter();
    this.subs = new CompositeDisposable();

    if (!fs.existsSync(watchedPath)) {
      throw new WatcherError('Unable to watch path', 'ENOENT');
    }

    try {
      this.normalizedPath = fs.realpathSync(watchedPath);
    } catch (err) {
      this.normalizedPath = watchedPath;
    }

    let stats = fs.statSync(this.normalizedPath);
    this.isWatchingParent = !stats.isDirectory();

    this.originalNormalizedPath = this.normalizedPath;
    if (!stats.isDirectory()) {
      this.normalizedPath = path.dirname(this.normalizedPath);
    }

    this.attachedPromise = new Promise(resolve => {
      this.resolveAttachedPromise = resolve;
    });

    this.startPromise = new Promise((resolve, reject) => {
      this.resolveStartPromise = resolve;
      this.rejectStartPromise = reject;
    });

    this.active = true;
  }

  getNormalizedPath() {
    return this.normalizedPath;
  }

  getNormalizedPathPromise () {
    return Promise.resolve(this.normalizedPath);
  }

  onDidChange (callback) {
    if (this.native) {
      let sub = this.native.onDidChange(event => {
        this.onNativeEvent(event, callback);
      });
      this.changeCallbacks.set(callback, sub);
      this.native.start();
    } else {
      if (this.normalizedPath) {
        this.registry.attach(this);
        this.onDidChange(callback);
      } else {
        this.registry.attachAsync(this).then(() => {
          this.onDidChange(callback);
        })
      }
    }

    return new Disposable(() => {
      let sub = this.changeCallbacks.get(callback);
      this.changeCallbacks.delete(callback);
      sub.dispose();
    });
  }

  onDidError (callback) {
    return this.emitter.on('did-error', callback);
  }

  attachToNative (native) {
    this.subs.dispose();
    this.subs = new CompositeDisposable();
    this.native = native;
    if (native.running) {
      this.resolveStartPromise();
    } else {
      this.subs.add(
        native.onDidStart(() => this.resolveStartPromise())
      );
    }

    // console.log('PathWatcher instance with path', this.originalNormalizedPath, 'is attaching to native:', native);

    // Transfer any native event subscriptions to the new NativeWatcher.
    for (let [callback, formerSub] of this.changeCallbacks) {
      let newSub = native.onDidChange(event => {
        return this.onNativeEvent(event, callback);
      });
      this.changeCallbacks.set(callback, newSub);
      formerSub.dispose();
    }

    this.subs.add(
      native.onDidError(err => this.emitter.emit('did-error', err))
    );

    this.subs.add(
      native.onShouldDetach(({ replacement, watchedPath }) => {
        if (isClosingAllWatchers) return;
        // console.warn('Should PathWatcher with ID', this.id, 'attach to:', replacement, 'when it already has native:', this.native, this.native === replacement);
        if (
          this.active &&
          this.native === native &&
          replacement !== native &&
          this.normalizedPath?.startsWith(watchedPath)
        ) {
          // console.log('PathWatcher with ID:', this.id, 'reattaching to:', replacement, ';\n  the PathWatcher is meant to watch the path:', this.originalNormalizedPath);
          // console.warn('The current watcher count is', getNativeWatcherCount());
          this.attachToNative(replacement, replacement.normalizedPath);
        }
      })
    );

    this.subs.add(
      native.onWillStop(() => {
        if (this.native !== native) return;
        this.subs.dispose();
        this.native = null;
      })
    );

    this.resolveAttachedPromise();
  }

  rename (newName) {
    this.close();
    try {
      this.normalizedPath = fs.realpathSync(newName);
    } catch (err) {
      this.normalizedPath = newName;
    }

    let stats = fs.statSync(this.normalizedPath);
    this.isWatchingParent = !stats.isDirectory();

    this.originalNormalizedPath = this.normalizedPath;
    if (!stats.isDirectory()) {
      this.normalizedPath = path.dirname(this.normalizedPath);
    }

    this.registry.attach(this);
    this.active = true;
  }

  onNativeEvent (event, callback) {
    // console.debug(
    //   'PathWatcher::onNativeEvent',
    //   event,
    //   'for watcher of path:',
    //   this.originalNormalizedPath
    // );

    let isWatchedPath = (eventPath) => {
      return eventPath?.startsWith(sep(this.normalizedPath));
    }

    // Does `event.path` match the exact path our `PathWatcher` cares about?
    let eventPathIsEqual = this.originalNormalizedPath === event.path;
    // Does `event.oldPath` match the exact path our `PathWatcher` cares about?
    let eventOldPathIsEqual = this.originalNormalizedPath === event.oldPath;

    // Is `event.path` somewhere within the folder that this `PathWatcher` is
    // monitoring?
    let newWatched = isWatchedPath(event.path);
    // Is `event.oldPath` somewhere within the folder that this `PathWatcher`
    // is monitoring?
    let oldWatched = isWatchedPath(event.oldPath);

    let newEvent = { ...event };

    if (!newWatched && !oldWatched) {
      return;
    }

    switch (newEvent.action) {
      case 'rename':
      case 'delete':
      case 'create':
        // These events need no alteration.
        break;
      case 'child-create':
        if (!this.isWatchingParent) {
          if (eventPathIsEqual) {
            // We're watching a directory and this is a create event for the
            // directory itself. This should be fixed in the bindings, but for
            // now we can switch the event type in the JS.
            newEvent.action = 'create';
          } else {
            newEvent.action = 'change';
            newEvent.path = '';
          }
          break;
        } else if (eventPathIsEqual) {
          newEvent.action = 'create';
        }
        break;
      case 'child-delete':
        if (!this.isWatchingParent) {
          newEvent.action = 'change';
          newEvent.path = '';
        } else if (eventPathIsEqual) {
          newEvent.action = 'delete';
        }
        break;
      case 'child-rename':
        // TODO: Laziness in the native addon means that even events that
        // happen to the directory itself are reported as `child-rename`
        // instead of `rename`. We can fix this in the JS for now, but it
        // should eventually be fixed in the C++.

        // First, weed out the cases that can't possibly affect us.
        let pathIsInvolved = eventPathIsEqual || eventOldPathIsEqual;

        // The only cases for which we should return early are the ones where
        // (a) we're watching a file, and (b) this event doesn't involve it
        // in any way.
        if (this.isWatchingParent && !pathIsInvolved) {
          return;
        }

        if (!this.isWatchingParent && !pathIsInvolved) {
          // We're watching a directory and these events involve something
          // inside of the directory.
          if (
            path.dirname(event.path) === this.normalizedPath ||
              path.dirname(event.oldPath) === this.normalizedPath
          ) {
            // This is a direct child of the directory, so we'll fire an
            // event.
            newEvent.action = 'change';
            newEvent.path = '';
          } else {
            // Changes in ancestors or descendants do not concern us, so
            // we'll return early.
            //
            // TODO: Changes in ancestors might, actually; they might need to
            // be treated as folder deletions/creations.
            return;
          }
        } else {
          // We're left with cases where
          //
          // * We're watching a directory and that directory is named by the
          //   event, or
          // * We're watching a file (via a directory watch) and that file is
          //   named by the event.
          //
          // Those cases are handled identically.

          if (newWatched && this.originalNormalizedPath !== event.path) {
            // The file/directory we care about has moved to a new destination
            // and that destination is visible to this watcher. That means we
            // can simply update the path we care about and keep path-watching.
            this.moveToPath(event.path);
          }

          if (oldWatched && newWatched) {
            // We can keep tabs on both file paths from here, so this will
            // be treated as a rename.
            newEvent.action = 'rename';
          } else if (oldWatched && !newWatched) {
            // We're moving the file to a place we're not observing, so
            // we'll treat it as a deletion.
            newEvent.action = 'delete';
          } else if (!oldWatched && newWatched) {
            // The file came from someplace we're not watching, so it might
            // as well be a file creation.
            newEvent.action = 'create';
          }
        }
        break;
      case 'child-change':
        if (!this.isWatchingParent) {
          // We are watching a directory.
          if (eventPathIsEqual) {
            // This makes no sense; we won't fire a `child-change` on a
            // directory. Ignore it.
            return;
          } else {
            newEvent.action = 'change';
            newEvent.path = '';
          }
        } else {
          if (eventPathIsEqual) {
            newEvent.action = 'change';
            newEvent.path = '';
          } else {
            return;
          }
        }
        break;
    } // end switch

    if (eventPathIsEqual && newEvent.action === 'create') {
      // This file or directory already existed; we checked. Any `create`
      // event for it is spurious.
      return;
    }

    if (eventPathIsEqual) {
      // Specs require that a `delete` action carry a path of `null`; other
      // actions should carry an empty path. (Weird decisions, but we can
      // live with them.)
      newEvent.path = newEvent.action === 'delete' ? null : '';
    }
    // console.debug(
    //   'FINAL EVENT ACTION:',
    //   newEvent.action,
    //   'PATH',
    //   newEvent.path
    // );
    callback(newEvent.action, newEvent.path);
  }

  moveToPath (newPath) {
    this.isWatchingParent = !isDirectory(newPath);
    if (this.isWatchingParent) {
      // Watching a directory just because we care about a specific file inside
      // it.
      this.originalNormalizedPath = newPath;
      this.normalizedPath = path.dirname(newPath);
    } else {
      // Actually watching a directory.
      this.originalNormalizedPath = newPath;
      this.normalizedPath = newPath;
    }
  }

  dispose () {
    this.disposing = true;
    for (let sub of this.changeCallbacks.values()) {
      sub.dispose();
    }

    this.emitter.dispose();
    this.subs.dispose();
  }

  close () {
    this.active = false;
    this.dispose();
  }
}

const REGISTRY = new NativeWatcherRegistry((normalizedPath) => {
  if (!initialized) {
    binding.setCallback(DEFAULT_CALLBACK);
    initialized = true;
  }
  // It's important that this function be able to return an existing instance
  // of `NativeWatcher` when present. Otherwise, the registry will try to
  // create a new instance at the same path, and the native bindings won't
  // allow that to happen.
  //
  // It's also important because the registry might respond to a sibling
  // `PathWatcher`’s removal by trying to reattach us — even though our
  // `NativeWatcher` still works just fine. The way around that is to make sure
  // that this function will return the same watcher we're already using
  // instead of creating a new one.
  return NativeWatcher.findOrCreate(normalizedPath);
});

class WatcherEvent {
  constructor(event, filePath, oldFilePath) {
    this.action = event;
    this.path = filePath;
    this.oldPath = oldFilePath;
  }
}

function DEFAULT_CALLBACK(action, handle, filePath, oldFilePath) {
  if (!NativeWatcher.INSTANCES.has(handle)) {
    // Might be a stray callback from a `NativeWatcher` that has already
    // stopped.
    return;
  }

  let watcher = NativeWatcher.INSTANCES.get(handle);
  let event = new WatcherEvent(action, filePath, oldFilePath);
  watcher.onEvent(event);
}

function watch (pathToWatch, callback) {
  if (!initialized) {
    binding.setCallback(DEFAULT_CALLBACK);
    initialized = true;
  }
  let watcher = new PathWatcher(REGISTRY, path.resolve(pathToWatch));
  watcher.onDidChange(callback);
  return watcher;
}

let isClosingAllWatchers = false;
function closeAllWatchers () {
  isClosingAllWatchers = true;
  for (let watcher of NativeWatcher.INSTANCES.values()) {
    watcher.stop(true);
  }
  NativeWatcher.INSTANCES.clear();
  REGISTRY.reset();
  isClosingAllWatchers = false;
}

function getWatchedPaths () {
  let watchers = Array.from(NativeWatcher.INSTANCES.values());
  let result = watchers.map(w => w.normalizedPath);
  return result
}

function getNativeWatcherCount() {
  return NativeWatcher.INSTANCES.size;
}

const File = require('./file');
const Directory = require('./directory');

module.exports = {
  watch,
  closeAllWatchers,
  getWatchedPaths,
  getNativeWatcherCount,
  File,
  Directory
};
