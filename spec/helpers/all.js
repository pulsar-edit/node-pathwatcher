// jasmine.getEnv().setIncludedTags([process.platform]);

global.makePromiseCallback = function makePromiseCallback(fn = () => {}) {
  let outerResolve;
  let promise = new Promise((resolve) => {
    outerResolve = resolve;
  });

  let callback = (...args) => {
    fn(...args);
    outerResolve();
  };

  return [promise, callback];
}

function timeoutPromise (ms) {
  return new Promise((_, reject) => setTimeout(reject, ms));
}

global.condition = function condition(fn, timeoutMs = 5000) {
  let promise = new Promise((resolve) => {
    let poll = () => {
      let outcome = fn();
      if (outcome) resolve();
      setTimeout(poll, 50);
    };
    poll();
  });

  return Promise.race([promise, timeoutPromise(timeoutMs)]);
};


global.wait = function wait(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
};
