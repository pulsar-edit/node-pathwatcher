const Path = require('path');
const FS = require('fs-plus');
const Grim = require('grim');
const async = require('async');
const { Emitter, Disposable } = require('event-kit');

const File = require('./file');
// Lazy-load the main `PathWatcher` import to prevent circular references.
let PathWatcher;

// Extended: Represents a directory on disk that can be traversed or watched
// for changes.
class Directory {
  realPath = null;
  subscriptionCount = 0;

  /*
  Section: Construction
  */

  // Public: Configures a new {Directory} instance.
  //
  // No files are accessed. The directory does not yet need to exist.
  //
  // * `directoryPath` A {String} containing the absolute path to the
  //   directory.
  // * `symlink` (optional) A {Boolean} indicating if the path is a symlink
  //   (default: `false`).
  constructor(directoryPath, symlink = false, includeDeprecatedAPIs = Grim.includeDeprecatedAPIs) {
    this.emitter = new Emitter();
    this.symlink = symlink;

    if (includeDeprecatedAPIs) {
      this.on('contents-changed-subscription-will-be-added', this.willAddSubscription.bind(this));
      this.on('contents-changed-subscription-removed', this.didRemoveSubscription.bind(this));
    }

    if (directoryPath) {
      directoryPath = Path.normalize(directoryPath);
      if (directoryPath.length > 1 && directoryPath.endsWith(Path.sep)) {
        directoryPath = directoryPath.substring(0, directoryPath.length - 1);
      }
    }
    this.path = directoryPath;
    if (FS.isCaseInsensitive()) {
      this.lowerCasePath = this.path.toLowerCase();
    }
    if (Grim.includeDeprecatedAPIs) {
      this.reportOnDeprecations  = true;
    }
  }

  // Public: Creates the directory on disk that corresponds to {::getPath} if
  // no such directory already exists.
  //
  // * `mode` (optional) {Number} that defaults to `0777` and represents the
  //   default permissions of the directory on supported platforms.
  //
  // Returns a {Promise} that resolves to a {Boolean}: `true` if the directory
  // was created and `false` if it already existed.
  async create (mode = 0o0777) {
    let isExistingDirectory = await this.exists();
    if (isExistingDirectory) return false;
    if (this.isRoot()) {
      throw new Error(`Root directory does not exist: ${this.getPath()}`);
    }
    await this.getParent().create();
    return new Promise((resolve, reject) => {
      FS.mkdir(this.getPath(), mode, (error) => {
        if (error) {
          reject(error);
        } else {
          resolve(true);
        }
      });
    });
  }

  /*
  Section: Event Subscription
  */

  // Public: Invoke the given callback when the directory’s contents change.
  //
  // A directory’s contents are considered to have changed when one of its
  // children is added, deleted, or renamed. This callback will not fire when
  // one of its children has its contents changed.
  //
  // * `callback` {Function} to be called when the directory’s contents change.
  //   Takes no arguments.
  //
  // Returns a {Disposable} on which {Disposable::dispose} can be called to
  // unsubscribe.
  onDidChange (callback) {
    this.willAddSubscription();
    return this.trackUnsubscription(
      this.emitter.on('did-change', callback)
    );
  }

  willAddSubscription () {
    if (this.subscriptionCount === 0) {
      this.subscribeToNativeChangeEvents();
    }
    this.subscriptionCount++;
  }

  didRemoveSubscription () {
    this.subscriptionCount--;
    if (this.subscriptionCount === 0) {
      this.unsubscribeFromNativeChangeEvents();
    }
  }

  trackUnsubscription (subscription) {
    return new Disposable(() => {
      subscription.dispose();
      this.didRemoveSubscription();
    });
  }

  // Public: Returns a {Boolean}; always `false`.
  isFile () {
    return false;
  }

  // Public: Returns a {Boolean}; always `true`.
  isDirectory() {
    return true;
  }

  // Public: Returns a {Boolean} indicating whetehr or not this is a symbolic
  // link.
  isSymbolicLink () {
    return this.symlink;
  }

  // Public: Returns a {Promise} that resolves to a {Boolean}: `true` if the
  // directory exists, `false` otherwise.
  exists () {
    return new Promise((resolve) => {
      FS.exists(this.getPath(), resolve)
    });
  }

  // Public: Returns a {Boolean}: `true` if the directory exists, `false`
  // otherwise.
  existsSync () {
    return FS.existsSync(this.getPath());
  }

