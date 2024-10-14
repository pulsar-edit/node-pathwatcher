// This script exists so that we can add some extra logic that runs once the
// suite has finished. This is necessary so that we can detect when the task
// fails to finish (perhaps because of an open handle somewhere) and prevent
// it from running in CI for hours while doing nothing.
const Path = require('path');
const Jasmine = require('jasmine');
const jasmine = new Jasmine();

// Load the config from the typical place…
const CONFIG = require(Path.resolve(__dirname, 'support', 'jasmine.json'));

// …but still allow the user to override the standard suite of specs.
if (process.argv[2]) {
  CONFIG.spec_files = [process.argv[2]];
}
jasmine.loadConfig(CONFIG);

const MAX_DURATION = 10 * 1000;

function bail () {
  console.error(`Script ran for more than ${MAX_DURATION / 1000} seconds after the end of the suite; there's an open handle somewhere!`);
  process.exit(2);
}

// Theory: the indefinite waiting that happens in CI sometimes might be the
// result of an open handle somewhere. If so, then the test task will keep
// running even though we haven't told Jasmine not to exit on completion. This
// approach might detect such scenarios and turn them into CI failures.
(async () => {
  await jasmine.execute();
  // Wait to see if the script is still running MAX_DURATION milliseconds from
  // now…
  let failsafe = setTimeout(bail, MAX_DURATION);
  // …but `unref` ourselves so that we're not the reason why the script keeps
  // running!
  failsafe.unref();
})();
