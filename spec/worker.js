const {
  Worker, isMainThread, parentPort, workerData
} = require('node:worker_threads');
const {
  performance
} = require('node:perf_hooks');

const temp = require('temp');
const fs = require('fs');
const path = require('path');
const util = require('util');

const wait = util.promisify(setTimeout);

const EXPECTED_CALL_COUNT = 3;

if (isMainThread) {
  module.exports = function spawnThread(index, indexOfEarlyReturn) {
    let id = index + 1;
    return new Promise(async (resolve, reject) => {
      console.log('Spawning worker:', id);
      const worker = new Worker(__filename, {
        workerData: { id, earlyReturn: indexOfEarlyReturn === null ? false : id === (indexOfEarlyReturn + 1) },
      });
      worker.on('message', async (msg) => {
        console.log('[parent] Worker', id, 'reported call count:', msg);
        await wait(1000);
        let expected = id === indexOfEarlyReturn + 1 ? (EXPECTED_CALL_COUNT - 1) : EXPECTED_CALL_COUNT;
        let passes = msg >= expected;
        if (passes) {
          console.log(`Worker ${id} passed!`);
          resolve();
        } else {
          reject(`Not enough calls on worker ${id}! Expected: ${expected} Actual: ${msg}`);
        }
      });
      worker.on('error', (err) => {
        console.error(`ERROR IN WORKER: ${id}`);
        console.error(err);
        reject(err);
      });
      worker.on('exit', (code) => {
        if (code !== 0) {
          console.log(`Worker ${id} stopped with exit code ${code}`);
          reject();
        } else {
          console.log(`Worker ${id} exited gracefully`);
          // resolve();
        }
      });
    });
  };
} else {
  let tempDir = temp.mkdirSync('node-pathwatcher-directory');
  const tempFile = path.join(tempDir, 'file');

  const { watch, closeAllWatchers } = require('../src/main');

  console.log('NEW WORKER:', workerData);

  class Scheduler {
    constructor(id, pathToWatch) {
      this.id = id;
      this.path = pathToWatch;
      this.callCount = 0;
    }

    async start () {
      console.log('Scheduler', this.id, 'starting at', performance.now(), 'watching path:', this.path);
      this.watcher = watch(this.path, (event) => {
        this.callCount++;
        console.warn('\x1b[33m%s\x1b[0m', 'PathWatcher event for worker', this.id, event)
        console.log('callCount is now:', this.callCount);
      });
      console.log('Scheduler', this.id, 'ready at:', performance.now());
    }

    stop () {
      this.watcher?.close();
    }
  }

  (async () => {
    console.log('Worker', workerData.id, 'creating file:', tempFile);
    fs.writeFileSync(tempFile, '');
    await wait(500);
    const scheduler = new Scheduler(workerData.id, tempFile);
    scheduler.start();
    await wait(2000);

    console.log('Worker', scheduler.id, 'changing file:', tempFile);
    // Should generate one or two events:
    fs.writeFileSync(tempFile, 'changed');
    await wait(1000);
    console.log('Worker', scheduler.id, 'changing file again:', tempFile);
    // Should generate another event:
    fs.writeFileSync(tempFile, 'changed again');
    await wait(500);
    if (workerData.earlyReturn) {
      console.log('Worker', scheduler.id, 'returning early!');
    } else {
      await wait(500);
      // Should generate a final event (total count 3 or 4):
      console.log('Worker', scheduler.id, 'deleting file:', tempFile);
      fs.rmSync(tempFile);
      await wait(1000);

      await wait(Math.random() * 2000);
    }

    parentPort.postMessage(scheduler.callCount);

    closeAllWatchers();
    console.log('Worker', scheduler.id, 'closing');
    // process.exit(0);
  })();
}
