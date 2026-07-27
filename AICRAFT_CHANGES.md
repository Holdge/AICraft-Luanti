# AICraft-Luanti modifications

This fork is based on Luanti 5.16.1 commit
`5ebd9b57984d5854e0a37fd0125da48e9e59e190` and remains licensed under
LGPL-2.1-or-later.

## Agent client target

Configure with `-DBUILD_AGENT_CLIENT=ON` to produce
`bin/aicraft-agent-client`.

The target:

- retains the upstream Client, LocalPlayer, collision, inventory, interaction,
  and UDP protocol code;
- selects Irrlicht's Null video driver and disables sound;
- skips the main menu and rejects startup without a server address, player
  name, session identifier, and Unix control socket;
- replaces physical keyboard/mouse input with bounded newline-delimited JSON
  actions received from the Unix socket;
- automatically releases movement and interaction controls at each action
  deadline.

Currently implemented control messages are `move`, `look`, `dig`, `attack`,
`place`, `use`, and `wait`. Inventory/craft/chat operations and complete action
result correlation are tracked as follow-up engine work.

## Branded desktop target

Configure with `-DBUILD_AICRAFT_CLIENT=ON` to produce `bin/aicraft`.
On macOS the install target produces a branded `AICraft.app` bundle with
bundle identifier `world.ai-craft.client`.
Its launcher exposes only AICraft match, exploration, world creation, and
observation entry points. Public server discovery, ContentDB, client mod
management, and general single-player administration remain upstream but are
not exposed by this target.
