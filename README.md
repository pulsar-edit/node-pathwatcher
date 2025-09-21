# node-pathwatcher

Watch files and directories for changes.


> [!IMPORTANT]
> This library is used in [Pulsar][] in several places for compatibility reasons. The [nsfw](https://www.npmjs.com/package/nsfw) library is more robust and more widely used; it is available in Pulsar via `atom.watchPath` and is usually a better choice.
>
> If you’re here because you want a general-purpose file-watching library for Node, use `nsfw` instead.
>
> The purpose of this library’s continued inclusion in Pulsar is to provide the [File][] and [Directory][] classes that have long been available as exports via `require('atom')`.

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

## Using

```js
const PathWatcher = require('pathwatcher');
```

### `watch(filename, listener)`

Watch for changes on `filename`, where `filename` is either a file or a directory. `filename` must be an absolute path and must exist at the time `watch` is called.

The listener callback gets two arguments: `(event, path)`. `event` can be `rename`, `delete` or `change`, and `path` is the path of the file which triggered the event.

The watcher is not recursive; changes to the contents of subdirectories will not be detected.

Returns an instance of `PathWatcher`. This instance is useful primarily for the `close` method that stops the watch operation.

#### Caveats

* Watching a specific file or directory will not notify you when that file or directory is created, since the file must already exist before you start watching the path.
* When watching a file, `event` can be any of `rename`, `delete`, or `change`, where `change` means that the file’s contents changed somehow.
* When watching a directory, `event` can only be `change`, and in this context `change` signifies that one or more of the directory’s children changed (by being renamed, deleted, added, or modified).
* A watched directory will not report when it is renamed or deleted. If you want to detect when a given directory is deleted, watch its parent directory and test for the child directory’s existence when you receive a `change` event.

### `PathWatcher::close()`

Stop watching for changes on the given `PathWatcher`.

### `closeAllWatchers()`

Stop watching on all subscribed paths.  All existing `PathWatcher` instances will stop receiving events. Call this if you’re going to end the process; it ensures that your script will exit cleanly.

### `getWatchedPaths()`

Returns an array of strings representing the actual paths that are being watched on disk.

`pathwatcher` watches directories in all instances, since it’s easy to do so in a cross-platform manner.

### `File` and `Directory`

These are convenience wrappers around some filesystem operations. They also wrap `PathWatcher.watch` via their `onDidChange` (and similar) methods.

Documentation can be found on the Pulsar documentation site:

* [File][]
* [Directory][]


[File]: https://docs.pulsar-edit.dev/api/pulsar/latest/File/
[Directory]: https://docs.pulsar-edit.dev/api/pulsar/latest/Directory/
[Pulsar]: https://pulsar-edit.dev
