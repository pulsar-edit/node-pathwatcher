const path = require('path');
const fs = require('fs-plus');
const temp = require('temp');
const Directory = require('../src/directory');
const PathWatcher = require('../src/main');

describe('Directory', () => {
  let directory;
  let isCaseInsensitiveSpy;
  let didSpy = false;

  beforeEach(() => {
    // TODO: There's got to be a better way to do this.
    if (!didSpy) {
      isCaseInsensitiveSpy = spyOn(fs, 'isCaseInsensitive');
      didSpy = true;
    }
    directory = new Directory(path.join(__dirname, 'fixtures'));
  });

  afterEach(() => {
    PathWatcher.closeAllWatchers();
    isCaseInsensitiveSpy.and.callThrough();
  });

  it('normalizes the specified path', () => {
    let filePath = path.join(directory.path, 'abc', '..');
    let otherDirectory = new Directory(filePath);
    expect(otherDirectory.getBaseName()).toBe('fixtures');
    expect(otherDirectory.path.toLowerCase()).toBe(directory.path.toLowerCase());

    otherDirectory = new Directory(`${directory.path}${path.sep}`);
    expect(otherDirectory.getBaseName()).toBe('fixtures');
    expect(otherDirectory.path.toLowerCase()).toBe(directory.path.toLowerCase());

    otherDirectory = new Directory(path.sep);
    expect(otherDirectory.getBaseName()).toBe('');
    expect(otherDirectory.path).toBe(path.sep);
  });

  it('returns false from ::isFile', () => {
    expect(directory.isFile()).toBe(false);
  });

  it('returns true from ::isDirectory', () => {
    expect(directory.isDirectory()).toBe(true);
  });

  describe('::isSymbolicLink', () => {
    it('returns false for regular directories', () => {
      expect(directory.isSymbolicLink()).toBe(false);
    });

    it('returns true for symlinked directories', () => {
      let symbolicDirectory = new Directory(path.join(__dirname, 'fixtures'), true);
      expect(symbolicDirectory.isSymbolicLink()).toBe(true);
    });
  });

  describe('::exists', () => {
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
    });

    it('resolves to true for a directory that exists', async () => {
      directory = new Directory(tempDir);
      expect(
        await directory.exists()
      ).toBe(true);
    });

    it('resolves to false for a directory that doesn’t exist', async () => {
      directory = new Directory(path.join(tempDir, 'foo'));
      expect(
        await directory.exists()
      ).toBe(false);
    });
  });

  describe('::existsSync', () => {
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
    });

    it('returns true for a directory that exists', () => {
      directory = new Directory(tempDir);
      expect(
        directory.existsSync()
      ).toBe(true);
    });

    it('returns false for a directory that doesn’t exist', () => {
      directory = new Directory(path.join(tempDir, 'foo'));
      expect(
        directory.existsSync()
      ).toBe(false);
    });
  });

  describe('::create', () => {
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
    });

    it('creates the directory if the directory doesn’t exist', async () => {
      let directoryName = path.join(tempDir, 'subdir');
      expect(fs.existsSync(directoryName)).toBe(false);
      let nonExistentDirectory = new Directory(directoryName);

      let didCreate = await nonExistentDirectory.create(0o0600);

      expect(didCreate).toBe(true);
      expect(fs.existsSync(directoryName)).toBe(true);
      expect(fs.isDirectorySync(directoryName)).toBe(true);

      if (process.platform === 'win32') return;

      let rawMode = fs.statSync(directoryName).mode;
      mode = rawMode & 0o07777;
      expect(mode.toString(8)).toBe((0o0600).toString(8));
    });

    it('leaves an existing directory alone', async () => {
      let directoryName = path.join(tempDir, 'subdir');
      fs.mkdirSync(directoryName);
      let existingDirectory = new Directory(directoryName);

      let didCreate = await existingDirectory.create();

      expect(didCreate).toBe(false);
      expect(fs.existsSync(directoryName)).toBe(true);
      expect(fs.isDirectorySync(directoryName)).toBe(true);
    });

    it('creates parent directories if they don’t exist', async () => {
      let directoryName = path.join(tempDir, 'foo', 'bar', 'baz');
      expect(fs.existsSync(directoryName)).toBe(false);
      let nonExistentDirectory = new Directory(directoryName);

      let didCreate = await nonExistentDirectory.create(0o0600);

      expect(didCreate).toBe(true);
      expect(fs.existsSync(directoryName)).toBe(true);
      expect(fs.isDirectorySync(directoryName)).toBe(true);

      let parentName = path.join(tempDir, 'foo', 'bar');
      expect(fs.existsSync(parentName)).toBe(true);
      expect(fs.isDirectorySync(parentName)).toBe(true);
    });

    it('throws an error when called on a root directory that does not exist', async () => {
      spyOn(Directory.prototype, 'isRoot').and.returnValue(true);
      let directory = new Directory(path.join(tempDir, 'subdir'));

      expect(directory.isRoot()).toBe(true);

      try {
        await directory.create();
        expect(false).toBe(true);
      } catch (err) {
        expect(true).toBe(true);
      }

      expect(fs.existsSync(path.join(tempDir, 'subdir'))).toBe(false);
    });
  });

  describe('when the contents of the directory change on disk', () => {
    let temporaryFilePath;

    beforeEach(() => {
      temporaryFilePath = path.join(__dirname, 'fixtures', 'temporary');
      fs.removeSync(temporaryFilePath);
    });

    afterEach(() => {
      fs.removeSync(temporaryFilePath);
    });

    it('notifies ::onDidChange observers', async () => {
      let handler = jasmine.createSpy('changeHandler');
      directory.onDidChange(handler);
      fs.writeFileSync(temporaryFilePath, '');

      await condition(() => handler.calls.count() > 0);

      handler.calls.reset();
      fs.removeSync(temporaryFilePath);

      await condition(() => handler.calls.count() > 0);
    });
  });

  describe('when the directory unsubscribes from events', () => {
    let temporaryFilePath;

    beforeEach(() => {
      temporaryFilePath = path.join(__dirname, 'fixtures', 'temporary');
      if (fs.existsSync(temporaryFilePath)) {
        fs.removeSync(temporaryFilePath);
      }
    });

    afterEach(() => {
      if (fs.existsSync(temporaryFilePath)) {
        fs.removeSync(temporaryFilePath);
      }
    });

    it('no longer triggers events', async () => {
      console.log('\nABOUT TO WATCH FAILING FILE TEST');
      console.log('================================');

      let changeHandler = jasmine.createSpy('changeHandler', () => {
        console.log('[[[CHANGE HANDLER!]]]');
      });
      let subscription = directory.onDidChange(changeHandler);

      console.log('\nWAITING');
      console.log('=======');
      await wait(1000);

      fs.writeFileSync(temporaryFilePath, '');

      console.log('\nWROTE FILE');
      console.log('==========');
      await condition(() => changeHandler.calls.count() > 0);

      changeHandler.calls.reset();
      subscription.dispose();

      await wait(20);
      fs.removeSync(temporaryFilePath);
      await wait(20);
      expect(changeHandler.calls.count()).toBe(0);
    });
  });

  if (process.platform !== 'win32') {
    describe('on #darwin or #linux', () => {
      it('includes symlink information about entries', async () => {
        let entries = directory.getEntriesSync();
        for (let entry of entries) {
          let name = entry.getBaseName();
          if (name === 'symlink-to-dir' || name === 'symlink-to-file') {
            expect(entry.symlink).toBeTruthy();
          } else {
            expect(entry.symlink).toBeFalsy();
          }
        }

        let callback = jasmine.createSpy('getEntries');
        directory.getEntries(callback);

        await condition(() => callback.calls.count() === 1);

        entries = callback.calls.mostRecent().args[1];
        for (let entry of entries) {
          let name = entry.getBaseName();
          if (name === 'symlink-to-dir' || name === 'symlink-to-file') {
            expect(entry.symlink).toBeTruthy();
          } else {
            expect(entry.symlink).toBeFalsy();
          }
        }
      });
    });
  }

  describe('::relativize', () => {
    if (process.platform !== 'win32') {
      describe('on #darwin or #linux', () => {
        it('returns a relative path based on the directory’s path', () => {
          let absolutePath = directory.getPath();
          expect(directory.relativize(absolutePath)).toBe('');
          expect(directory.relativize(path.join(absolutePath, 'b'))).toBe('b')
          expect(directory.relativize(path.join(absolutePath, 'b/file.coffee'))).toBe('b/file.coffee');
          expect(directory.relativize(path.join(absolutePath, "file.coffee"))).toBe('file.coffee');
        });

        it('returns a relative path based on the directory’s symlinked source path', () => {
          let symlinkPath = path.join(__dirname, 'fixtures', 'symlink-to-dir');
          let symlinkDirectory = new Directory(symlinkPath);
          let realFilePath = require.resolve('./fixtures/dir/a');
          expect(symlinkDirectory.relativize(symlinkPath)).toBe('');
          expect(symlinkDirectory.relativize(realFilePath)).toBe('a');
        });

        it('returns the full path if the directory’s path is not a prefix of the path', () => {
          expect(directory.relativize('/not/relative')).toBe('/not/relative');
        });

        it('handles case-insensitive filesystems', () => {
          isCaseInsensitiveSpy.and.returnValue(true);
          let directoryPath = temp.mkdirSync('Mixed-case-directory-')
          let directory = new Directory(directoryPath)

          expect(directory.relativize(directoryPath.toUpperCase())).toBe("");
          expect(directory.relativize(path.join(directoryPath.toUpperCase(), "b"))).toBe("b");
          expect(directory.relativize(path.join(directoryPath.toUpperCase(), "B"))).toBe("B");
          expect(directory.relativize(path.join(directoryPath.toUpperCase(), "b/file.coffee"))).toBe("b/file.coffee");
          expect(directory.relativize(path.join(directoryPath.toUpperCase(), "file.coffee"))).toBe("file.coffee");

          expect(directory.relativize(directoryPath.toLowerCase())).toBe("");
          expect(directory.relativize(path.join(directoryPath.toLowerCase(), "b"))).toBe("b");
          expect(directory.relativize(path.join(directoryPath.toLowerCase(), "B"))).toBe("B");
          expect(directory.relativize(path.join(directoryPath.toLowerCase(), "b/file.coffee"))).toBe("b/file.coffee");
          expect(directory.relativize(path.join(directoryPath.toLowerCase(), "file.coffee"))).toBe("file.coffee");

          expect(directory.relativize(directoryPath)).toBe("");
          expect(directory.relativize(path.join(directoryPath, "b"))).toBe("b");
          expect(directory.relativize(path.join(directoryPath, "B"))).toBe("B");
          expect(directory.relativize(path.join(directoryPath, "b/file.coffee"))).toBe("b/file.coffee");
          expect(directory.relativize(path.join(directoryPath, "file.coffee"))).toBe("file.coffee");
        });
      });
    } // end #darwin/#linux

    if (process.platform === 'win32') {
      describe('on #win32', () => {
        it('returns a relative path based on the directory’s path', () => {
          let absolutePath = directory.getPath();
          expect(directory.relativize(absolutePath)).toBe('');
          expect(directory.relativize(path.join(absolutePath, 'b'))).toBe('b')
          expect(directory.relativize(path.join(absolutePath, 'b/file.coffee'))).toBe('b\\file.coffee');
          expect(directory.relativize(path.join(absolutePath, "file.coffee"))).toBe('file.coffee');
        });

        it('returns the full path if the directory’s path is not a prefix of the path', () => {
          expect(directory.relativize('/not/relative')).toBe("\\not\\relative");
        });
      });
    }
  });

  describe('::resolve', () => {
    describe('when passed an absolute or relative path', () => {
      it('returns an absolute path based on the directory’s path', () => {
        let absolutePath = require.resolve('./fixtures/dir/a');
        expect(directory.resolve('dir/a')).toBe(absolutePath);
        expect(directory.resolve(absolutePath + '/../a')).toBe(absolutePath)
        expect(directory.resolve('dir/a/../a')).toBe(absolutePath)
        expect(directory.resolve()).toBeUndefined()
      });
    });

    describe('when passed a URI with a scheme', () => {
      it('does not modify URIs that begin with a scheme', () => {
        expect(directory.resolve('http://zombo.com')).toBe('http://zombo.com');
      });
    });
  });

  describe('::contains', () => {
    it('returns true if the path is a child of the directory’s path', () => {
      let absolutePath = directory.getPath();

      expect(directory.contains(path.join(absolutePath))).toBe(false);
      expect(directory.contains(path.join(absolutePath, "b"))).toBe(true);
      expect(directory.contains(path.join(absolutePath, "b", "file.coffee"))).toBe(true);
      expect(directory.contains(path.join(absolutePath, "file.coffee"))).toBe(true);
    });

    it('returns false if the directory’s path is not a prefix of the path', () => {
      expect(directory.contains('/not/relative')).toBe(false);
    });

    it('handles case-insensitive filesystems', () => {
      isCaseInsensitiveSpy.and.returnValue(true);
      let directoryPath = temp.mkdirSync('Mixed-case-directory-')
      let directory = new Directory(directoryPath)

      expect(directory.contains(directoryPath.toUpperCase())).toBe(false);
      expect(directory.contains(path.join(directoryPath.toUpperCase(), "b"))).toBe(true);
      expect(directory.contains(path.join(directoryPath.toUpperCase(), "B"))).toBe(true);
      expect(directory.contains(path.join(directoryPath.toUpperCase(), "b", "file.coffee"))).toBe(true);
      expect(directory.contains(path.join(directoryPath.toUpperCase(), "file.coffee"))).toBe(true);

      expect(directory.contains(directoryPath.toLowerCase())).toBe(false);
      expect(directory.contains(path.join(directoryPath.toLowerCase(), "b"))).toBe(true);
      expect(directory.contains(path.join(directoryPath.toLowerCase(), "B"))).toBe(true);
      expect(directory.contains(path.join(directoryPath.toLowerCase(), "b", "file.coffee"))).toBe(true);
      expect(directory.contains(path.join(directoryPath.toLowerCase(), "file.coffee"))).toBe(true);

      expect(directory.contains(directoryPath)).toBe(false);
      expect(directory.contains(path.join(directoryPath, "b"))).toBe(true);
      expect(directory.contains(path.join(directoryPath, "B"))).toBe(true);
      expect(directory.contains(path.join(directoryPath, "b", "file.coffee"))).toBe(true);
      expect(directory.contains(path.join(directoryPath, "file.coffee"))).toBe(true);
    });

    if (process.platform !== 'win32') {
      describe('on #darwin or #linux', () => {
        it('returns true if the path is a child of the directory’s symlinked source path', () => {
          let symlinkPath = path.join(__dirname, 'fixtures', 'symlink-to-dir');
          let symlinkDirectory = new Directory(symlinkPath);
          let realFilePath = require.resolve('./fixtures/dir/a');
          expect(symlinkDirectory.contains(realFilePath)).toBe(true);
        });
      });
    }

    describe('traversal', () => {
      beforeEach(() => {
        directory = new Directory(path.join(__dirname, 'fixtures', 'dir'));
      });

      function fixturePath (...parts) {
        return path.join(__dirname, 'fixtures', ...parts);
      }

      describe('::getFile', () => {
        it('returns a File within this directory', () => {
          let f = directory.getFile('a');
          expect(f.isFile()).toBe(true);
          expect(f.getRealPathSync()).toBe(fixturePath('dir', 'a'));
        });

        it('can descend more than one directory at a time', () => {
          let f = directory.getFile('subdir', 'b');
          expect(f.isFile()).toBe(true);
          expect(f.getRealPathSync()).toBe(fixturePath('dir', 'subdir', 'b'));
        });

        it('doesn’t have to exist', () => {
          let f = directory.getFile('the-silver-bullet');
          expect(f.isFile()).toBe(true);
          expect(f.existsSync()).toBe(false);
        });
      });

      describe('::getSubdirectory', () => {
        it('returns a subdirectory within this directory', () => {
          let d = directory.getSubdirectory('subdir');
          expect(d.isDirectory()).toBe(true);
          expect(d.getRealPathSync()).toBe(fixturePath('dir', 'subdir'));
        });

        it('can descend more than one directory at a time', () => {
          let d = directory.getSubdirectory('subdir', 'subsubdir');
          expect(d.isDirectory()).toBe(true);
          expect(d.getRealPathSync()).toBe(fixturePath('dir', 'subdir', 'subsubdir'));
        });

        it('doesn’t have to exist', () => {
          let d = directory.getSubdirectory("why-would-you-call-a-directory-this-come-on-now");
          expect(d.isDirectory()).toBe(true);
        });
      });

      describe('::getParent', () => {
        it('returns the parent Directory', () => {
          let d = directory.getParent();
          expect(d.isDirectory()).toBe(true);
          expect(d.getRealPathSync()).toBe(fixturePath());
        });
      });

      describe('::isRoot', () => {
        it('returns false if the Directory isn’t the root', () => {
          expect(directory.isRoot()).toBe(false);
        });

        it('returns true if the Directory is the root', () => {
          let current = directory;
          let previous = null;
          while (current.getPath() !== previous?.getPath()) {
            previous = current;
            current = current.getParent();
          }
          expect(current.isRoot()).toBe(true);
        });
      });
    });
  });
});
