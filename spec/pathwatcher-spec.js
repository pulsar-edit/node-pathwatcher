const PathWatcher = require('../src/main');
const fs = require('fs-plus');
const path = require('path');
const temp = require('temp');

temp.track();

function EMPTY() {}

describe('PathWatcher', () => {
  let tempDir = temp.mkdirSync('node-pathwatcher-directory');
  let tempFile = path.join(tempDir, 'file');

  beforeEach(() => fs.writeFileSync(tempFile, ''));
  afterEach(async () => {
    PathWatcher.closeAllWatchers();
    // Allow time in between each spec so that file-watchers have a chance to
    // clean up.
    await wait(100);
  });

  describe('getWatchedPaths', () => {
    it('returns an array of all watched paths', () => {
      let realTempFilePath = fs.realpathSync(tempFile);
      let expectedWatchPath = path.dirname(realTempFilePath);

      expect(PathWatcher.getWatchedPaths()).toEqual([]);

      // Watchers watch the parent directory.
      let watcher1 = PathWatcher.watch(tempFile, EMPTY);
      expect(PathWatcher.getWatchedPaths()).toEqual([expectedWatchPath]);

      // Second watcher is a sibling of the first and should be able to reuse
      // the existing watcher.
      let watcher2 = PathWatcher.watch(tempFile, EMPTY);
      expect(PathWatcher.getWatchedPaths()).toEqual([expectedWatchPath]);
      watcher1.close();

      // Native watcher won't close yet because it knows it had two listeners.
      expect(PathWatcher.getWatchedPaths()).toEqual([expectedWatchPath]);
      watcher2.close();

      expect(PathWatcher.getWatchedPaths()).toEqual([]);
    });
  });

  describe('closeAllWatchers', () => {
    it('closes all watched paths', () => {
      let realTempFilePath = fs.realpathSync(tempFile);
      let expectedWatchPath = path.dirname(realTempFilePath);
      expect(PathWatcher.getWatchedPaths()).toEqual([]);
      PathWatcher.watch(tempFile, EMPTY);
      expect(PathWatcher.getWatchedPaths()).toEqual([expectedWatchPath]);
      PathWatcher.closeAllWatchers();
      expect(PathWatcher.getWatchedPaths()).toEqual([]);
    });
  });

  // The purpose of this `describe` block is to ensure that our custom FSEvent
  // implementation on macOS agrees with the built-in `efsw` implementations on
  // Windows and Linux.
  //
  // Notably: in order to behave predictably, the FSEvent watcher should not
  // trigger a watcher on `/foo/bar/baz` when `/foo/bar/baz` itself is deleted,
  // since that’s how the other watcher implementations behave.
  describe('when a watched directory is deleted', () => {
    let subDir;
    beforeEach(() => {
      subDir = path.join(tempDir, 'subdir');
      if (!fs.existsSync(subDir)) {
        fs.mkdirSync(subDir);
      }
    });

    afterEach(() => {
      if (subDir && fs.existsSync(subDir)) {
        fs.rmSync(subDir, { recursive: true });
      }
    });

    it('does not trigger the callback', async () => {
      // This test proves that `efsw` does not detect when a directory is
      // deleted if you are watching that exact path. Our custom macOS
      // implementation should behave the same way for predictability.
      let innerSpy = jasmine.createSpy('innerSpy');
      PathWatcher.watch(subDir, innerSpy);
      await wait(20);
      fs.rmSync(subDir, { recursive: true });
      await wait(200);
      expect(innerSpy).not.toHaveBeenCalled();
    });

    it('triggers a callback on the directory’s parent if the parent is being watched', async () => {
      // We can detect the directory’s deletion if we watch its parent
      // directory.
      //
      // This test proves that, but it also proves that a watcher on the
      // deleted directory is still not invoked in this scenario. This was a
      // specific scenario I tried to handle and this test proves that said
      // workaround is not present.
      let outerSpy = jasmine.createSpy('outerSpy');
      let innerSpy = jasmine.createSpy('innerSpy');
      PathWatcher.watch(tempDir, outerSpy);
      PathWatcher.watch(subDir, innerSpy);
      await wait(20);
      fs.rmSync(subDir, { recursive: true });
      await condition(() => outerSpy.calls.count() > 0);
      await wait(200);
      expect(innerSpy).not.toHaveBeenCalled();
    });
  });

  describe('when a watched path is changed', () => {
    it('fires the callback with the event type and empty path', async () => {
      let eventType;
      let eventPath;

      PathWatcher.watch(tempFile, (type, path) => {
        eventType = type;
        eventPath = path;
      });

      fs.writeFileSync(tempFile, 'changed');

      await condition(() => !!eventType);

      expect(eventType).toBe('change');
      expect(eventPath).toBe('');
    });
  });


  if (process.platform !== 'linux') {
    describe('when a watched path is renamed #darwin #win32', () => {
      it('fires the callback with the event type and new path and watches the new path', async () => {
        let eventType;
        let eventPath;

        let watcher = PathWatcher.watch(tempFile, (type, path) => {
          eventType = type;
          eventPath = path;
        });

        let tempRenamed = path.join(tempDir, 'renamed');
        fs.renameSync(tempFile, tempRenamed);

        await condition(() => !!eventType);

        expect(eventType).toBe('rename');
        expect(fs.realpathSync(eventPath)).toBe(fs.realpathSync(tempRenamed));
        expect(PathWatcher.getWatchedPaths()).toEqual([watcher.native.path]);
      });
    });

    describe('when a watched path is deleted #darwin #win32', () => {
      it('fires the callback with the event type and null path', async () => {
        let deleted = false;

        PathWatcher.watch(tempFile, (type, path) => {
          if (type === 'delete' && path === null) {
            deleted = true;
          }
        });

        fs.unlinkSync(tempFile);
        await condition(() => deleted);
      });
    });
  }

  describe('when a file under a watched directory is deleted', () => {
    it('fires the callback with the change event and empty path', async () => {
      let fileUnderDir = path.join(tempDir, 'file');
      fs.writeFileSync(fileUnderDir, '');
      let done = false;

      PathWatcher.watch(tempDir, (type, path) => {
        expect(type).toBe('change');
        expect(path).toBe('');
        done = true;
      });

      fs.writeFileSync(fileUnderDir, 'what');
      await wait(200);
      fs.unlinkSync(fileUnderDir);
      await condition(() => done);
    });
  });

  describe('when a new file is created under a watched directory', () => {
    it('fires the callback with the change event and empty path', async () => {
      let newFile = path.join(tempDir, 'file');
      if (fs.existsSync(newFile)) {
        fs.unlinkSync(newFile);
      }
      let done = false;
      PathWatcher.watch(tempDir, (type, path) => {
        if (fs.existsSync(newFile)) {
          fs.unlinkSync(newFile);
        }
        expect(type).toBe('change');
        expect(path).toBe('');
        done = true;
      });

      fs.writeFileSync(newFile, 'x');
      await condition(() => done);
    });
  });

  describe('when a file under a watched directory is moved', () => {
    it('fires the callback with the change event and empty path', async () => {

      let newName = path.join(tempDir, 'file2');
      let done = false;
      PathWatcher.watch(tempDir, (type, path) => {
        expect(type).toBe('change');
        expect(path).toBe('');
        done = true;
      });

      fs.renameSync(tempFile, newName);
      await condition(() => done);
    });
  });

  describe('when an exception is thrown in the closed watcher’s callback', () => {
    it('does not crash', async () => {
      let done = false;
      let watcher = PathWatcher.watch(tempFile, (_type, _path) => {
        watcher.close();
        try {
          throw new Error('test');
        } catch (e) {
          done = true;
        }
      });

      fs.writeFileSync(tempFile, 'changed');
      await condition(() => done);
    });
  });

  if (process.platform !== 'win32') {
    describe('when watching a file that does not exist', () => {
      it('throws an error with a code #darwin #linux', () => {
        let doesNotExist = path.join(tempDir, 'does-not-exist');
        let watcher;
        try {
          watcher = PathWatcher.watch(doesNotExist, EMPTY);
        } catch (error) {
          expect(error.message).toBe('Unable to watch path');
          expect(error.code).toBe('ENOENT');
        }
        expect(watcher).toBe(undefined); // (ensure it threw)
      });
    });
  }

  describe('when watching multiple files under the same directory', () => {
    it('fires the callbacks when both of the files are modified', async () => {
      let called = 0;
      let tempFile2 = path.join(tempDir, 'file2');
      fs.writeFileSync(tempFile2, '');
      PathWatcher.watch(tempFile, () => called |= 1);
      PathWatcher.watch(tempFile2, () => called |= 2);
      fs.writeFileSync(tempFile, 'changed');
      fs.writeFileSync(tempFile2, 'changed');
      await condition(() => called === 3);
    });

    if (process.platform === 'win32') {
      it('shares the same handle watcher between the two files on #win32', () => {
        let tempFile2 = path.join(tempDir, 'file2');
        fs.writeFileSync(tempFile2, '');
        let watcher1 = PathWatcher.watch(tempFile, EMPTY);
        let watcher2 = PathWatcher.watch(tempFile2, EMPTY);
        expect(watcher1.native).toBe(watcher2.native);
      });
    }
  });

  describe('when a file is unwatched', () => {
    it('does not lock the file system tree', () => {
      let nested1 = path.join(tempDir, 'nested1');
      let nested2 = path.join(nested1, 'nested2');
      let nested3 = path.join(nested2, 'nested3');
      fs.mkdirSync(nested1);
      fs.mkdirSync(nested2);
      fs.writeFileSync(nested3, '');

      let subscription1 = PathWatcher.watch(nested1, EMPTY);
      let subscription2 = PathWatcher.watch(nested2, EMPTY);
      let subscription3 = PathWatcher.watch(nested3, EMPTY);

      subscription1.close();
      subscription2.close();
      subscription3.close();

      fs.unlinkSync(nested3);
      fs.rmdirSync(nested2);
      fs.rmdirSync(nested1);
    });
  });
});
