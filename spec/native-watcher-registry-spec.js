const path = require('path');
const { Emitter } = require('event-kit');
const { NativeWatcherRegistry } = require('../src/native-watcher-registry');

function findRootDirectory() {
  let current = process.cwd();
  while (true) {
    let next = path.resolve(current, '..');
    if (next === current) {
      return next;
    } else {
      current = next;
    }
  }
}

function EMPTY() {}

const ROOT = findRootDirectory();

function absolute(...parts) {
  const candidate = path.join(...parts);
  return path.isAbsolute(candidate) ? candidate : path.join(ROOT, candidate);
}

function parts(fullPath) {
  return fullPath.split(path.sep).filter(part => part.length > 0);
}

class MockWatcher {
  constructor(normalizedPath) {
    this.normalizedPath = normalizedPath;
    this.native = null;
    this.active = true;
  }

  getNormalizedPathPromise() {
    return Promise.resolve(this.normalizedPath);
  }

  getNormalizedPath () {
    return this.normalizedPath;
  }

  attachToNative(native, nativePath) {
    if (this.normalizedPath.startsWith(nativePath)) {
      if (this.native) {
        this.native.attached = this.native.attached.filter(
          each => each !== this
        );
      }
      this.native = native;
      this.native.onDidChange(EMPTY);
      this.native.attached.push(this);
    }
  }

  stop () {
    this.active = false;
    this.native.stopIfNoListeners();
  }
}

class MockNative {
  constructor(name) {
    this.name = name;
    this.attached = [];
    this.disposed = false;
    this.stopped = true;
    this.wasListenedTo = false;

    this.emitter = new Emitter();
  }

  reattachListenersTo(newNative, nativePath) {
    for (const watcher of this.attached) {
      watcher.attachToNative(newNative, nativePath);
    }
  }

  onDidChange (callback) {
    this.wasListenedTo = true;
    this.stopped = false;
    return this.emitter.on('did-change', callback);
  }

  onWillStop(callback) {
    return this.emitter.on('will-stop', callback);
  }

  dispose() {
    this.disposed = true;
  }

  get listenerCount () {
    return this.emitter.listenerCountForEventName('did-change');
  }

  stopIfNoListeners () {
    if (this.listenerCount > 0) return;
    this.stop();
  }

  stop() {
    console.log('Stopping:', this.name);
    this.stopped = true;
    this.emitter.emit('will-stop');
    this.dispose();
  }
}

