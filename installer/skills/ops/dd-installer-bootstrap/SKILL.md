---
name: dd-installer-bootstrap
description: How daedalus-cpp installs itself — dd_install compiles itself with a bare g++ before it compiles anything else, then runs a step graph that builds, gates, installs, seeds and smoke-tests. Read this before adding a step, moving a home, or debugging an install that stopped at a step.
tags: [installer, bootstrap, build, ops]
platform: linux
---

# The bootstrap is the proof

`installer/dd_install.cpp` is one translation unit compiled by a bare `g++` with
nothing else in the tree built. It then re-execs itself with `--booted` and does the
rest of the work. That ordering is the point, not a flourish: the compiler invocation
that produces the harness is the one the harness itself runs, so a fresh machine learns
whether this repo's toolchain assumptions hold before anything is claimed installed.

```
g++ -std=c++20 -O2 -Wall -Wextra -Werror -Wshadow -pipe -Iinclude \
    -o build/dd_install installer/dd_install.cpp
build/dd_install --self-check     # the freshly built binary answers
build/dd_install --booted ...     # the same binary, doing the install
```

Flags: `--home DIR` (default `$HOME/projects/daedalus`), `--root DIR`, `--minimal`
(install only `dd_agent`), `--no-test`, `--no-audit`, `--from DIR`.

## The steps, and what each refuses

1. **preflight** — repo holds `Makefile` and `installer/dd_install.cpp`; `g++` exists and
   *compiles* a `-std=c++20` probe; libcurl compiles **and links**; `-fopenmp` compiles and
   reports a thread count; `make` exists; `mold` is reported found or absent (absent is
   fine — it is an optimization, not a dependency); the home is not refused.
2. **build** — `make build/dd_tui` always, then `dd_agent`, then `dd` unless `--minimal`.
3. **gate** — `make test` must pass. `make doc-audit` reports drift but does not block:
   it is a claim about docs, not a binary.
4. **install** — `install -m 0755` each product into the home, then seed with
   `<dd_tui> --home DIR --init`, then assert `config.yaml` exists.
5. **skills** — seed `installer/skills/<group>/<name>/SKILL.md`, never over one that exists.
6. **import** — optional `dd import-daedalus --from DIR --skills --skins`.
7. **smoke** — an installed binary answers; see below.

A non-optional step failing **fails the install**. Exit codes: `0` installed, `2` bad
arguments, `3` a step ran and failed. An installer that papers over a failure is how a
broken install gets called "installed".

## Two rules the installer must not break

- **dd owns the config.** The home is seeded by `dd_tui --init`, never by the installer
  writing its own copy of the reference config — a second copy drifts the moment dd
  changes one key. `--init` exists only in `dd_tui`, so `--minimal` still *builds* dd_tui
  as the seeder and deliberately does not install it.
- **skills are seeded, never replaced.** dd's learn loop gives a skill carrying
  `edited_by` precedence over a draft, so overwriting a `SKILL.md` on upgrade erases
  exactly the thing the learn loop earned. The write is write-if-absent.

## Homes that are refused

A folder named `build/`, `out/` or `obj/` is make's output, not an install.
`~/.daedalus` belongs to the Python Daedalus and dd never lives there. Both are refused
in preflight with the reason, mirroring `dd::core::resolve_home`.

## Smoke without a network

`dd_agent` has no `--help` and needs an endpoint, so the minimal smoke points it at a
closed loopback port: `DD_BASE_URL=http://127.0.0.1:1/v1 dd_agent --home DIR -- ping`.
Passing needs the `[base= model= turns=` line (config.yaml parsed, model read), a `curl:`
line (transport reached), a non-zero exit, and **no** `terminate called`. A refused
connection proves home resolution, config parsing and transport wiring in one call, and
cannot fail for a reason the install is not responsible for.

## Probing a toolchain beats naming a package

`command -v libcurl` passes on a distro that ships the library without its headers and
then fails the build. So every dependency is probed by compiling — and for libcurl, by
*linking* too. The same reason the C++ standard is probed rather than parsed from
`g++ -dumpversion`: that prints the major only.
