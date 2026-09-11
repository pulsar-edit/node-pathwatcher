# node-pathwatcher

Watch files and directories for changes.


> [!IMPORTANT]
> This library is used in [Pulsar][] in several places for compatibility reasons. If you’re here because you want a general-purpose file-watching library for Node, use `nsfw` or `@parcel/watcher` instead.
>
> The purpose of this library’s continued inclusion in Pulsar is to provide the [File][] and [Directory][] classes that have long been available as exports via `require('atom')`. It also delivers reliable file-watching on macOS on volumes that `FSEvents` cannot support (i.e., network volumes and drives that use incompatible filesystems).

## Installing

```bash
npm install pathwatcher
```

## Building

* Clone the repository
* `git submodule init && git submodule update`
* Run `npm install` to install the dependencies
* Run `npm test` to run the specs

## Caveats

This module is context-aware and context-safe; it can be used from multiple worker threads in the same process. If you keep a file-watcher active, though, it’ll keep the environment from closing; you must stop all watchers if you want your script or thread to finish.

If you’re using it in an Electron renderer process, you must take extra care in page reloading scenarios. Be sure to use `closeAllWatchers` well before the page environment is terminated — e.g., by attaching a `beforeunload` listener.

Be sure to read the more specific file-watching caveats below.

## Using

```js
const { watch, closeAllWatchers, getWatchedPaths } = require('pathwatcher');
```

### `watch(filename, listener)`

Watch for changes on `filename`, where `filename` is either a file or a directory. `filename` must be an absolute path and must exist at the time `watch` is called.

The listener callback gets two arguments: `(event, path)`. `event` can be `rename`, `delete` or `change`, and `path` is the path of the file which triggered the event.

The watcher is not recursive; changes to the contents of subdirectories will not be detected.

Returns an instance of `PathWatcher`. This instance is useful primarily for the `close` method that stops the watch operation.

#### Caveats

* All watching is **non-recursive**. If you watch `/foo/bar`, you will be notified about a change to `/foo/bar/index.js`, or the creation of the directory `/foo/bar/baz`; but you will not be told about a change to `/foo/bar/baz/something.js`.
* You may not watch a nonexistent path. Thus watching a specific file or directory can never notify you when that file or directory is created.
* When watching a file, `event` can be any of `rename`, `delete`, or `change`, where `change` means that the file’s contents changed somehow.
* When watching a directory, `event` can **only** be `change`, and in this context `change` signifies that one or more of the directory’s children changed (by being renamed, deleted, added, or modified).
* A watched directory will not report when it is renamed or deleted; it will simply stop reporting events. If you want to detect when a given directory is renamed or deleted, watch its parent directory and test for the child directory’s existence when you receive a `change` event. (But if the parent directory itself can possibly be renamed or deleted, you’re in a pickle! This scenario is better suited to a recursive watcher.)

### `closeAllWatchers()`

Stop watching on all subscribed paths.  All existing `PathWatcher` instances will stop receiving events. Call this if you’re going to end the process; it ensures that your script will exit cleanly.

### `getWatchedPaths()`

Returns an array of strings representing the **actual paths** that are being watched on disk.

These paths may not correlate to the paths that a consumer asked to watch for two reasons:

* Some platforms, when asked to watch `/foo/bar.js`, watch `/foo` instead. This allows for watcher reuse in some scenarios and would explain why the number of watched paths may not correlate to the number of active watchers.
* `pathwatcher` does `realpath` resolution and watches at a path’s canonical location on disk — which may or may not match the path you asked to watch.

### `PathWatcher::close()`

Stop watching for changes on the given `PathWatcher`.

### `File` and `Directory`

These are convenience wrappers around some filesystem operations. They also wrap `PathWatcher.watch` via their `onDidChange` (and similar) methods.

Documentation can be found on the Pulsar documentation site:

* [File][]
* [Directory][]


[File]: https://docs.pulsar-edit.dev/api/pulsar/latest/File/
[Directory]: https://docs.pulsar-edit.dev/api/pulsar/latest/Directory/
[Pulsar]: https://pulsar-edit.dev