xdescribe('NativeWatcherRegistry', function() {
  let createNative, registry;

  beforeEach(function() {
    registry = new NativeWatcherRegistry(normalizedPath =>
      createNative(normalizedPath)
    );
  });

  it('attaches a Watcher to a newly created NativeWatcher for a new directory', async function() {
    const watcher = new MockWatcher(absolute('some', 'path'));
    const NATIVE = new MockNative('created');
    createNative = () => NATIVE;

    await registry.attach(watcher);

    expect(watcher.native).toBe(NATIVE);
  });

  it('reuses an existing NativeWatcher on the same directory', async function() {
    const EXISTING = new MockNative('existing');
    const existingPath = absolute('existing', 'path');
    let firstTime = true;
    createNative = () => {
      if (firstTime) {
        firstTime = false;
        return EXISTING;
      }

      return new MockNative('nope');
    };
    await registry.attach(new MockWatcher(existingPath));

    const watcher = new MockWatcher(existingPath);
    await registry.attach(watcher);

    expect(watcher.native).toBe(EXISTING);
  });

  it('attaches to an existing NativeWatcher on a parent directory', async function() {
    const EXISTING = new MockNative('existing');
    const parentDir = absolute('existing', 'path');
    const subDir = path.join(parentDir, 'sub', 'directory');
    let firstTime = true;
    createNative = () => {
      if (firstTime) {
        firstTime = false;
        return EXISTING;
      }

      return new MockNative('nope');
    };
    await registry.attach(new MockWatcher(parentDir));

    const watcher = new MockWatcher(subDir);
    await registry.attach(watcher);

    expect(watcher.native).toBe(EXISTING);
  });

  it('adopts Watchers from NativeWatchers on child directories', async function() {
    const parentDir = absolute('existing', 'path');
    const childDir0 = path.join(parentDir, 'child', 'directory', 'zero');
    const childDir1 = path.join(parentDir, 'child', 'directory', 'one');
    const otherDir = absolute('another', 'path');

    const expectedCommonDir = path.join(parentDir, 'child', 'directory');

    const CHILD0 = new MockNative('existing0');
    const CHILD1 = new MockNative('existing1');
    const COMMON = new MockNative('commonAncestor');
    const OTHER = new MockNative('existing2');
    const PARENT = new MockNative('parent');

    createNative = dir => {
      if (dir === childDir0) {
        return CHILD0;
      } else if (dir === childDir1) {
        return CHILD1;
      } else if (dir === otherDir) {
        return OTHER;
      } else if (dir === parentDir) {
        return PARENT;
      } else if (dir === expectedCommonDir) {
        return COMMON;
      } else {
        throw new Error(`Unexpected path: ${dir}`);
      }
    };

    // With only one watcher, we expect it to be CHILD0.
    const watcher0 = new MockWatcher(childDir0);
    await registry.attach(watcher0);
    expect(watcher0.native).toBe(CHILD0);

    // When we add another watcher at a sibling path, we expect them to unify
    // under a common watcher on their closest ancestor.
    const watcher1 = new MockWatcher(childDir1);
    await registry.attach(watcher1);
    expect(watcher1.native).toBe(COMMON);
    expect(CHILD0.stopped).toBe(true);

    const watcher2 = new MockWatcher(otherDir);
    await registry.attach(watcher2);
    expect(watcher2.native).toBe(OTHER);

    // Consolidate all three watchers beneath the same native watcher on the
    // parent directory.
    const watcher = new MockWatcher(parentDir);
    await registry.attach(watcher);
    expect(watcher.native).toBe(PARENT);
    expect(watcher0.native).toBe(PARENT);

    expect(CHILD0.stopped).toBe(true);
    expect(CHILD0.disposed).toBe(true);

    expect(watcher1.native).toBe(PARENT);
    // CHILD1 should never have been used.
    expect(CHILD1.stopped).toBe(true);
    expect(CHILD1.wasListenedTo).toBe(false);

    expect(watcher2.native).toBe(OTHER);
    expect(OTHER.stopped).toBe(false);
    expect(OTHER.disposed).toBe(false);
  });

  describe('removing NativeWatchers', function() {
    it('happens when nothing is subscribed to them', async function() {
      const STOPPED = new MockNative('stopped');
      const RUNNING = new MockNative('running');

      const stoppedPath = absolute('watcher', 'that', 'will', 'be', 'stopped');
      const stoppedPathParts = stoppedPath
        .split(path.sep)
        .filter(part => part.length > 0);
      const runningPath = absolute(
        'watcher',
        'that',
        'will',
        'be'
      );
      const runningPathParts = runningPath
        .split(path.sep)
        .filter(part => part.length > 0);

      createNative = dir => {
        if (dir === stoppedPath) {
          return STOPPED;
        } else if (dir === runningPath) {
          return RUNNING;
        } else {
          throw new Error(`Unexpected path: ${dir}`);
        }
      };

      const stoppedWatcher = new MockWatcher(stoppedPath);
      await registry.attach(stoppedWatcher);

      const runningWatcher = new MockWatcher(runningPath);
      await registry.attach(runningWatcher);

      stoppedWatcher.stop();
      registry.detach(stoppedWatcher);

      const runningNode = registry.tree.root.lookup(runningPathParts).when({
        parent: node => node,
        missing: () => false,
        children: () => false
      });

      expect(runningNode).toBeTruthy();
      expect(runningNode.getNativeWatcher()).toBe(RUNNING);

      // Either of the `parent` or `missing` outcomes would be fine here as
      // long as the exact node doesn't exist.
      const stoppedNode = registry.tree.root.lookup(stoppedPathParts).when({
        parent: (_, remaining) => remaining.length > 0,
        missing: () => true,
        children: () => false
      });

      expect(stoppedNode).toBe(true);
    });

    it('reassigns new child watchers when a parent watcher is stopped', async function() {
      const CHILD0 = new MockNative('child0');
      const CHILD1 = new MockNative('child1');
      const PARENT = new MockNative('parent');

      const parentDir = absolute('parent');
      const childDir0 = path.join(parentDir, 'child0');
      const childDir1 = path.join(parentDir, 'child1');

      createNative = dir => {
        if (dir === parentDir) {
          return PARENT;
        } else if (dir === childDir0) {
          return CHILD0;
        } else if (dir === childDir1) {
          return CHILD1;
        } else {
          throw new Error(`Unexpected directory ${dir}`);
        }
      };

      const parentWatcher = new MockWatcher(parentDir);
      const childWatcher0 = new MockWatcher(childDir0);
      const childWatcher1 = new MockWatcher(childDir1);

      await registry.attach(parentWatcher);
      await Promise.all([
        registry.attach(childWatcher0),
        registry.attach(childWatcher1)
      ]);

      // All three watchers should share the parent watcher's native watcher.
      expect(parentWatcher.native).toBe(PARENT);
      expect(childWatcher0.native).toBe(PARENT);
      expect(childWatcher1.native).toBe(PARENT);

      // Stopping the parent should detach and recreate the child watchers.
      parentWatcher.stop();
      registry.detach(parentWatcher);

      expect(childWatcher0.native).toBe(CHILD0);
      expect(childWatcher1.native).toBe(CHILD1);

      expect(
        registry.tree.root.lookup(parts(parentDir)).when({
          parent: () => false,
          missing: () => false,
          children: () => true
        })
      ).toBe(true);

      expect(
        registry.tree.root.lookup(parts(childDir0)).when({
          parent: () => true,
          missing: () => false,
          children: () => false
        })
      ).toBe(true);

      expect(
        registry.tree.root.lookup(parts(childDir1)).when({
          parent: () => true,
          missing: () => false,
          children: () => false
        })
      ).toBe(true);
    });

    it('consolidates children when splitting a parent watcher', async function() {
      const CHILD0 = new MockNative('child0');
      const PARENT = new MockNative('parent');

      const parentDir = absolute('parent');
      const childDir0 = path.join(parentDir, 'child0');
      const childDir1 = path.join(parentDir, 'child0', 'child1');

      createNative = dir => {
        if (dir === parentDir) {
          return PARENT;
        } else if (dir === childDir0) {
          return CHILD0;
        } else {
          throw new Error(`Unexpected directory ${dir}`);
        }
      };

      const parentWatcher = new MockWatcher(parentDir);
      const childWatcher0 = new MockWatcher(childDir0);
      const childWatcher1 = new MockWatcher(childDir1);

      await registry.attach(parentWatcher);
      await Promise.all([
        registry.attach(childWatcher0),
        registry.attach(childWatcher1)
      ]);

      // All three watchers should share the parent watcher's native watcher.
      expect(parentWatcher.native).toBe(PARENT);
      expect(childWatcher0.native).toBe(PARENT);
      expect(childWatcher1.native).toBe(PARENT);

      // Stopping the parent should detach and create the child watchers. Both child watchers should
      // share the same native watcher.
      parentWatcher.stop();
      registry.detach(parentWatcher);

      expect(childWatcher0.native).toBe(CHILD0);
      expect(childWatcher1.native).toBe(CHILD0);

      expect(
        registry.tree.root.lookup(parts(parentDir)).when({
          parent: () => false,
          missing: () => false,
          children: () => true
        })
      ).toBe(true);

      expect(
        registry.tree.root.lookup(parts(childDir0)).when({
          parent: () => true,
          missing: () => false,
          children: () => false
        })
      ).toBe(true);

      expect(
        registry.tree.root.lookup(parts(childDir1)).when({
          parent: () => true,
          missing: () => false,
          children: () => false
        })
      ).toBe(true);
    });
  });
});
