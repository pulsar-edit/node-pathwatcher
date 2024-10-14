const crypto = require('crypto');
const Path = require('path');
const _ = require('underscore-plus');
const { Emitter, Disposable } = require('event-kit');
const FS = require('fs-plus');
const Grim = require('grim');

let iconv;
let Directory;

const PathWatcher = require('./main');

class File {
  encoding = 'utf8';
  realPath = null;
  subscriptionCount = 0;

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

  onDidChange (callback) {
    this.willAddSubscription();
    return this.trackUnsubscription(this.emitter.on('did-change', callback));
  }

  onDidRename (callback) {
    this.willAddSubscription();
    return this.trackUnsubscription(this.emitter.on('did-rename', callback));
  }

  onDidDelete (callback) {
    this.willAddSubscription();
    return this.trackUnsubscription(this.emitter.on('did-delete', callback));
  }

  onWillThrowWatchError (_callback) {
    // DEPRECATED
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

  isFile () {
    return true;
  }

  isDirectory () {
    return false;
  }

  isSymbolicLink () {
    return this.symlink;
  }

  async exists () {
    return new Promise((resolve) => FS.exists(this.getPath(), resolve));
  }

  existsSync () {
    return FS.existsSync(this.getPath());
  }

  async getDigest () {
    if (this.digest != null) {
      return this.digest;
    }
    await this.read();
    return this.digest;
  }

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

  setEncoding (encoding = 'utf8') {
    if (encoding !== 'utf8') {
      iconv ??= require('iconv-lite');
      iconv.getCodec(encoding);
    }
    this.encoding = encoding;
    return encoding;
  }

  getEncoding () {
    return this.encoding;
  }

  /*
  Section: Managing Paths
  */

  getPath () {
    return this.path;
  }

  setPath (path) {
    this.path = path;
    this.realPath = null;
  }

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

  getBaseName () {
    return Path.basename(this.path);
  }

  /*
  Section: Traversing
  */

  getParent () {
    Directory ??= require('./directory');
    return new Directory(Path.dirname(this.path));
  }


  /*
  Section: Reading and Writing
  */

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

  writeFileSync (filePath, contents) {
    let encoding = this.getEncoding();
    if (encoding === 'utf8') {
      return FS.writeFileSync(filePath, contents, { encoding });
    } else {
      iconv ??= require('iconv-lite');
      return FS.writeFileSync(filePath, iconv.encode(contents, encoding));
    }
  }

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

  async write (text) {
    let previouslyExisted = await this.exists();
    await this.writeFile(this.getPath(), text);
    this.cachedContents = text;
    this.setDigest(text);
    if (!previouslyExisted && this.hasSubscriptions()) {
      this.subscribeToNativeChangeEvents();
    }
  }

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

  /*
  Section: Private
  */

  handleNativeChangeEvent (eventType, eventPath) {
    switch (eventType) {
      case 'delete':
        this.unsubscribeFromNativeChangeEvents();
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
