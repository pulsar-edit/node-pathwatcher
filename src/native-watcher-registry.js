// Heavily based on a similar implementation in the Atom/Pulsar codebase and
// licensed under MIT.
//
// This differs from the one in the Pulsar codebase in the following ways:
//
// * We employ more strategies for reusing watchers. For instance: we allow for
//   “sibling” directories to detect each other and share a watcher pointing at
//   their common parent. We also allow a `PathWatcher` whose `NativeWatcher`
//   points at an ancestor to eschew it in favor of a closer `NativeWatcher`
//   when it's the only consumer on that path chain.
// * We don't have any automatic behavior around `NativeWatcher` stopping. If a
//   `NativeWatcher` stops, we assume it's because its `PathWatcher` stopped
//   and it had no other `PathWatcher`s to assist.
//
// This registry does not manage `NativeWatcher`s and is not in charge of
// destroying them during cleanup. It _is_ in charge of knowing when and where
// to create a `NativeWatcher` — including knowing when it _doesn’t_ need to
// create a `NativeWatcher` because an existing one can already do the job.

const path = require('path');

// Private: re-join the segments split from an absolute path to form another
// absolute path.
function absolute(...parts) {
  const candidate = path.join(...parts);
  let result = path.isAbsolute(candidate)
    ? candidate
    : path.join(path.sep, candidate);
  return result;
}

// Private: Map userland filesystem watcher subscriptions efficiently to
// deliver filesystem change notifications to each watcher with the most
// efficient coverage of native watchers.
//
// * If two watchers subscribe to the same directory, use a single native
//   watcher for each.
// * Re-use a native watcher watching a parent directory for a watcher on a
//   child directory. If the parent directory watcher is removed, it will be
//   split into child watchers.
// * If any child directories already being watched, stop and replace them with
//   a watcher on the parent directory.
//
// Uses a trie whose structure mirrors the directory structure.
class RegistryTree {
  // Private: Construct a tree with no native watchers.
  //
  // * `basePathSegments` the position of this tree's root relative to the
  //   filesystem's root as an {Array} of directory names.
  // * `createNative` {Function} used to construct new native watchers. It
  //   should accept an absolute path as an argument and return a new
  //   {NativeWatcher}.
  constructor(basePathSegments, createNative, options = {}) {
    this.basePathSegments = basePathSegments;
    this.root = new RegistryNode(null);
    this.createNative = createNative;
    this.options = options;
  }

