# scrap

## README.md

Do not update README.md. It is intentionally just the title and a short
description — leave it alone. Never add features, keybinds, options, or usage
sections to it, and do not update it when adding or changing functionality.

## Naming

Config keys, struct fields, and anything the UI prints are read cold by someone
who did not write them. Use the plain technical noun for the thing — `job`,
`step`, `tier` — never a verb phrase or a colloquialism (`does`, `skip the
step`), and name the setting rather than describing it. The same holds for the
sentences written into generated config files: technical and brief, no chat.
