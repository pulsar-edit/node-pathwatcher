const crypto = require('crypto');
const Path = require('path');
const { Emitter, Disposable } = require('event-kit');
const _ = require('underscore-plus');
const FS = require('fs-plus');
const Grim = require('grim');

let iconv;
let Directory;

async function wait (ms) {
  return new Promise(r => setTimeout(r, ms));
}

// Lazy-load the main `PathWatcher` import to prevent circular references.
let PathWatcher;

// Extended: Represents an individual file that can be watched, read from, and
// written to.
class File {
  encoding = 'utf8';
  realPath = null;
  subscriptionCount = 0;

  /*
  Section: Construction
  */

  // Public: Configures a new {File} instance.
  //
  // No files are accessed. The file does not yet need to exist.
  //
  // * `filePath` A {String} containing the absolute path to the file.
  // * `symlink` (optional) A {Boolean} indicating if the path is a symlink
  //   (default: false).
  constructor(
    filePath,
    symlink = false,
    includeDeprecatedAPIs = Grim.includeDeprecatedAPIs
  ) {
    this.didRemoveSubscription = this.didRemoveSubscription.bind(this);
    this.willAddSubscription = this.willAddSubscription.bind(this);
    this.symlink = symlink;

    filePath &&= Path.normalize(filePath);
    this.path = filePath;
    this.emitter = new Emitter();

    if (includeDeprecatedAPIs) {
      this.on('contents-changed-subscription-will-be-added', this.willAddSubscription);
      this.on('moved-subscription-will-be-added', this.willAddSubscription);
      this.on('removed-subscription-will-be-added', this.willAddSubscription);
      this.on('contents-changed-subscription-removed', this.didRemoveSubscription);
      this.on('moved-subscription-removed', this.didRemoveSubscription);
      this.on('removed-subscription-removed', this.didRemoveSubscription);
    }

    this.cachedContents = null;
    this.reportOnDeprecations = true;
  }

  // Public: Creates the file on disk that corresponds to {::getPath} if no
  // such file already exists.
  //
  // Returns a {Promise} that resolves once the file is created on disk. It
  // resolves to a boolean value that is `true` if the file was created or
  // `false` if it already existed.
  //
  async create () {
    let isExistingFile = await this.exists();
    let parent;
    if (!isExistingFile) {
      parent = this.getParent();
      await parent.create();
      await this.write('');
      return true;
    } else {
      return false;
    }
  }

  /*
  Section: Event Subscription
  */

  // Public: Invoke the given callback when the file’s contents change.
  //
  // * `callback` {Function} to be called when the file’s contents change.
  //   Takes no arguments.
  //
  // Returns a {Disposable} on which {Disposable::dispose} can be called to
  // unsubscribe.
  onDidChange (callback) {
    this.willAddSubscription();
    // Add a small buffer here. If a file has changed, we want to wait briefly
    // to see if it's prelude to a delete event (as EFSW sometimes does). The
    // good news is that we don't have to wait very long at all.
    let wrappedCallback = async (...args) => {
      await wait(0);
      if (!(await this.exists())) return;
      callback(...args);
    };
    return this.trackUnsubscription(this.emitter.on('did-change', wrappedCallback));
  }

  // Public: Invoke the given callback when the file’s path changes.
  //
  // * `callback` {Function} to be called when the file’s path changes.
  //   Takes no arguments.
  //
  // Returns a {Disposable} on which {Disposable::dispose} can be called to
  // unsubscribe.
  onDidRename (callback) {
    this.willAddSubscription();
    return this.trackUnsubscription(this.emitter.on('did-rename', callback));
  }

  // Public: Invoke the given callback when the file is deleted.
  //
  // * `callback` {Function} to be called when the file is deleted.
  //   Takes no arguments.
  //
  // Returns a {Disposable} on which {Disposable::dispose} can be called to
  // unsubscribe.
  onDidDelete (callback) {
    this.willAddSubscription();
    return this.trackUnsubscription(this.emitter.on('did-delete', callback));
  }

  onWillThrowWatchError (_callback) {
    // Deprecated callback; must return a `Disposable` for compatibility.
    return new Disposable(() => {});
  }

  willAddSubscription () {
    this.subscriptionCount++;
    try {
      return this.subscribeToNativeChangeEvents();
    } catch (_err) {}
  }

  didRemoveSubscription () {
    this.subscriptionCount--;
    if (this.subscriptionCount === 0) {
      return this.unsubscribeFromNativeChangeEvents();
    }
  }

  trackUnsubscription (subscription) {
    return new Disposable(() => {
      subscription.dispose();
      this.didRemoveSubscription();
    });
  }

  /*
  Section: File Metadata
  */

  // Public: Returns a {Boolean}; always `true`.
  isFile () {
    return true;
  }

  // Public: Returns a {Boolean}; always `false`.
  isDirectory () {
    return false;
  }

  // Public: Returns a {Boolean} indicating whether or not this is a symbolic
  // link.
  isSymbolicLink () {
    return this.symlink;
  }