  // Private: Identify the native watcher that should be used to produce events
  // at a watched path, creating a new one if necessary.
  //
  // * `pathSegments` The path to watch represented as an {Array} of directory
  //   names relative to this {RegistryTree}'s root.
  // * `attachToNative` {Function} invoked with the appropriate native watcher
  //   and the absolute path to its watch root.
  add(pathSegments, attachToNative) {
    const absolutePathSegments = this.basePathSegments.concat(pathSegments);
    const absolutePath = absolute(...absolutePathSegments);

    // Scenario in which we're attaching to an ancestor of this path — for
    // instance, if we discover that we share an ancestor directory with an
    // existing watcher.
    const attachToAncestor = (absolutePaths, childPaths) => {
      let ancestorAbsolutePath = absolute(...absolutePaths);
      let native = this.createNative(ancestorAbsolutePath);
      let leaf = new RegistryWatcherNode(
        native,
        absolutePaths,
        childPaths,
        this.options
      );
      this.root = this.root.insert(absolutePaths, leaf);
      attachToNative(native, ancestorAbsolutePath);
      return native;
    };

    // Scenario in which we're attaching directly to a specific path.
    const attachToNew = (childPaths) => {
      const native = this.createNative(absolutePath);
      const leaf = new RegistryWatcherNode(
        native,
        absolutePathSegments,
        childPaths,
        this.options
      );
      this.root = this.root.insert(pathSegments, leaf);
      attachToNative(native, absolutePath);
      return native;
    };

    this.root.lookup(pathSegments).when({
      parent: (parent, remaining) => {
        if (!this.options.reuseAncestorWatchers) {
          attachToNew([]);
          return;
        }
        // An existing `NativeWatcher` is watching the same directory or a
        // parent directory of the requested path. Attach this `PathWatcher` to
        // it as a filtering watcher and record it as a dependent child path.
        const native = parent.getNativeWatcher();
        parent.addChildPath(remaining);
        attachToNative(native, absolute(...parent.getAbsolutePathSegments()));
      },
      children: children => {
        // One or more `NativeWatcher`s exist on child directories of the
        // requested path. Create a new `NativeWatcher` on the parent
        // directory, note the subscribed child paths, and cleanly stop the
        // child native watchers.
        const newNative = attachToNew(children.map(child => child.path));
        for (let i = 0; i < children.length; i++) {
          const childNode = children[i].node;
          const childNative = childNode.getNativeWatcher();
          childNative.reattachListenersTo(newNative, absolutePath);
          childNative.dispose();
          childNative.stop();
        }
      },
      missing: (lastParent) => {
        if (!this.options.mergeWatchersWithCommonAncestors) {
          attachToNew([]);
          return;
        }

        // We couldn't find an existing watcher anywhere above us in this path
        // hierarchy. But we helpfully receive the last node that was already
        // in the tree (i.e., created by a previous watcher), so we might be
        // able to consolidate two watchers.
        if (lastParent?.parent == null) {
          // We're at the root node; there is no other watcher in this tree.
          // Create one at the current location.
          attachToNew([]);
          return;
        }

        let leaves = lastParent.leaves(this.basePathSegments);

        if (leaves.length === 0) {
          // There's an ancestor node, but it doesn't have any native watchers
          // below it. This would happen if there once was a watcher at a
          // different point in the tree, but it was disposed of before we got
          // here.
          //
          // This is functionally the same as the above case, so we'll create a
          // new native watcher at the current path.
          attachToNew([]);
          return;
        }

        // If we get this far, then one of our ancestor directories has an
        // active native watcher somewhere underneath it. We can streamline
        // native watchers by creating a new one to manage two or more existing
        // paths, then stopping the one that was previously running.
        let ancestorPathSegments = lastParent.getPathSegments(this);

        let remainingPathSegments = [...pathSegments];
        for (let i = 0; i < ancestorPathSegments.length; i++) {
          remainingPathSegments.shift();
        }

        // Taken to its logical extreme, this approach would always yield
        // a maximum of one watcher, since all paths have a common ancestor.
        // But if we listen at the root of the volume, we'll be drinking from a
        // firehose and making our wrapped watchers do a lot of work.
        //
        // So we should strike a balance: good to consolidate watchers when
        // they’re “close enough” to one another in the tree, but bad to do it
        // obsessively and create lots of churn.
        //
        // NOTE: We can also introduce platform-specific logic here. For
        // instance, consolidating watchers seems to be important on macOS and
        // less so on Windows and Linux.
        //
        // Let's impose some constraints:

        // Impose a max distance when moving upward. This will let us avoid
        // _creating_ a new watcher that's more than a certain number of levels
        // above the path we care about.
        //
        // This does not prevent us from _reusing_ such a watcher that is
        // already present (as in the `parent` scenario above). We were already
        // paying the cost of that watcher.
        //
        // TODO: Expose configuration options for these constraints to the
        // consumer.
        //
        let difference = pathSegments.length - ancestorPathSegments.length;
        console.debug('Tier difference:', difference);
        if (difference > this.options.maxCommonAncestorLevel) {
          attachToNew([]);
          return;
        }

        // NOTE: Future ideas for constraints:
        //
        // * Don't create a new watcher at the root unless explicitly told to.
        // * Allow the wrapper code to specify certain paths above which we're
        //   not allowed to ascend unless explicitly told. (The user's home
        //   folder feels like a good one.)
        // * Perhaps enforce a soft native-watcher quota and have it
        //   consolidate more aggressively when we're close to the quota than
        //   when we're not.

        let childPaths = leaves.map(l => l.path);
        childPaths.push(remainingPathSegments);
        let newNative = attachToAncestor(ancestorPathSegments, childPaths);
        let absolutePath = absolute(...ancestorPathSegments);
        for (let i = 0; i < leaves.length; i++) {
          let leaf = leaves[i].node;
          let native = leaf.getNativeWatcher();
          native.reattachListenersTo(newNative, absolutePath);
          native.dispose();
          native.stop();
          // NOTE: Should not need to dispose of native watchers; it should
          // happen automatically as they are left.
        }
      }
    });
  }

