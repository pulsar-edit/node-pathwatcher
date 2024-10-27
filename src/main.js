let binding;
// console.log('ENV:', process.NODE_ENV);
// if (process.NODE_ENV === 'DEV') {
  try {
    binding = require('../build/Debug/pathwatcher.node');
  } catch (err) {
    binding = require('../build/Release/pathwatcher.node');
  }
// } else {
//   binding = require('../build/Release/pathwatcher.node');
// }

const fs = require('fs');
const path = require('path');
const { Emitter, Disposable, CompositeDisposable } = require('event-kit');
const { NativeWatcherRegistry } = require('./native-watcher-registry');

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

class WatcherError extends Error {
  constructor(message, code) {
    super(message);
    this.name = 'WatcherError';
    this.code = code;
  }
}

function getRealFilePath (filePath) {
  try {
    return fs.realpathSync(filePath) ?? filePath;
  } catch (_err) {
    return filePath;
  }
}

function isDirectory (filePath) {
  let stats = fs.statSync(filePath);
  return stats.isDirectory();
}

let NativeWatcherId = 1;

// A “native” watcher instance that is responsible for managing watchers on
// various directories.
//
// Each `NativeWatcher` can supply file-watcher events for one or more
// `PathWatcher` instances; those `PathWatcher`s may care about all events in
// the watched directory, only events that affect a particular child of the
// directory, or even events that affect a descendant folder or file.
//
// We employ some common-sense measures to consolidate watchers when they want
// to watch similar paths. A `NativeWatcher` will start when a `PathWatcher`
// first subscribes to it; it will stop when no more `PathWatcher`s are
// subscribed.
class NativeWatcher {
  // Holds _active_ `NativeWatcher` instances. A `NativeWatcher` is active if
  // at least one consumer has subscribed to it via `onDidChange`; it becomes
  // inactive whenever its last consumer unsubscribes.
  static INSTANCES = new Map();

  // Given a path, returns whatever existing active `NativeWatcher` is already
  // watching that path, or creates one if it doesn’t yet exist.
  static findOrCreate (normalizedPath, options) {
    for (let instance of this.INSTANCES.values()) {
      if (instance.normalizedPath === normalizedPath) {
        return instance;
      }
    }
    return new NativeWatcher(normalizedPath, options);
  }

  // Returns the number of active `NativeWatcher` instances.
  static get instanceCount() {
    return this.INSTANCES.size;
  }

  constructor(normalizedPath, { recursive = false } = {}) {
    this.id = NativeWatcherId++;
    this.normalizedPath = normalizedPath;
    this.emitter = new Emitter();
    this.subs = new CompositeDisposable();
    this.recursive = recursive;
    this.running = false;
  }

  get path () {
    return this.normalizedPath;
  }

  get listenerCount () {
    return this.emitter.listenerCountForEventName('did-change');
  }

  start () {
    if (this.running) return;
    if (!fs.existsSync(this.normalizedPath)) {
      // We can't start a watcher on a path that doesn't exist.
      return;
    }
    this.handle = binding.watch(this.normalizedPath, this.recursive);
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

  reattachListenersTo (replacement, watchedPath, options) {
    if (replacement === this) return;
    this.emitter.emit('should-detach', { replacement, watchedPath, options });
  }

  stop (shutdown = false) {
    if (this.running) {
      this.emitter.emit('will-stop', shutdown);
      binding.unwatch(this.handle);
      this.running = false;
      this.emitter.emit('did-stop', shutdown);
    }

    NativeWatcher.INSTANCES.delete(this.handle);
  }

  dispose () {
    this.emitter.dispose();
  }

  onEvent (event) {
    this.emitter.emit('did-change', event);
  }

  onError (err) {
    this.emitter.emit('did-error', err);
  }
}

let PathWatcherId = 10;

// A class responsible for watching a particular directory or file on the
// filesystem.
//
// A `PathWatcher` is backed by a `NativeWatcher` that is guaranteed to notify
// it about the events it cares about, and often other events it _doesn’t_ care
// about; it’s the `PathWatcher`’s job to filter this stream and ignore the
// irrelevant events.
//
// For instance, a `NativeWatcher` can only watch a directory, but a
// `PathWatcher` can watch a specific file in the directory. In that case, it’s
// up to the `PathWatcher` to ignore any events that do not pertain to that
// file.
//
// A `PathWatcher` might be asked to switch from one `NativeWatcher` to another
// after creation. As more `PathWatcher`s are created, and more
// `NativeWatchers` are created to support them, opportunities for
// consolidation and reuse might present themselves. For instance, sibling
// watchers with two different `NativeWatcher`s might trigger the creation of a
// new `NativeWatcher` pointing to their shared parent directory.
//
// In these scenarios, the new `NativeWatcher` is created before the old one is
// destroyed; the goal is that the switch from one `NativeWatcher` to another
// is atomic and results in no missed filesystem events. The old watcher will
// be disposed of once no `PathWatcher`s are listening to it anymore.
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

    // NOTE: Right now we have the constraint that we can't watch a path that
    // doesn't yet exist. This is a long-standing behavior of `pathwatcher` and
    // would have to be changed carefully if it were changed at all.
    if (!fs.existsSync(watchedPath)) {
      throw new WatcherError('Unable to watch path', 'ENOENT');
    }

    // Because `pathwatcher` is historically a very synchronous API, you'll see
    // lots of synchronous `fs` calls in this code. This is done for
    // backward-compatibility. It's a medium-term goal for us to reduce our
    // dependence on this library and move its consumers to a file-watcher
    // contract with an asynchronous API.
    this.normalizedPath = getRealFilePath(watchedPath);
    // try {
    //   this.normalizedPath = fs.realpathSync(watchedPath) ?? watchedPath;
    // } catch (err) {
    //   this.normalizedPath = watchedPath;
    // }