  // Public: Returns a {Promise} that resolves to a {Boolean}: `true` if the
  // file exists; `false` otherwise.
  async exists () {
    return new Promise((resolve) => FS.exists(this.getPath(), resolve));
  }

  // Public: Returns a {Boolean}: `true` if the file exists; `false` otherwise.
  existsSync () {
    return FS.existsSync(this.getPath());
  }

  // Public: Get the SHA-1 digest of this file.
  //
  // Returns a {Promise} that resolves to a {String}.
  async getDigest () {
    if (this.digest != null) {
      return this.digest;
    }
    await this.read();
    return this.digest;
  }

  // Public: Get the SHA-1 digest of this file.
  //
  // Returns a {String}.
  getDigestSync () {
    if (this.digest == null) {
      this.readSync();
    }
    return this.digest;
  }


  setDigest (contents) {
    this.digest = crypto
      .createHash('sha1')
      .update(contents ?? '')
      .digest('hex');
    return this.digest;
  }

  // Public: Sets the file's character set encoding name.
  //
  // Supports `utf8` natively and whichever other encodings are supported by
  // the `iconv-lite` package.
  setEncoding (encoding = 'utf8') {
    if (encoding !== 'utf8') {
      iconv ??= require('iconv-lite');
      iconv.getCodec(encoding);
    }
    this.encoding = encoding;
    return encoding;
  }

  // Public: Returns the {String} encoding name for this file; default is
  // `utf8`.
  getEncoding () {
    return this.encoding;
  }

  /*
  Section: Managing Paths
  */

  // Public: Returns the {String} path for this file.
  getPath () {
    return this.path;
  }

  // Public: Sets the path for the file.
  //
  // This should not normally need to be called; use it only when you know a
  // file’s path has changed and you don’t want to rely on the internal
  // renaming detection.
  //
  // * `path` {String} The new path to set; should be absolute.
  setPath (path) {
    this.path = path;
    this.realPath = null;
  }

  // Public: Returns a {Promise} that resolves to this file’s completely
  // resolved {String} path, following symlinks if necessary.
  async getRealPath () {
    if (this.realPath != null) {
      return this.realPath;
    }
    return new Promise((resolve, reject) => {
      FS.realpath(this.path, (err, result) => {
        if (err != null) return reject(err);
        this.realPath = result;
        return resolve(this.realPath);
      });
    });
  }

  // Public: Returns this file’s completely resolved {String} path, following
  // symlinks if necessary.
  getRealPathSync () {
    if (this.realPath == null) {
      try {
        this.realPath = FS.realpathSync(this.path);
      } catch (_error) {
        this.realPath = this.path;
      }
    }
    return this.realPath;
  }

  // Public: Returns the {String} filename of this file without its directory
  // context.
  getBaseName () {
    return Path.basename(this.path);
  }

  /*
  Section: Traversing
  */

  // Public: Returns the {Directory} that contains this file.
  getParent () {
    Directory ??= require('./directory');
    return new Directory(Path.dirname(this.path));
  }


  /*
  Section: Reading and Writing
  */

  // Public: Reads the contents of the file.
  //
  // * `flushCache` A {Boolean} indicating whether to require a direct read or
  //   if a cached copy is acceptable.
  //
  // Returns a {Promise} that resolves to a {String} (if the file exists) or
  // `null` (if it does not).
  async read (flushCache) {
    let contents;
    if (!flushCache && this.cachedContents != null) {
      contents = this.cachedContents;
    } else {
      contents = await new Promise((resolve, reject) => {
        let content = [];
        let readStream = this.createReadStream();
        readStream.on('data', (chunk) => content.push(chunk));
        readStream.on('end', () => resolve(content.join('')));
        readStream.on('error', (error) => {
          if (error.code === 'ENOENT') {
            return resolve(null);
          } else {
            return reject(error);
          }
        })
      });
    }
    this.setDigest(contents);
    this.cachedContents = contents;
    return contents;
  }

  // Public: Reads the contents of the file synchronously.
  //
  // * `flushCache` A {Boolean} indicating whether to require a direct read or
  //   if a cached copy is acceptable.
  //
  // Returns a {String} (if the file exists) or `null` (if it does not).
  readSync (flushCache) {
    if (!this.existsSync()) {
      this.cachedContents = null;
    } else if ((this.cachedContents == null) || flushCache) {
      let encoding = this.getEncoding();
      if (encoding === 'utf8') {
        this.cachedContents = FS.readFileSync(this.getPath(), encoding);
      } else {
        iconv ??= require('iconv-lite');
        this.cachedContents = iconv.decode(
          FS.readFileSync(this.getPath()),
          encoding
        );
      }
    }
    this.setDigest(this.cachedContents);
    return this.cachedContents;
  }

  // Public: Returns a stream to read the content of the file.
  //
  // Returns a {ReadStream} object.
  createReadStream () {
    let encoding = this.getEncoding();
    if (encoding === 'utf8') {
      return FS.createReadStream(this.getPath(), { encoding });
    } else {
      iconv ??= require('iconv-lite');
      return FS.createReadStream(this.getPath())
        .pipe(iconv.decodeStream(encoding));
    }
  }