  remove (pathSegments) {
    this.root = this.root.remove(pathSegments, this.createNative) ||
      new RegistryNode(null);
  }

  // Private: Access the root node of the tree.
  getRoot() {
    return this.root;
  }

  // Private: Return a {String} representation of this tree’s structure for
  // diagnostics and testing.
  print() {
    return this.root.print();
  }
}

// Private: Non-leaf node in a {RegistryTree} used by the
// {NativeWatcherRegistry} to cover the allocated {Watcher} instances with the
// most efficient set of {NativeWatcher} instances possible. Each
// {RegistryNode} maps to a directory in the filesystem tree.
class RegistryNode {
  // Private: Construct a new, empty node representing a node with no watchers.
  constructor(parent, pathKey, options) {
    this.parent = parent;
    this.pathKey = pathKey;
    this.children = {};
    this.options = options;
  }

  getPathSegments (comparison = null) {
    let result = [this.pathKey];
    let pointer = this.parent;
    while (pointer && pointer.pathKey && pointer !== comparison) {
      result.unshift(pointer.pathKey);
      pointer = pointer.parent;
    }
    return result;
  }

  // Private: Recursively discover any existing watchers corresponding to a
  // path.
  //
  // * `pathSegments` filesystem path of a new {Watcher} already split into an
  //   Array of directory names.
  //
  // Returns: A {ParentResult} if the exact requested directory or a parent
  // directory is being watched, a {ChildrenResult} if one or more child paths
  // are being watched, or a {MissingResult} if no relevant watchers exist.
  lookup(pathSegments) {
    if (pathSegments.length === 0) {
      return new ChildrenResult(this.leaves([]));
    }

    const child = this.children[pathSegments[0]];
    if (child === undefined) {
      return new MissingResult(this);
    }

    return child.lookup(pathSegments.slice(1));
  }

  // Private: Insert a new {RegistryWatcherNode} into the tree, creating new
  // intermediate {RegistryNode} instances as needed. Any existing children of
  // the watched directory are removed.
  //
  // * `pathSegments` filesystem path of the new {Watcher}, already split into
  //   an Array of directory names.
  // * `leaf` initialized {RegistryWatcherNode} to insert
  //
  // Returns: The root of a new tree with the {RegistryWatcherNode} inserted at
  // the correct location. Callers should replace their node references with
  // the returned value.
  insert(pathSegments, leaf) {
    // console.log('Insert:', pathSegments);
    if (pathSegments.length === 0) {
      return leaf;
    }

    const pathKey = pathSegments[0];
    let child = this.children[pathKey];
    if (child === undefined) {
      child = new RegistryNode(this, pathKey, this.options);
    }
    this.children[pathKey] = child.insert(pathSegments.slice(1), leaf);
    return this;
  }

  // Private: Remove a {RegistryWatcherNode} by its exact watched directory.
  //
  // * `pathSegments` absolute pre-split filesystem path of the node to remove.
  // * `createSplitNative` callback to be invoked with each child path segment
  //   {Array} if the {RegistryWatcherNode} is split into child watchers rather
  //   than removed outright. See {RegistryWatcherNode.remove}.
  //
  // Returns: The root of a new tree with the {RegistryWatcherNode} removed.
  // Callers should replace their node references with the returned value.
  remove(pathSegments, createSplitNative) {
    if (pathSegments.length === 0) {
      // Attempt to remove a path with child watchers. Do nothing.
      return this;
    }

    const pathKey = pathSegments[0];
    const child = this.children[pathKey];
    if (child === undefined) {
      // Attempt to remove a path that isn't watched. Do nothing.
      return this;
    }

    // Recurse
    const newChild = child.remove(pathSegments.slice(1), createSplitNative);
    if (newChild === null) {
      delete this.children[pathKey];
    } else {
      this.children[pathKey] = newChild;
    }

    // Remove this node if all of its children have been removed
    return Object.keys(this.children).length === 0 ? null : this;
  }