    // We must watch a directory. If this is a file, we must watch its parent.
    // If this is a directory, we can watch it directly. This flag helps us
    // keep track of it.
    this.isWatchingParent = !isDirectory(this.normalizedPath);

    // `originalNormalizedPath` will always contain the resolved (real path on
    // disk) file path that we care about.
    this.originalNormalizedPath = this.normalizedPath;
    if (this.isWatchingParent) {
      // `normalizedPath` will always contain the path to the directory we mean
      // to watch.
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
    // We don't try to create a native watcher until something subscribes to
    // our `did-change` events.
    if (this.native) {
      let sub = this.native.onDidChange(event => {
        this.onNativeEvent(event, callback);
      });
      this.changeCallbacks.set(callback, sub);
      this.native.start();
    } else {
      // We don't have a native watcher yet, so we’ll ask the registry to
      // assign one to us. This could be a brand-new instance or one that was
      // already watching one of our ancestor folders.
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

  // Attach a `NativeWatcher` to this `PathWatcher`.
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
        // When we close all native watchers, we set this flag; we don't want
        // it to trigger a flurry of new watcher creation.
        if (isClosingAllWatchers) return;

        // The `NativeWatcher` is telling us that it may shut down; it’s
        // offering a replacement `NativeWatcher`. We are in charge of whether
        // we jump ship, though:
        //
        // * Are we even watching a path anymore? Maybe this was triggered
        //   because we called our own `close` method.
        // * Is it trying to get us to “switch” to the `NativeWatcher` we’re
        //   already using?
        // * Sanity check: is this even a `NativeWatcher` we can use?
        //
        // Keep in mind that a `NativeWatcher` isn’t doomed to be stopped
        // unless it has signaled a `will-stop` event. If that hasn’t happened,
        // then `should-detach` is merely offering a suggestion.
        if (
          this.active &&
          this.native === native &&
          replacement !== native &&
          this.normalizedPath?.startsWith(watchedPath)
        ) {
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
    this.registry.detach(this);
  }
}

function getDefaultRegistryOptionsForPlatform (platform) {
  switch (platform) {
    case 'linux':
      // On Linux, `efsw`’s strategy for a recursive watcher is to recursively
      // watch each folder underneath the tree and call `inotify_add_watch` for
      // each one. This is a far more “wasteful” strategy than the one we’d be
      // trying to avoid by reusing native watchers in the first place!
      //
      // Hence, on Linux, all these options are disabled. More non-recursive
      // `NativeWatcher`s are better than fewer recursive `NativeWatcher`s.
      return {
        reuseAncestorWatchers: false,
        relocateDescendantWatchers: false,
        relocateAncestorWatchers: false,
        mergeWatchersWithCommonAncestors: false
      };
    case 'win32':
      return {
        reuseAncestorWatchers: false,
        relocateDescendantWatchers: false,
        relocateAncestorWatchers: false,
        mergeWatchersWithCommonAncestors: false
      };
    case 'darwin':
      // On macOS, the `FSEvents` API is a godsend in a number of ways. But
      // there’s a big caveat: `fseventsd` allows only a fixed number of
      // “clients” (currently 1024) _system-wide_ and attempts to add more will
      // fail.
      //
      // Each `NativeWatcher` counts as a single “client” for these purposes,
      // making it extremely plausible for us to use hundreds of these clients
      // on our own. Each `FSEvents` stream can watch any number of arbitrary
      // paths on the filesystem, but `efsw` doesn’t approach it that way, and
      // any change to that list of paths would involve stopping one watcher
      // and creating another. `efsw` currently doesn’t do this, though it’d be
      // nice if they did in the future.
      //
      // That aside, the biggest thing we’ve got going for us is that
      // `FSEvents` streams are natively recursive. That makes it easier to
      // reuse `NativeWatcher`s; a watcher on an ancestor can be reused on a
      // descendant, for instance. And `fseventsd`’s hard upper limit on
      // watchers makes it worth the tradeoff here.
      return {
        reuseAncestorWatchers: true,
        relocateDescendantWatchers: false,
        relocateAncestorWatchers: true,
        mergeWatchersWithCommonAncestors: true,
        maxCommonAncestorLevel: 2
      };
    default:
      return {
        reuseAncestorWatchers: true,
        relocateDescendantWatchers: false,
        relocateAncestorWatchers: true,
        mergeWatchersWithCommonAncestors: false
      };
  }
}

function registryOptionsToNativeWatcherOptions(registryOptions) {
  let recursive = false;
  // Any of the following options put us into a mode where we try to
  // consolidate and reuse native watchers. For this to be feasible (beyond two
  // `PathWatcher`s sharing a `NativeWatcher` because they care about the same
  // directory) requires that we set up a recursive watcher.
  //
  // On some platforms, this strategy doesn’t make sense, and it’s better to
  // use a different `NativeWatcher` for each directory.
  let {
    reuseAncestorWatchers,
    relocateAncestorWatchers,
    relocateDescendantWatchers,
    mergeWatchersWithCommonAncestors
  } = registryOptions;
  if (
    relocateAncestorWatchers ||
    relocateDescendantWatchers ||
    reuseAncestorWatchers ||
    mergeWatchersWithCommonAncestors
  ) {
    recursive = true;
  }

  return { recursive };
}

const REGISTRY = new NativeWatcherRegistry(
  (normalizedPath, registryOptions) => {
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
    return NativeWatcher.findOrCreate(
      normalizedPath,
      registryOptionsToNativeWatcherOptions(registryOptions)
    );
  },
  getDefaultRegistryOptionsForPlatform(process.platform)
);

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