  // Public: Returns a {Boolean}: `true` if this {Directory} is the root
  // directory of the filesystem, or `false` if it isn’t.
  isRoot () {
    let realPath = this.getRealPathSync();
    return realPath === this.getParent().getRealPathSync();
  }

  /*
  Section: Managing Paths
  */

  // Public: Returns the directory’s {String} path.
  //
  // This may include unfollowed symlinks or relative directory entries; or it
  // may be fully resolved. It depends on what you give it. Anything that
  // Node’s builtin `fs` and `path` libraries can resolve will also be
  // understood by {Directory}.
  getPath () {
    return this.path;
  }

  // Public: Returns the directory’s resolved {String} path, resolving symlinks
  // if necessary.
  //
  // This will always be an absolute path; all relative paths are resolved and
  // all symlinks are followed.
  getRealPathSync () {
    if (!this.realPath) {
      try {
        this.realPath = FS.realpathSync(this.path);
        if (FS.isCaseInsensitive()) {
          this.lowerCaseRealPath = this.realPath.toLowerCase();
        }
      } catch (err) {
        this.realPath = this.path;
        if (FS.isCaseInsensitive()) {
          this.lowerCaseRealPath = this.lowerCasePath;
        }
      }
    }
    return this.realPath;
  }

  // Public: Returns the {String} basename of the directory.
  getBaseName () {
    return Path.basename(this.path);
  }