  // Private: Discover all {RegistryWatcherNode} instances beneath this tree
  // node and the child paths that they are watching.
  //
  // * `prefix` {Array} of intermediate path segments to prepend to the
  //   resulting child paths.
  //
  // Returns: A possibly empty {Array} of `{node, path}` objects describing
  // {RegistryWatcherNode} instances beneath this node.
  leaves(prefix) {
    const results = [];
    for (const p of Object.keys(this.children)) {
      results.push(...this.children[p].leaves(prefix.concat([p])));
    }
    return results;
  }

  // Private: Return a {String} representation of this subtree for diagnostics
  // and testing.
  print(indent = 0) {
    let spaces = '';
    for (let i = 0; i < indent; i++) {
      spaces += ' ';
    }

    let result = '';
    for (const p of Object.keys(this.children)) {
      result += `${spaces}${p}\n${this.children[p].print(indent + 2)}`;
    }
    return result;
  }
}

// Private: Leaf node within a {NativeWatcherRegistry} tree. Represents a directory that is covered by a
// {NativeWatcher}.
class RegistryWatcherNode {
  // Private: Allocate a new node to track a {NativeWatcher}.
  //
  // * `nativeWatcher` An existing {NativeWatcher} instance.
  // * `absolutePathSegments` The absolute path to this {NativeWatcher}'s
  //   directory as an {Array} of path segments.
  // * `childPaths` {Array} of child directories that are currently the
  //   responsibility of this {NativeWatcher}, if any. Directories are
  //   represented as arrays of the path segments between this node's directory
  //   and the watched child path.
  constructor(nativeWatcher, absolutePathSegments, childPaths, options) {
    this.nativeWatcher = nativeWatcher;
    this.absolutePathSegments = absolutePathSegments;
    this.options = options;

    // Store child paths as joined strings so they work as Set members.
    this.childPaths = new Set();
    for (let i = 0; i < childPaths.length; i++) {
      this.childPaths.add(path.join(...childPaths[i]));
    }
  }

  // Private: Assume responsibility for a new child path. If this node is
  // removed, it will instead split into a subtree with a new
  // {RegistryWatcherNode} for each child path.
  //
  // * `childPathSegments` the {Array} of path segments between this node's
  //   directory and the watched child directory.
  addChildPath(childPathSegments) {
    this.childPaths.add(path.join(...childPathSegments));
  }

  // Private: Stop assuming responsibility for a previously assigned child
  // path. If this node is removed, the named child path will no longer be
  // allocated a {RegistryWatcherNode}.
  //
  // * `childPathSegments` the {Array} of path segments between this node's
  //   directory and the no longer watched child directory.
  removeChildPath(childPathSegments) {
    this.childPaths.delete(path.join(...childPathSegments));
  }

  // Private: Accessor for the {NativeWatcher}.
  getNativeWatcher() {
    return this.nativeWatcher;
  }

  // Private
  insert (pathSegments, leaf) {
    if (pathSegments.length === 0) return leaf;
  }

  destroyNativeWatcher(shutdown) {
    this.nativeWatcher.stop(shutdown);
  }

  // Private: Return the absolute path watched by this {NativeWatcher} as an
  // {Array} of directory names.
  getAbsolutePathSegments() {
    return this.absolutePathSegments;
  }

  // Private: Identify how this watcher relates to a request to watch a
  // directory tree.
  //
  // * `pathSegments` filesystem path of a new {Watcher} already split into an
  //   Array of directory names.
  //
  // Returns: A {ParentResult} referencing this node.
  lookup(pathSegments) {
    return new ParentResult(this, pathSegments);
  }

