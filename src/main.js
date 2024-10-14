
let binding;
try {
  binding = require('../build/Debug/pathwatcher.node');
} catch (err) {
  binding = require('../build/Release/pathwatcher.node');
}
const { Emitter } = require('event-kit');
const fs = require('fs');
const path = require('path');

const HANDLE_WATCHERS = new Map();
let initialized = false;

function wait (ms) {
  return new Promise(r => setTimeout(r, ms));
}

class HandleWatcher {

  constructor(path) {
    this.path = path;
    this.emitter = new Emitter();
    this.start();
  }

  onEvent (event, filePath, oldFilePath) {
    filePath &&= path.normalize(filePath);
    oldFilePath &&= path.normalize(oldFilePath);

    switch (event) {
      case 'rename':
        this.close();
        let detectRename = () => {
          return fs.stat(
            this.path,
            (err) => {
              if (err) {
                this.path = filePath;
                if (process.platform === 'darwin' && /\/\.Trash\//.test(filePath)) {
                  this.emitter.emit(
                    'did-change',
                    { event: 'delete', newFilePath: null }
                  );
                  this.close();
                  return;
                } else {
                  this.start();
                  this.emitter.emit(
                    'did-change',
                    { event: 'rename', newFilePath: filePath }
                  );
                  return;
                }
              } else {
                this.start();
                this.emitter.emit(
                  'did-change',
                  { event: 'change', newFilePath: null }
                );
                return;
              }
            }
          );
        };
        setTimeout(detectRename, 100);
        return;
      case 'delete':
        this.emitter.emit(
          'did-change',
          { event: 'delete', newFilePath: null }
        );
        this.close();
        return;
      case 'unknown':
        throw new Error("Received unknown event for path: " + this.path);
      default:
        this.emitter.emit(
          'did-change',
          { event, newFilePath: filePath, oldFilePath }
        );
    }
  }

  onDidChange (callback) {
    return this.emitter.on('did-change', callback);
  }

  start () {
    let troubleWatcher;
    this.handle = binding.watch(this.path);
    if (HANDLE_WATCHERS.has(this.handle)) {
      troubleWatcher = HANDLE_WATCHERS.get(this.handle);
      troubleWatcher.close();
      console.error(`The handle (${this.handle}) returned by watching path ${this.path} is the same as an already-watched path: ${troubleWatcher.path}`);
    }
    return HANDLE_WATCHERS.set(this.handle, this);
  }

  closeIfNoListener () {
    if (this.emitter.getTotalListenerCount() === 0) {
      this.close();
    }
  }

  async close () {
    if (!HANDLE_WATCHERS.has(this.handle)) return;
    binding.unwatch(this.handle);
    HANDLE_WATCHERS.delete(this.handle);
    // Watchers take 100ms to realize they're closed.
    await wait(100);
  }
}

class PathWatcher {
  isWatchingParent = false;
  path = null;
  handleWatcher = null;
  constructor(filePath, callback) {
    this.path = filePath;
    this.emitter = new Emitter();
    if (process.platform === 'win32') {
      let stats = fs.statSync(filePath);
      this.isWatchingParent = !stats.isDirectory();
    }
    if (this.isWatchingParent) {
      filePath = path.dirname(filePath);
    }
    for (let watcher of HANDLE_WATCHERS.values()) {
      if (watcher.path === filePath) {
        this.handleWatcher = watcher;
        break;
      }
    }
    this.handleWatcher ??= new HandleWatcher(filePath);

    this.onChange = ({ event, newFilePath, oldFilePath }) => {
      switch (event) {
        case 'rename':
        case 'change':
        case 'delete':
          if (event === 'rename') {
            this.path = newFilePath;
          }
          if (typeof callback === 'function') {
            callback.call(this, event, newFilePath);
          }
          this.emitter.emit(
            'did-change',
            { event, newFilePath }
          );
          return;
        case 'child-rename':
          if (this.isWatchingParent) {
            if (this.path === oldFilePath) {
              return this.onChange({ event: 'rename', newFilePath });
            }
          } else {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
          break;
        case 'child-delete':
          if (this.isWatchingParent) {
            if (this.path === newFilePath) {
              return this.onChange({ event: 'delete', newFilePath: null });
            }
          } else {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
          break;
        case 'child-change':
          if (this.isWatchingParent && this.path === newFilePath) {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
          break;
        case 'child-create':
          if (!this.isWatchingParent) {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
      }
    };

    this.disposable = this.handleWatcher.onDidChange(this.onChange);
  }

  onDidChange (callback) {
    return this.emitter.on('did-change', callback);
  }

  close () {
    this.emitter?.dispose();
    this.disposable?.dispose();
    this.handleWatcher?.closeIfNoListener();
  }
}

function watch (pathToWatch, callback) {
  if (!initialized) {
    initialized = true;
    binding.setCallback((event, handle, filePath, oldFilePath) => {
      if (!HANDLE_WATCHERS.has(handle)) return;
      HANDLE_WATCHERS.get(handle).onEvent(event, filePath, oldFilePath);
    });
  }

  return new PathWatcher(path.resolve(pathToWatch), callback);
}

async function closeAllWatchers () {
  let promises = [];
  for (let watcher of HANDLE_WATCHERS.values()) {
    promises.push(watcher?.close());
  }
  HANDLE_WATCHERS.clear();
  await Promise.allSettled(promises);
}

function getWatchedPaths () {
  let watchers = Array.from(HANDLE_WATCHERS.values());
  return watchers.map(w => w.path);
}

const File = require('./file');
const Directory = require('./directory');

module.exports = {
  watch,
  closeAllWatchers,
  getWatchedPaths,
  File,
  Directory
};
