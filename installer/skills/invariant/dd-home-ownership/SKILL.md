---
name: dd-home-ownership
description: Which folder dd's config and state live in, in what order, and what refuses to be a home — the invariant that keeps daedalus-cpp out of ~/.daedalus and out of build/. Read this before moving an install, pointing DD_HOME at a folder, or sharing a config between dd and the Python Daedalus.
tags: [home, config, invariant, ownership]
platform: linux
---

# One folder governs

`dd::core::resolve_home` picks the home in this order, high to low:

1. `--home DIR`
2. the folder of `--config FILE`
3. `DD_HOME`, else `DAEDALUS_HOME`
4. the binary's own folder, **when it already holds `config.yaml`** — an installed copy
5. the folder dd was started from, **when it holds `config.yaml`** — the project
6. the binary's own folder, when it already holds `state/ cache/ logs/ learn/`
7. the folder dd was started from, where nothing of either kind exists yet

`$HOME` is never consulted. A folder that already carries its own `config.yaml` always
governs, so an installed copy never leaks into the project it was called from, and a
folder dd was started from never writes into the install. That is the whole reason
`make install` copies the binary into its own folder instead of running it from `build/`.

## Two homes are refused, whatever the order says

- **`build/`, `out/`, `obj/`** — make's output is not an install. An implicit home there
  is refused with the reason, even though rules 6 and 7 would otherwise land on it.
- **`~/.daedalus`** — the Python Daedalus's home. dd never reads `config.yaml` or `state/`
  from it, and asking for it explicitly (`--home ~/.daedalus`, `DD_HOME=~/.daedalus`) is
  refused. Sharing it makes two installs fight over one `config.yaml`.

The refusal is `resolve_home` returning an empty `root` plus a `why`; `dd_tui` and
`dd_agent` print it and exit **2**. `dd_install` mirrors both refusals in its own
preflight, so a bad home is rejected before anything is compiled rather than after.

## Only `--init` may create a home

A plain start never creates a folder. `dd_tui --init` is the only path that may make a
home, and it writes dd's embedded reference config; `--init --from FILE` copies an
existing config once. Without `config.yaml` and without `DD_BASE_URL`, dd stops with
exit 2 and says which folder it chose — an unconfigured install runs on defaults with a
warning, never on another install's config.

`DD_BASE_URL` is the deliberate escape hatch: it lets a home that has no `config.yaml`
run, because the endpoint is named in the environment rather than borrowed from a file
that belongs to someone else.

## Copying from the Python install is a one-way copy, once

`dd import-daedalus --from DIR --skills --skins` reads `$HOME/.daedalus` — the only place
that folder is ever read — and copies into a dd home. Nothing afterwards shares it: no
path in dd points at the Python home, so the two installs cannot drift each other.

## Editing config without editing code

The reference config lives in dd's source, and `dd_tui --init --from FILE` is how an
existing config becomes a home's. An installer or a script that carries its own copy of
the reference is a second source of truth; it drifts the moment dd changes one key.