  // Private: Remove this leaf node if the watcher's exact path matches. If
  // this node is covering additional {Watcher} instances on child paths, it
  // will be split into a subtree.
  //
  // * `pathSegments` filesystem path of the node to remove.
  // * `createSplitNative` callback invoked with each {Array} of absolute child
  //   path segments to create a native watcher on a subtree of this node.
  //
  // Returns: If `pathSegments` match this watcher's path exactly, returns
  // `null` if this node has no `childPaths` or a new {RegistryNode} on a newly
  // allocated subtree if it did. If `pathSegments` does not match the
  // watcher's path, it's an attempt to remove a subnode that doesn't exist, so
  // the remove call has no effect and returns `this` unaltered.
  remove(pathSegments, createSplitNative) {
    // This function represents converting this `RegistryWatcherNode` into a
    // plain `RegistryNode` that no longer has the direct responsibility of
    // managing a native watcher. Any child paths on this node are converted to
    // leaf nodes with their own native watchers.
    //
    // We do this if:
    //
    // * This path itself is being removed.
    // * One of this path’s child paths is being removed and it has only one
    //   remaining child path. (We move the watcher down to the child in this
    //   instance.)
    //
    // TODO: Also invoke some form of this logic if more than two paths are
    // being watched… but the removal of a path creates a scenario where we can
    // move a watcher to a closer common descendant of the remaining paths.
    let replacedWithNode = () =>{
      let newSubTree = new RegistryTree(
        this.absolutePathSegments,
        createSplitNative,
        this.options
      );

      for (const childPath of this.childPaths) {
        const childPathSegments = childPath.split(path.sep);
        newSubTree.add(childPathSegments, (native, attachmentPath) => {
          this.nativeWatcher.reattachListenersTo(native, attachmentPath);
        });
      }
      return newSubTree.getRoot();
    };
    if (pathSegments.length !== 0) {
      this.removeChildPath(pathSegments);
      if (this.childPaths.size === 1) {
        return replacedWithNode();
      }
      return this;
    } else if (this.childPaths.size > 0) {
      // We are here because a watcher for this path is being removed. If this
      // path has descendants depending on the same watcher, this is an
      // opportunity to create a new `NativeWatcher` that is more proximate to
      // those descendants.
      return replacedWithNode();
    } else {
      return null;
    }
  }

  // Private: Discover this {RegistryWatcherNode} instance.
  //
  // * `prefix` {Array} of intermediate path segments to prepend to the
  //   resulting child paths.
  //
  // Returns: An {Array} containing a `{node, path}` object describing this
  // node.
  leaves(prefix) {
    return [{ node: this, path: prefix }];
  }

  // Private: Return a {String} representation of this watcher for diagnostics
  // and testing. Indicates the number of child paths that this node's
  // {NativeWatcher} is responsible for.
  print(indent = 0) {
    let result = '';
    for (let i = 0; i < indent; i++) {
      result += ' ';
    }
    result += '[watcher';
    if (this.childPaths.size > 0) {
      result += ` +${this.childPaths.size}`;
    }
    result += ']\n';

    return result;
  }
}

// Private: A {RegistryNode} traversal result that's returned when neither a
// directory, its children, nor its parents are present in the tree.
class MissingResult {
  // Private: Instantiate a new {MissingResult}.
  //
  // * `lastParent` the final successfully traversed {RegistryNode}.
  constructor(lastParent) {
    this.lastParent = lastParent;
  }

  // Private: Dispatch within a map of callback actions.
  //
  // * `actions` {Object} containing a `missing` key that maps to a callback to
  //   be invoked when no results were returned by {RegistryNode.lookup}. The
  //   callback will be called with the last parent node that was encountered
  //   during the traversal.
  //
  // Returns: the result of the `actions` callback.
  when(actions) {
    return actions.missing(this.lastParent);
  }
}

// Private: A {RegistryNode.lookup} traversal result that's returned when a
// parent or an exact match of the requested directory is being watched by an
// existing {RegistryWatcherNode}.
class ParentResult {
  // Private: Instantiate a new {ParentResult}.
  //
  // * `parent` the {RegistryWatcherNode} that was discovered.
  // * `remainingPathSegments` an {Array} of the directories that lie between
  //   the leaf node's watched directory and the requested directory. This will
  //   be empty for exact matches.
  constructor(parent, remainingPathSegments) {
    this.parent = parent;
    this.remainingPathSegments = remainingPathSegments;
  }

  getAbsolutePathSegments () {
    let result = Array.from(this.remainingPathSegments);
    let pointer = this.parent;
    while (pointer) {
      result.push(pointer.remainingPathSegments);
      pointer = pointer.parent;
    }
    return result;
  }