  // Public: Returns the relative {String} path to the given path from this
  // directory. If the given path is not a descendant of this directory, will
  // return its full absolute path.
  //
  // * `fullPath` A path to compare against the real, absolute path of this
  //   {Directory}.
  relativize (fullPath) {
    if (!fullPath) return fullPath;

    if (process.platform === 'win32') {
      fullPath = fullPath.replace(/\//g, '\\');
    }

    let pathToCheck;
    let directoryPath;
    if (FS.isCaseInsensitive()) {
      pathToCheck = fullPath.toLowerCase();
      directoryPath = this.lowerCasePath;
    } else {
      pathToCheck = fullPath;
      directoryPath = this.path;
    }

    if (pathToCheck === directoryPath) {
      return '';
    } else if (this.isPathPrefixOf(directoryPath, pathToCheck)) {
      return fullPath.substring(directoryPath.length + 1);
    }

    // Check the real path.
    this.getRealPathSync();
    if (FS.isCaseInsensitive()) {
      directoryPath = this.lowerCaseRealPath;
    } else {
      directoryPath = this.realPath;
    }

    if (pathToCheck === directoryPath) {
      return '';
    } else if (this.isPathPrefixOf(directoryPath, pathToCheck)) {
      return fullPath.substring(directoryPath.length + 1);
    } else {
      return fullPath;
    }
  }

  // Public: Resolves the given relative path to an absolute path relative to
  // this directory. If the path is already absolute or prefixed with a URI
  // scheme, it is returned unchanged.
  //
  // * `uri` A {String} containing the path to resolve.
  //
  // Returns a {String} containing an absolute path, or `undefined` if the
  // given URI is falsy (like an empty string).
  resolve (relativePath) {
    if (!relativePath) return;

    if (relativePath?.match(/[A-Za-z0-9+-.]+:\/\//)) {
      // Leave the path alone if it has a scheme.
      return relativePath;
    } else if (FS.isAbsolute(relativePath)) {
      return Path.normalize(FS.resolveHome(relativePath));
    } else {
      return Path.normalize(
        FS.resolveHome(Path.join(this.getPath(), relativePath))
      );
    }
  }

  /*
  Section: Traversing
  */

  // Public: Traverse to the parent directory.
  //
  // Returns a {Directory}.
  getParent () {
    return new Directory(Path.join(this.path, '..'));
  }

  // Public: Traverse within this {Directory} to a child {File}. This method
  // doesn't actually check to see if the {File} exists; it just creates the
  // {File} object.
  //
  // You can also access descendant files by passing multiple arguments. In
  // this usage, the final segment should be the name of a file; the others
  // should be directories.
  //
  // * `filename` The {String} name of a File within this Directory.
  //
  // Returns a {File}.
  getFile (...fileName) {
    return new File(Path.join(this.getPath(), ...fileName));
  }

  // Public: Traverse within this {Directory} to a child {Directory}. This
  // method doesn't actually check to see if the directory exists; it just
  // creates the {Directory} object.
  //
  // You can also access descendant directories by passing multiple arguments.
  // In this usage, all segments should be directory names.
  //
  // * `dirname` The {String} name of the child {Directory}.
  //
  // Returns a {Directory}.
  getSubdirectory (...dirName) {
    return new Directory(Path.join(this.getPath(), ...dirName));
  }

  // Public: Reads file entries in this directory from disk asynchronously and
  // applies a function to each.
  //
  // * `callback` A {Function} to call with the following arguments:
  //   * `error` An {Error}, may be null.
  //   * `entries` An {Array} of {File} and {Directory} objects.
  getEntries (callback) {
    FS.list(this.path, (error, entries) => {
      if (error) return callback(error);

      let directories = [];
      let files = [];

      let addEntry = (entryPath, stat, symlink, innerCallback) => {
        if (stat?.isDirectory()) {
          directories.push(new Directory(entryPath, symlink));
        } else if (stat?.isFile()) {
          files.push(new File(entryPath, symlink))
        }
        return innerCallback();
      };

      let statEntry = (entryPath, innerCallback) => {
        FS.lstat(entryPath, (_error, stat) => {
          if (stat?.isSymbolicLink()) {
            FS.stat(entryPath, (_error, stat) => {
              addEntry(entryPath, stat, true, innerCallback)
            });
          } else {
            addEntry(entryPath, stat, false, innerCallback);
          }
        });
      };

      return async.eachLimit(
        entries,
        1,
        statEntry,
        function() {
          return callback(null, directories.concat(files));
        }
      );
    });
  }

  // Public: Reads file entries in this directory from disk synchronously.
  //
  // Returns an {Array} of {File} and {Directory} objects.
  getEntriesSync () {
    let directories = [];
    let files = [];
    for (let entryPath of FS.listSync(this.path)) {
      let stat;
      let symlink = false;
      try {
        stat = FS.lstatSync(entryPath);
        symlink = stat.isSymbolicLink();
        if (symlink) {
          stat = FS.statSync(entryPath);
        }
      } catch (_err) {}
      if (stat?.isDirectory()) {
        directories.push(new Directory(entryPath, symlink));
      } else if (stat?.isFile()) {
        files.push(new File(entryPath, symlink));
      }
    }
    return directories.concat(files);
  }


  // Public: Determines if the given path (real or symbolic) is inside this
  // directory. This method does not actually check if the path exists; it just
  // checks if the path is under this directory.
  //
  // * `pathToCheck` The {String} path to check.
  //
  // Returns a {Boolean} whether the given path is inside this directory.
  contains (pathToCheck) {
    if (!pathToCheck) return false;

    // Normalize forward slashes to backslashes on Windows.
    if (process.platform === 'win32') {
      pathToCheck = pathToCheck.replace(/\//g, '\\');
    }

    let directoryPath;
    if (FS.isCaseInsensitive()) {
      directoryPath = this.lowerCasePath;
      pathToCheck = pathToCheck.toLowerCase();
    } else {
      directoryPath = this.path;
    }

    if (this.isPathPrefixOf(directoryPath, pathToCheck)) {
      return true;
    }

    // Check the real path.
    this.getRealPathSync();
    if (FS.isCaseInsensitive()) {
      directoryPath = this.lowerCaseRealPath;
    } else {
      directoryPath = this.realPath;
    }

    return this.isPathPrefixOf(directoryPath, pathToCheck);
  }

  /*
  Section: Private
  */

  subscribeToNativeChangeEvents () {
    PathWatcher ??= require('./main');
    this.watchSubscription ??= PathWatcher.watch(
      this.path,
      (_eventType) => {
        if (Grim.includeDeprecatedAPIs) {
          this.emit('contents-changed');
        }
        this.emitter.emit('did-change');
      }
    );
  }

  unsubscribeFromNativeChangeEvents () {
    this.watchSubscription?.close();
    this.watchSubscription &&= null;
  }

  // Does the given full path start with the given prefix?
  isPathPrefixOf (prefix, fullPath) {
    return fullPath.startsWith(prefix) && fullPath[prefix.length] === Path.sep;
  }
}

let EmitterMixin;
if (Grim.includeDeprecatedAPIs) {
  EmitterMixin = require('emissary').Emitter;
  EmitterMixin.includeInto(Directory);

  Directory.prototype.on = function on(eventName, ...args) {
    if (eventName === 'contents-changed') {
      Grim.deprecate("Use Directory::onDidChange instead");
    } else if (this.reportOnDeprecations) {
      Grim.deprecate("Subscribing via ::on is deprecated. Use documented event subscription methods instead.");
    }
    EmitterMixin.prototype.on.call(this, eventName, ...args);
  };
}

module.exports = Directory;
