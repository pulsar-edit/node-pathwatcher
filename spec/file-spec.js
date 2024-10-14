const path = require('path');
const fs = require('fs-plus');
const temp = require('temp');
const File = require('../src/file');
const PathWatcher = require('../src/main');

describe('File', () => {
  let filePath;
  let file;

  beforeEach(() => {
    // Don't put in /tmp because /tmp symlinks to /private/tmp and screws up
    // the rename test
    filePath = path.join(__dirname, 'fixtures', 'file-test.txt');
    fs.removeSync(filePath);
    fs.writeFileSync(filePath, 'this is old!');
    file = new File(filePath);
  });

  afterEach(async () => {
    file.unsubscribeFromNativeChangeEvents();
    fs.removeSync(filePath);
    await PathWatcher.closeAllWatchers();
  });

  it('normalizes the specified path', () => {
    let fileName = path.join(__dirname, 'fixtures', 'abc', '..', 'file-test.txt');
    let f = new File(fileName);

    expect(f.getBaseName()).toBe('file-test.txt');
    expect(f.path.toLowerCase()).toBe(file.path.toLowerCase());
  });

  it('returns true from isFile()', () => {
    expect(file.isFile()).toBe(true);
  });

  it('returns false from isDirectory()', () => {
    expect(file.isDirectory()).toBe(false);
  });

  describe('::isSymbolicLink', () => {
    it('returns false for regular files', () => {
      expect(file.isSymbolicLink()).toBe(false);
    });

    it('returns true for symlinked files', () => {
      let symbolicFile = new File(filePath, true);
      expect(symbolicFile.isSymbolicLink()).toBe(true);
    });
  });

  describe('::getDigestSync', () => {
    it('computes and returns the SHA-1 digest and caches it', () => {
      filePath = path.join(temp.mkdirSync('node-pathwatcher-directory'), 'file.txt');
      fs.writeFileSync(filePath, '');
      file = new File(filePath);
      spyOn(file, 'readSync').and.callThrough();

      expect(
        file.getDigestSync()
      ).toBe('da39a3ee5e6b4b0d3255bfef95601890afd80709');
      expect(file.readSync.calls.count()).toBe(1);
      expect(
        file.getDigestSync()
      ).toBe('da39a3ee5e6b4b0d3255bfef95601890afd80709');
      expect(file.readSync.calls.count()).toBe(1);

      file.writeSync('x');

      expect(
        file.getDigestSync()
      ).toBe('11f6ad8ec52a2984abaafd7c3b516503785c2072');
      expect(file.readSync.calls.count()).toBe(1);
      expect(
        file.getDigestSync()
      ).toBe('11f6ad8ec52a2984abaafd7c3b516503785c2072');
      expect(file.readSync.calls.count()).toBe(1);
    });
  });

  describe('::create()', () => {
    let nonExistentFile;
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
    });

    afterEach(() => {
      nonExistentFile?.unsubscribeFromNativeChangeEvents();
      if (nonExistentFile?.getPath()) {
        fs.removeSync(nonExistentFile?.getPath());
      }
    });

    it('creates file in directory if file does not exist', async () => {
      fileName = path.join(tempDir, 'file.txt');
      expect(fs.existsSync(fileName)).toBe(false);
      nonExistentFile = new File(fileName);

      let didCreate = await nonExistentFile.create();
      expect(didCreate).toBe(true);
      expect(fs.existsSync(fileName)).toBe(true);
      expect(fs.isFileSync(fileName)).toBe(true);
      expect(fs.readFileSync(fileName).toString()).toBe('');
    });

    it('leaves existing file alone if it exists', async () => {
      fileName = path.join(tempDir, 'file.txt');
      fs.writeFileSync(fileName, 'foo');
      let existingFile = new File(fileName);

      let didCreate = await existingFile.create();
      expect(didCreate).toBe(false);
      expect(fs.existsSync(fileName)).toBe(true);
      expect(fs.isFileSync(fileName)).toBe(true);
      expect(fs.readFileSync(fileName).toString()).toBe('foo');
    });

    it('creates parent directories and files if they do not exist', async () => {
      fileName = path.join(tempDir, 'foo', 'bar', 'file.txt');
      expect(fs.existsSync(fileName)).toBe(false);
      nonExistentFile = new File(fileName);

      let didCreate = await nonExistentFile.create();
      expect(didCreate).toBe(true);
      expect(fs.existsSync(fileName)).toBe(true);
      expect(fs.isFileSync(fileName)).toBe(true);

      let parentName = path.join(tempDir, 'foo', 'bar');
      expect(fs.existsSync(parentName)).toBe(true);
      expect(fs.isDirectorySync(parentName)).toBe(true);
    });
  });

  describe('when the file has not been read', () => {
    describe('when the contents of the file change', () => {
      it('notifies ::onDidChange observers', async () => {
        let spy = jasmine.createSpy('changeHandler');
        let [promise, changeHandler] = makePromiseCallback(spy)
        file.onDidChange(changeHandler);
        fs.writeFileSync(file.getPath(), 'this is new!');
        await promise;
        expect(spy.calls.count()).toBe(1);
      });
    });

    describe('when the contents of the file are deleted', () => {
      it('notifies ::onDidChange observers', async () => {
        let spy = jasmine.createSpy('changeHandler');
        let [promise, changeHandler] = makePromiseCallback(spy)
        file.onDidChange(changeHandler);
        fs.writeFileSync(file.getPath(), '');
        await promise;
        expect(spy.calls.count()).toBe(1);
      });
    });
  });

  if (process.platform === 'darwin') {
    describe('when the file has already been read', () => {
      beforeEach(() => file.readSync());

      describe('when the contents of the file change', () => {
        it('notifies ::onDidChange observers', async () => {
          let lastText = null;

          file.onDidChange(async () => {
            lastText = await file.read();
          });

          fs.writeFileSync(file.getPath(), 'this is new!');
          await condition(() => lastText === 'this is new!');
          expect(file.readSync()).toBe('this is new!');

          fs.writeFileSync(file.getPath(), 'this is newer!');
          await condition(() => lastText === 'this is newer!');
          expect(file.readSync()).toBe('this is newer!');
        });
      })
    });

    describe('when the file is deleted', () => {
      it('notifies ::onDidDelete observers', async () => {
        let deleteHandler = jasmine.createSpy('deleteHandler');
        file.onDidDelete(deleteHandler);
        fs.removeSync(file.getPath());

        await condition(() => deleteHandler.calls.count() > 0);
      });
    });

    describe('when a file is moved (via the filesystem)', () => {
      let newPath = null;

      beforeEach(() => {
        newPath = path.join(path.dirname(filePath), 'file-was-moved-test.txt');
      });

      afterEach(async () => {
        if (!fs.existsSync(newPath)) return;
        fs.removeSync(newPath);
        let deleteHandler = jasmine.createSpy('deleteHandler');
        file.onDidDelete(deleteHandler);
        await condition(() => deleteHandler.calls.count() > 0, 30000);
      });

      it('updates its path', async () => {
        let moveHandler = jasmine.createSpy('moveHandler');
        file.onDidRename(moveHandler);

        fs.moveSync(filePath, newPath);

        await condition(() => moveHandler.calls.count() > 0, 30000);

        expect(file.getPath()).toBe(newPath);
      });

      it('maintains ::onDidChange observers that were subscribed on the previous path', async () => {
        let moveHandler = jasmine.createSpy('moveHandler');
        file.onDidRename(moveHandler);

        let changeHandler = jasmine.createSpy('changeHandler');
        file.onDidChange(changeHandler);

        fs.moveSync(filePath, newPath);

        await condition(() => moveHandler.calls.count() > 0);

        expect(changeHandler).not.toHaveBeenCalled();
        fs.writeFileSync(file.getPath(), 'this is new!');

        await condition(() => changeHandler.calls.count() > 0);
      });
    });

    describe('when a file is deleted and then recreated within a small amount of time (git sometimes does this)', () => {
      it('triggers a contents-changed event if the contents change', async () => {
        let changeHandler = jasmine.createSpy('file changed');
        let deleteHandler = jasmine.createSpy('file deleted');
        file.onDidChange(changeHandler);
        file.onDidDelete(deleteHandler);

        expect(changeHandler).not.toHaveBeenCalled();
        fs.removeSync(filePath);
        expect(changeHandler).not.toHaveBeenCalled();

        await wait(20);

        fs.writeFileSync(filePath, 'HE HAS RISEN!');
        expect(changeHandler).not.toHaveBeenCalled();

        await condition(() => changeHandler.calls.count() === 1);

        expect(deleteHandler).not.toHaveBeenCalled();
        fs.writeFileSync(filePath, 'Hallelujah!');
        changeHandler.calls.reset();

        await condition(() => changeHandler.calls.count() > 0);
      });
    });

    describe('when a file is moved to the trash', () => {
      const MACOS_TRASH_DIR = path.join(process.env.HOME, '.Trash');
      let expectedTrashPath = path.join(MACOS_TRASH_DIR, 'file-was-moved-to-trash.txt');

      it('triggers a delete event', async () => {
        let deleteHandler = jasmine.createSpy('deleteHandler');
        file.onDidDelete(deleteHandler);

        fs.moveSync(filePath, expectedTrashPath);

        await condition(() => deleteHandler.calls.count() > 0);

        if (fs.existsSync(expectedTrashPath)) {
          fs.removeSync(expectedTrashPath);
        }
      });
    });

    // NOTE: We used to have tests for the ` onWillThrowWatchError` callback,
    // but that callback was made a no-op many years ago. This seems to have
    // been done for performance reasons, since there is no practical way to
    // detect errors of the sort that were thrown via `onWillThrowWatchError`
    // without re-reading the entire file whenever a change is detected.


  } // end darwin-only tests

  describe('::getRealPathSync', () => {
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
      fs.writeFileSync(path.join(tempDir, 'file'), '');
      fs.writeFileSync(path.join(tempDir, 'file2'), '');
    });

    it('returns the resolved path to the file', () => {
      let tempFile = new File(path.join(tempDir, 'file'));
      expect(
        tempFile.getRealPathSync()
      ).toBe(
        fs.realpathSync(path.join(tempDir, 'file'))
      );

      tempFile.setPath(path.join(tempDir, 'file2'));
      expect(
        tempFile.getRealPathSync()
      ).toBe(
        fs.realpathSync(path.join(tempDir, 'file2'))
      );
    });
  });

  describe('::exists', () => {
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
      fs.writeFileSync(path.join(tempDir, 'file'), '');
    });

    it('does actually exist', async () => {
      let existingFile = new File(path.join(tempDir, 'file'));
      let exists = await existingFile.exists();
      expect(exists).toBe(true);
    });

    it('doesn’t exist', async () => {
      let nonExistentFile = new File(path.join(tempDir, 'not_file'));
      let exists = await nonExistentFile.exists();
      expect(exists).toBe(false);
    });
  });

  describe('::getRealPath', () => {
    let tempDir;

    beforeEach(() => {
      tempDir = temp.mkdirSync('node-pathwatcher-directory');
      fs.writeFileSync(path.join(tempDir, 'file'), '');
      fs.writeFileSync(path.join(tempDir, 'file2'), '');
    });

    it('returns the resolved path to the file', async () => {
      let tempFile = new File(path.join(tempDir, 'file'));
      expect(
        await tempFile.getRealPath()
      ).toBe(
        fs.realpathSync(path.join(tempDir, 'file'))
      );
    });

    it('returns the resolved path to the file after a call to setPath', async () => {
      let tempFile = new File(path.join(tempDir, 'file'));
      tempFile.setPath(path.join(tempDir, 'file2'));
      expect(
        await tempFile.getRealPath()
      ).toBe(
        fs.realpathSync(path.join(tempDir, 'file2'))
      );
    });

    if (process.platform !== 'win32') {
      describe('on #darwin and #linux', () => {
        it('returns the target path for symlinks', async () => {
          fs.symlinkSync(
            path.join(tempDir, 'file2'),
            path.join(tempDir, 'file3')
          );
          let tempFile = new File(path.join(tempDir, 'file3'));
          expect(
            await tempFile.getRealPath()
          ).toBe(
            fs.realpathSync(path.join(tempDir, 'file2'))
          );
        });
      });
    }
  });

  describe('::getParent', () => {
    it('gets the parent directory', () => {
      expect(
        file.getParent().getRealPathSync()
      ).toBe(
        path.join(__dirname, 'fixtures')
      );
    });
  });

  describe('encoding', () => {
    it('should be utf8 by default', () => {
      expect(file.getEncoding()).toBe('utf8');
    });

    it('should be settable', () => {
      file.setEncoding('cp1252');
      expect(file.getEncoding()).toBe('cp1252');
    });

    it('throws an exception when assigning an invalid encoding', () => {
      expect(() => {
        file.setEncoding('utf-8-bom');
      }).toThrow();
    });
  })

  describe('::createReadStream', () => {
    it('returns a stream to read the file', async () => {
      let stream = file.createReadStream();
      let ended = false;
      let content = [];

      stream.on('data', (chunk) => content.push(chunk));
      stream.on('end', () => ended = true);

      await condition(() => ended);

      expect(content.join('')).toEqual('this is old!');
    });

    it('honors the specified encoding', async () => {
      let unicodeText = 'ё';
      let unicodeBytes = Buffer.from('\x51\x04') // 'ё'

      fs.writeFileSync(file.getPath(), unicodeBytes);

      file.setEncoding('utf16le');

      let stream = file.createReadStream();
      let ended = false;
      let content = [];

      stream.on('data', (chunk) => content.push(chunk));
      stream.on('end', () => ended = true);

      await condition(() => ended);
      expect(content.join('')).toEqual(unicodeText);
    });
  });

  describe('::createWriteStream', () => {
    it('returns a stream to read the file', async () => {
      let unicodeText = 'ё';
      let unicodeBytes = Buffer.from('\x51\x04') // 'ё'

      file.setEncoding('utf16le');

      let stream = file.createWriteStream();
      let ended = false;
      stream.on('finish', () => ended = true);

      stream.end(unicodeText);
      await condition(() => ended);
      expect(fs.statSync(file.getPath()).size).toBe(2);
      let content = fs.readFileSync(file.getPath()).toString('ascii');
      expect(content).toBe(unicodeBytes.toString('ascii'));
    });
  });

  describe('encoding support', () => {
    let unicodeText;
    let unicodeBytes;

    beforeEach(() => {
      unicodeText = 'ё';
      unicodeBytes = Buffer.from('\x51\x04') // 'ё'
    });

    it('should read a file in UTF-16', async () => {
      fs.writeFileSync(file.getPath(), unicodeBytes);
      file.setEncoding('utf16le');

      let contents = await file.read();
      expect(contents).toBe(unicodeText);
    });

    it('should readSync a file in UTF-16', () => {
      fs.writeFileSync(file.getPath(), unicodeBytes);
      file.setEncoding('utf16le');
      expect(file.readSync()).toBe(unicodeText);
    });

    it('should write a file in UTF-16', async () => {
      file.setEncoding('utf16le');
      await file.write(unicodeText);
      expect(fs.statSync(file.getPath()).size).toBe(2);
      let content = fs.readFileSync(file.getPath()).toString('ascii');
      expect(content).toBe(unicodeBytes.toString('ascii'));
    });

    it('should writeSync a file in UTF-16', () => {
      file.setEncoding('utf16le');
      file.writeSync(unicodeText);
      expect(fs.statSync(file.getPath()).size).toBe(2);
      let content = fs.readFileSync(file.getPath()).toString('ascii');
      expect(content).toBe(unicodeBytes.toString('ascii'));
    });
  });

  describe('reading a nonexistent file', () => {
    it('should return null', async () => {
      file = new File('not_existing.txt');
      expect(
        await file.read()
      ).toBe(null);
    });
  });

  describe('::writeSync', () => {
    it('emits did-change event', async () => {
      let handler = jasmine.createSpy('write handler');
      file.onDidChange(handler);
      file.writeSync('ok');
      await condition(() => handler.calls.count() > 0);
    });
  });
});