  // Private: Dispatch within a map of callback actions.
  //
  // * `actions` {Object} containing a `parent` key that maps to a callback to
  //   be invoked when a parent of a requested requested directory is returned
  //   by a {RegistryNode.lookup} call. The callback will be called with the
  //   {RegistryWatcherNode} instance and an {Array} of the {String} path
  //   segments that separate the parent node and the requested directory.
  //
  // Returns: the result of the `actions` callback.
  when(actions) {
    return actions.parent(this.parent, this.remainingPathSegments);
  }
}

// Private: A {RegistryNode.lookup} traversal result that's returned when one
// or more children of the requested directory are already being watched.
class ChildrenResult {
  // Private: Instantiate a new {ChildrenResult}.
  //
  // * `children` {Array} of the {RegistryWatcherNode} instances that were
  //   discovered.
  constructor(children) {
    this.children = children;
  }

  // Private: Dispatch within a map of callback actions.
  //
  // * `actions` {Object} containing a `children` key that maps to a callback
  //   to be invoked when a parent of a requested requested directory is
  //   returned by a {RegistryNode.lookup} call. The callback will be called
  //   with the {RegistryWatcherNode} instance.
  //
  // Returns: the result of the `actions` callback.
  when(actions) {
    return actions.children(this.children);
  }
}

// Private: Track the directories being monitored by native filesystem
// watchers. Minimize the number of native watchers allocated to receive events
// for a desired set of directories by:
//
// 1. Subscribing to the same underlying {NativeWatcher} when watching the same
//    directory multiple times.
// 2. Subscribing to an existing {NativeWatcher} on a parent of a desired
//    directory.
// 3. Replacing multiple {NativeWatcher} instances on child directories with a
//    single new {NativeWatcher} on the parent.
class NativeWatcherRegistry {
  static DEFAULT_OPTIONS = {
    // When adding a watcher for `/foo/bar/baz/thud`, will reuse any of
    // `/foo/bar/baz`, `/foo/bar`, or `/foo` that may already exist.
    //
    // When `false`, a second native watcher will be created in this scenario
    // instead.
    reuseAncestorWatchers: true,

    // When a single native watcher exists at `/foo/bar/baz/thud` and a watcher
    // is added for `/foo/bar`, will create a new native watcher at `/foo/bar`
    // and tell the existing watcher to use it instead.
    //
    // When `false`, a second native watcher will be created in this scenario
    // instead.
    relocateDescendantWatchers: true,

    // When a single native watcher at `/foo/bar` supplies watchers at both
    // `/foo/bar` and `/foo/bar/baz/thud`, and the watcher at `/foo/bar` is
    // removed, will relocate the native watcher to the more specific
    // `/foo/bar/baz/thud` path for efficiency.
    //
    // When `false`, the too-broad native watcher will remain in place.
    relocateAncestorWatchers: true,

    // When adding a watcher for `/foo/bar/baz/thud`, will look for an existing
    // watcher at any descendant of `/foo/bar/baz`, `/foo/bar`, or `/foo` and
    // create a new native watcher that supplies both the existing watcher and
    // the new watcher by watching their common ancestor.
    //
    // When `false`, watchers will not be consolidated when one is not an
    // ancestor of the other.
    mergeWatchersWithCommonAncestors: true,

    // When using the strategy described above, will enforce a maximum limit on
    // common ancestorship. For instance, if two directories share a
    // great-great-great-great-grandfather, then it would not necessarily make
    // sense for them to share a watcher; the potential firehose of file events
    // they’d have to ignore would more than counterbalance the resource
    // savings.
    //
    // When set to a positive integer X, will refuse to consolidate watchers in
    // different branches of a tree unless their common ancestor is no more
    // than X levels above _each_ one.
    //
    // When set to `0` or a negative integer, will enforce no maximum common
    // ancestor level.
    //
    // Has no effect unless `mergeWatchersWithCommonAncestors` is `true`.
    maxCommonAncestorLevel: 3
  };

  // Private: Instantiate an empty registry.
  //
  // * `createNative` {Function} that will be called with a normalized
  //   filesystem path to create a new native filesystem watcher.
  constructor(createNative, options = {}) {
    this._createNative = createNative;
    this.options = {
      ...NativeWatcherRegistry.DEFAULT_OPTIONS,
      ...options
    };
    this.tree = new RegistryTree([], createNative, this.options);
  }

