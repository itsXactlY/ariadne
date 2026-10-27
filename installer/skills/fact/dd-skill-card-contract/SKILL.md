---
name: dd-skill-card-contract
description: The exact contract a SKILL.md must satisfy to become a router-visible card in daedalus-cpp — frontmatter keys, the name/description truncations, the category rule that needs three path parts, and what dd skills check --strict reports. Read this before writing or moving a skill in a dd home.
tags: [skills, scanner, catalogue, contract]
platform: linux
---

# What a SKILL.md becomes

`dd::skills::Scanner::scan()` walks a root for files named `SKILL.md`, sorts them, and
reads only the first 16384 bytes of each — enough to close a frontmatter on a long card
without reading a monster whole. Everything below is what the scan actually does, not
what a skill *should* have.

| key | behaviour |
|---|---|
| `name` | used unquoted; absent → the containing folder's name; truncated at **64 bytes** |
| `description` | used unquoted; absent → the first body line that is not a heading; truncated at **1024 bytes** with a `...` tail |
| `platform` | a list; a card whose list does not contain the active platform is skipped. Absent key = all platforms |
| `tags` | parsed as a list into the card's tags |
| any other key | kept in the frontmatter map, not surfaced as a card field |

**First `name` wins.** A duplicate name is dropped, and because the walk is sorted, the
lexicographically-first path keeps the name. A skill that shadows another is silently
absent, so names are the one thing worth grepping before adding a card.

## Category needs three path parts

`category_from_path` splits the path under the root and returns the first component **only
when there are at least three parts**: `<group>/<name>/SKILL.md`. A flat
`skills/<name>/SKILL.md` scans with an empty category — it is a card, but it appears in no
group and cannot be filtered with `dd list --category C`. This is why `installer/skills/`
mirrors the group level instead of seeding a flat tree.

## The card the router sees

`dd::know::DiskSkills::list()` turns each scanned skill into a card, on all cores when the
set is large enough to be worth an OpenMP clause, gathering results in scan order:

- `gist` = `gist_of(description)` — squashed whitespace, cut at 400 bytes on a UTF-8
  boundary and preferably at a space, with an ellipsis. A card with an empty description
  has an empty gist, and `dd skills check --strict` calls that **no gist**.
- `body_bytes` = the file's whole size, capped by `read_all` at 4 MiB.
- `rev` = a hash of the text, so a changed card is distinguishable from a re-read one.
- `group` = the category; `source` = `disk:<parent>/<root>`, which is what tells an
  operator which root a card came from.

A card with no body is **a pointer to nothing**: `Router` skips it, and `deliver` prints
`(no body to open)`. Only a card with a body and `body_bytes` under the policy's
`open_max_bytes` is opened automatically — bigger ones are offered to be opened by name.

`dd skills check --strict` reports `no body`, `no gist`, `skipped`, `truncated`; any of
them non-zero is problems. A skill directory that ships frontmatter-only cards therefore
fails the census: the body is what the router can open.

## Files inside a skill

`DiskSkills::file(card, rel)` serves a file from the skill's own folder: both the folder
and the target are canonicalized and the target must be strictly inside the folder, so a
`..` path cannot leave it. That is the only way a card reaches `references/` or a script
beside `SKILL.md`.

## Seeding is not editing

An installed copy of a skill is the model's, not the repo's: dd's learn loop gives a card
carrying `edited_by` precedence over a draft, so an installer, a sync, or a `git checkout`
over that folder erases learned state. Seed where absent; never overwrite in place.
