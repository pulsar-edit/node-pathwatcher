
// This script tests the library for context safety by creating several
// instances on separate threads.
//
// This test is successful when the script exits gracefully. It fails when the
// script segfaults or runs indefinitely.
const spawnThread = require('./worker');

const NUM_WORKERS = 2;
const MAX_DURATION = 30 * 1000;

// Pick one of the workers to return earlier than the others.
let earlyReturn = null;
if (NUM_WORKERS > 1) {
  earlyReturn = Math.floor(Math.random() * NUM_WORKERS);
}

function bail () {
  console.error(`Script ran for more than ${MAX_DURATION / 1000} seconds; there's an open handle somewhere!`);
  process.exit(2);
}

// Wait to see if the script is still running MAX_DURATION milliseconds from
// now…
let failsafe = setTimeout(bail, MAX_DURATION);
// …but `unref` ourselves so that we're not the reason why the script keeps
// running!
failsafe.unref();

let promises = [];
let errors = [];
for (let i = 0; i < NUM_WORKERS; i++) {
  let promise = spawnThread(i, earlyReturn);

  // We want to prevent unhandled promise rejections. The errors from any
  // rejected promises will be collected and handled once all workers are done.
  promise.catch((err) =>  errors.push(err));
  promises.push(promise);
}

(async () => {
  await Promise.allSettled(promises);
  if (errors.length > 0) {
    console.error('Errors:');
    for (let error of errors) {
      console.error(`  ${error}`);
    }
    // Don't call `process.exit`; we want to be able to detect whether there
    // are open handles. If there aren't, the process will exit on its own;
    // if there are, then the failsafe will detect it and tell us about it.
    process.exitCode = 1;
  }
})();