  // Public: Overwrites the file with the given text.
  //
  // * `text` The {String} text to write to the underlying file.
  //
  // Returns a {Promise} that resolves when the file has been written.
  async write (text) {
    let previouslyExisted = await this.exists();
    await this.writeFile(this.getPath(), text);
    this.cachedContents = text;
    this.setDigest(text);
    if (!previouslyExisted && this.hasSubscriptions()) {
      this.subscribeToNativeChangeEvents();
    }
  }

  // Public: Overwrites the file with the given text.
  //
  // * `text` The {String} text to write to the underlying file.
  writeSync (text) {
    let previouslyExisted = this.existsSync();
    this.writeFileSync(this.getPath(), text);
    this.cachedContents = text;
    this.setDigest(text);
    if (Grim.includeDeprecatedAPIs) {
      this.emit('contents-changed');
    }
    this.emitter.emit('did-change');
    if (!previouslyExisted && this.hasSubscriptions()) {
      this.subscribeToNativeChangeEvents();
    }
  }

  // Public: Returns a stream to write content to the file.
  //
  // Returns a {WriteStream} object.
  createWriteStream () {
    let encoding = this.getEncoding();
    if (encoding === 'utf8') {
      return FS.createWriteStream(this.getPath(), { encoding });
    } else {
      iconv ??= require('iconv-lite');
      let stream = iconv.encodeStream(encoding);
      stream.pipe(FS.createWriteStream(this.getPath()));
      return stream;
    }
  }

  // Internal helper method for writing to a file.
  async writeFile (filePath, contents) {
    let encoding = this.getEncoding();
    if (encoding === 'utf8') {
      return new Promise((resolve, reject) => {
        FS.writeFile(
          filePath,
          contents,
          { encoding },
          (err, result) => {
            if (err != null) {
              reject(err);
            } else {
              resolve(result);
            }
          }
        )
      });
    } else {
      iconv ??= require('iconv-lite');
      return new Promise((resolve, reject) => {
        FS.writeFile(
          filePath,
          iconv.encode(contents, encoding),
          (err, result) => {
            if (err != null) {
              reject(err);
            } else {
              resolve(result);
            }
          }
        )
      });
    }
  }

  // Internal helper method for writing to a file.
  writeFileSync (filePath, contents) {
    let encoding = this.getEncoding();
    if (encoding === 'utf8') {
      return FS.writeFileSync(filePath, contents, { encoding });
    } else {
      iconv ??= require('iconv-lite');
      return FS.writeFileSync(filePath, iconv.encode(contents, encoding));
    }
  }

  /*
  Section: Private
  */

  async handleNativeChangeEvent (eventType, eventPath) {
    switch (eventType) {
      case 'delete':
        this.unsubscribeFromNativeChangeEvents();
        // We could just `await wait(50)` here, but this method exists so that
        // we can monkeypatch it in the Pulsar specs and prevent it from going
        // async.
        this.detectResurrectionAfterDelay();
        return;
      case 'rename':
        this.setPath(eventPath);
        if (Grim.includeDeprecatedAPIs) {
          this.emit('moved');
        }
        this.emitter.emit('did-rename');
        return;
      case 'change':
      case 'resurrect':
        this.cachedContents = null;
        this.emitter.emit('did-change');
    }
  }

  detectResurrectionAfterDelay () {
    return _.delay(() => this.detectResurrection(), 50);
  }

  async detectResurrection () {
    let exists = await this.exists();
    if (exists) {
      this.subscribeToNativeChangeEvents();
      this.handleNativeChangeEvent('resurrect');
    } else {
      this.cachedContents = null;
      if (Grim.includeDeprecatedAPIs) {
        this.emit('removed');
      }
      this.emitter.emit('did-delete');
    }
  }

  subscribeToNativeChangeEvents () {
    PathWatcher ??= require('./main');
    this.watchSubscription ??= PathWatcher.watch(
      this.path,
      (...args) => {
        return this.handleNativeChangeEvent(...args);
      }
    );
    return this.watchSubscription;
  }

  unsubscribeFromNativeChangeEvents () {
    this.watchSubscription?.close();
    this.watchSubscription &&= null;
  }
}

if (Grim.includeDeprecatedAPIs) {
  EmitterMixin = require('emissary').Emitter;
  EmitterMixin.includeInto(File);
  File.prototype.on = function(eventName) {
    switch (eventName) {
      case 'contents-changed':
        Grim.deprecate("Use File::onDidChange instead");
        break;
      case 'moved':
        Grim.deprecate("Use File::onDidRename instead");
        break;
      case 'removed':
        Grim.deprecate("Use File::onDidDelete instead");
        break;
      default:
        if (this.reportOnDeprecations) {
          Grim.deprecate("Subscribing via ::on is deprecated. Use documented event subscription methods instead.");
        }
    }
    return EmitterMixin.prototype.on.apply(this, arguments);
  };
} else {
  File.prototype.hasSubscriptions = function() {
    return this.subscriptionCount > 0;
  };
}


module.exports = File;