  reset () {
    this.tree = new RegistryTree([], this._createNative, this.options);
  }

  // Private: Attach a watcher to a directory, assigning it a {NativeWatcher}.
  // If a suitable {NativeWatcher} already exists, it will be attached to the
  // new {Watcher} with an appropriate subpath configuration. Otherwise, the
  // `createWatcher` callback will be invoked to create a new {NativeWatcher},
  // which will be registered in the tree and attached to the watcher.
  //
  // If any pre-existing child watchers are removed as a result of this
  // operation, {NativeWatcher.onWillReattach} will be broadcast on each with
  // the new parent watcher as an event payload to give child watchers a chance
  // to attach to the new watcher.
  //
  // * `watcher` an unattached {Watcher}.
  attach(watcher, normalizedDirectory = undefined) {
    if (!normalizedDirectory) {
      normalizedDirectory = watcher.getNormalizedPath();
      if (!normalizedDirectory) {
        return this.attachAsync(watcher);
      }
    }
    const pathSegments = normalizedDirectory
      .split(path.sep)
      .filter(segment => segment.length > 0);

    this.tree.add(pathSegments, (native, nativePath) => {
      watcher.attachToNative(native, nativePath);
    });
  }

  async attachAsync (watcher) {
    const normalizedDirectory = await watcher.getNormalizedPathPromise();
    return this.attach(watcher, normalizedDirectory);
  }

  // TODO: This registry envisions `PathWatcher` instances that can be attached
  // to any number of `NativeWatcher` instances. But it also envisions an
  // “ownership” model that isn't quite accurate.
  //
  // Ideally, we'd want something like this:
  //
  //   1. Someone adds a watcher for /Foo/Bar/Baz/thud.txt.
  //   2. We set up a `NativeWatcher` for /Foo/Bar/Baz.
  //   3. Someone adds a watcher for /Foo/Bar/Baz/A/B/C/zort.txt.
  //   4. We reuse the existing `NativeWatcher`.
  //   5. Someone stops the `PathWatcher` from step 1.
  //
  // What we want to happen:
  //
  //   6. We take that opportunity to streamline the `NativeWatchers`; since
  //      it’s the only one left, we know we can create a new `NativeWatcher`
  //      at /Foo/Bar/Baz/A/B/C and swap it onto the last `PathWatcher`
  //      instance.
  //
  // What actually happens:
  //
  //   6. The original `NativeWatcher` keeps going (since it has one remaining
  //     dependency) and our single `PathWatcher` stays subscribed to it.
  //
  //
  // This is fine as a consolation prize, but it's less efficient.
  //
  // Frustratingly, most of what we want happens in response to the stopping of
  // a `NativeWatcher`. If a `PathWatcher` relies on a `NativeWatcher` and
  // finds that it has stopped, this registry will spin up a new
  // `NativeWatcher` and allow it to resume. But we wouldn’t stop that
  // `NativeWatcher` in the first place, since we know more than one
  // `PathWatcher` is relying on it!
  //
  // I’ve made preliminary attempts to address this by moving some of the logic
  // around, but it’s not yet had the effect I want.

  detach (watcher, normalizedDirectory = undefined) {
    if (!normalizedDirectory) {
      normalizedDirectory = watcher.getNormalizedPath();
      if (!normalizedDirectory) {
        return this.detachAsync(watcher);
      }
    }
    const pathSegments = normalizedDirectory
      .split(path.sep)
      .filter(segment => segment.length > 0);

    this.tree.remove(pathSegments, (native, nativePath) => {
      watcher.attachToNative(native, nativePath);
    });

  }

  async detachAsync(watcher) {
    const normalizedDirectory = await watcher.getNormalizedPathPromise();
    return this.detach(watcher, normalizedDirectory);
  }

  // Private: Generate a visual representation of the currently active watchers
  // managed by this registry.
  //
  // Returns a {String} showing the tree structure.
  print() {
    return this.tree.print();
  }
}

module.exports = { NativeWatcherRegistry };
