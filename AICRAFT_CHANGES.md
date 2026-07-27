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
  name, and Unix control socket;
- replaces physical keyboard/mouse input with bounded newline-delimited JSON
  actions received from the Unix socket;
- automatically releases movement and interaction controls at each action
  deadline.

Currently implemented control messages are `move`, `look`, `dig`, `attack`,
`place`, `use`, and `wait`. Inventory/craft/chat operations and complete action
result correlation are tracked as follow-up engine work.

