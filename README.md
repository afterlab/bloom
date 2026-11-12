# Bloom

**Motion graphics and compositing, where the layer timeline and the node graph are the same
composition.**

[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
![Status: pre-release](https://img.shields.io/badge/status-pre--release-orange)

![Bloom's Default workspace: Assets, the Viewer showing a title card, Properties for the selected Title layer, and a Timeline of colored layers with keyframes](docs/images/readme/default-workspace.webp)

Bloom is an open-source desktop app for motion graphics and visual effects. If you've used a
layer-based compositor, you'll feel at home: layers on a timeline, keyframes, precomps, masks,
track mattes and a render queue. Underneath, every composition is also a node graph. Open the Nodes panel and
you're looking at the same shot, not a copy of it. Change either one and the other follows.

Bloom is written in C++20 and Qt 6. On Linux, the Viewer and final renders run on the GPU through
Vulkan. A CPU reference renderer defines what every frame should look like, so the GPU can't
quietly change your picture.

> [!IMPORTANT]
> Bloom is pre-release software. There are no downloadable builds yet, so you build it from source,
> and day-to-day development and testing happen on Linux.

## A look around

Every image here is a real Bloom session, captured from the `main` branch. The project uses
generated footage plus Bloom's own shapes, gradients and text.

**Node workspace.** The same composition as a graph. The fractal footage runs through Exposure and
Gaussian Blur before it joins the other layers in one Merge.

![Node workspace: the Viewer on the left, the node graph on the right, and the Timeline below](docs/images/readme/node-workspace.webp)

**Animation workspace.** The Viewer and Properties over a tall Timeline, with every layer and its
keyframes in view.

![Animation workspace: the Viewer and Properties above a Timeline listing eight layers with keyframes](docs/images/readme/animation-workspace.webp)

**Rendering in the background.** The Export queue takes the Timeline's place, renders one job at a
time and tells you how long is left. You can keep working while it runs.

![The Export queue rendering a composition at 44 percent, under the Viewer, with two more jobs queued](docs/images/readme/export-queue.webp)

## Build from source

You need:

- a C++20 compiler
- CMake 3.25 or newer
- Ninja
- Qt 6.8 or newer with Qt Widgets (the `dev` preset; a `qualified` build takes the one locked
  Qt from its dependency prefix instead)

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On Linux, the app is written to `build/dev/apps/bloom/Bloom`.

The `dev` preset uses the packages on your system. CI uses the `qualified` dependency mode instead,
which takes every dependency from a prefix built against a reviewed lock file. To reproduce the
Qt-free Linux CI build, build that prefix once per checkout (and again after the lock changes),
then use the CI preset:

```sh
cmake -S dependencies/superbuild -B build/dependency-superbuild \
    -DBLOOM_DEPENDENCY_PREFIX="$PWD/build/dependency-prefix" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/dependency-superbuild

CC=clang CXX=clang++ cmake --preset ci-linux-native
cmake --build --preset ci-linux-native
ctest --preset ci-linux-native
```

## Documentation

- [User guides](docs/README.md#user-guides): the Viewer, keyframes and easing, workspaces,
  exporting, color management, Python, `bloom-cli` and MCP.
- [Documentation index](docs/README.md): Bloom's canonical docs, in reading order.
- [Product foundation](docs/product/foundation.md): what Bloom is, and the decisions behind it.
- [Architecture overview](docs/architecture/overview.md) and
  [decision records](docs/decisions/): how the code is organized, and why.
- [Roadmap](docs/roadmap.md) and [node catalogue](docs/product/node-catalogue.md): the long-term
  plan.

## Contributing

The docs in this repository are Bloom's source of truth. Before you propose a change, read the
[documentation index](docs/README.md), the [product foundation](docs/product/foundation.md), and
the architecture or UX document that owns the behavior you want to change. Changes to module
boundaries or saved project semantics should update that document too, with an
[ADR](docs/decisions/) when they cut across the codebase.

Keep contributions small and vertical, with tests for document, command, project I/O and rendering
behavior. UI code reads state and issues commands. It never owns project data or does heavy work
on the UI thread.

## License

Bloom's original source and documentation are licensed under the
[Apache License, Version 2.0](LICENSE). Third-party dependencies and assets keep their own
licenses. Distributed builds must include the corresponding notices, source information and other
obligations of the components they ship.
