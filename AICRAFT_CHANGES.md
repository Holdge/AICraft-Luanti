# AICraft-Luanti modifications

This fork is based on Luanti 5.16.1 commit
`5ebd9b57984d5854e0a37fd0125da48e9e59e190` and remains licensed under
LGPL-2.1-or-later.

Modification notice date: 2026-07-27.

The upstream project is <https://github.com/luanti-org/luanti>. AICraft
modifications are maintained as source code in this fork; the complete
corresponding source for a binary release is the commit recorded in the
artifact's `AICRAFT_RELEASE.json` and its matching `*-source.tar.gz`.

## Agent client target

Configure with `-DBUILD_AGENT_CLIENT=ON` to produce
`bin/aicraft-agent-client`.

The target:

- retains the upstream Client, LocalPlayer, collision, inventory, interaction,
  and UDP protocol code;
- selects Irrlicht's Null video driver and disables sound;
- skips the main menu and rejects startup without a server address/UDP port, player
  name, account credential supplied through `--password-file`, session
  identifier, Unix control socket, and a session secret supplied through
  `--agent-session-secret-file`;
- reads both `--password-file` and `--agent-session-secret-file` only from
  current-user-owned, regular, non-symlink files with permissions exactly
  `0600`, never logs their contents, and rejects plaintext `--password`;
- reports the internal `BOOTING`, `AUTHENTICATING`, `MEDIA_SYNC`,
  `PLAYER_READY`, and `CONTROL_READY` lifecycle, and sends `agent.hello` only
  after the LocalPlayer and all initial media/definitions are ready and the
  server has acknowledged the peer as an active Player;
- includes `session_id` and `session_secret` in the hello, and does not accept
  actions until the control service returns a matching `agent.registered`;
- replaces physical keyboard/mouse input with bounded newline-delimited JSON
  actions received from the Unix socket;
- supports `move`, `look`, `dig`, `place`, `use`, `attack`, `craft`, `equip`,
  `chat`, `wait`, `inventory_move`, `container_move`, and `formspec_submit`
  through the standard Luanti client control, interaction, inventory, and
  formspec code paths;
- resolves node targets by position and optional face, resolves visible
  entities through session-local `object:<id>` identifiers, and never
  hard-codes Mineclonia node or item identifiers in the engine;
- emits `accepted`, then `executing`, then exactly one `succeeded`, `rejected`,
  or `expired` action status. Invalid, duplicate, busy, or unauthenticated
  actions are rejected before execution;
- waits for bounded observable client/server evidence for networked
  interactions, including authoritative node updates, inventory replies,
  object state, chat echo, or formspec changes, instead of reporting success
  when a request is merely sent;
- optionally publishes a bounded 5 Hz generic client observation after
  `agent.registered` opts in with `observation_stream: true`. It includes
  player state, all player inventory lists, nearby entity identifiers, the
  pointed node/entity, and the active formspec/container. Large creative
  catalogs and formspecs are capped so they cannot exhaust the control socket;
- automatically releases movement and interaction controls after every
  terminal action status;
- omits the render-target pipeline and frame drawing while retaining the
  network, client simulation, collision, and PlayerControl loops.

The production invocation contract is:

```sh
bin/aicraft-agent-client \
  --address <game-host> \
  --port <game-udp-port> \
  --name <leased-player-name> \
  --password-file <0600-ticket-file> \
  --agent-control-socket <unix-socket-path> \
  --agent-session <session-id> \
  --agent-session-secret-file <0600-session-secret-file> \
  --go
```

`--go` is accepted for an explicit launch command; the Agent target also
forces direct connection and never opens the main menu.

Mineclonia-specific recipe lookup, objective state, stable referee entity IDs,
and ranked-match authority remain adapter/referee responsibilities. The
generic client can execute a `craft` action when the requested output is
already present in Luanti's `craftpreview`; a server adapter is required to
prepare arbitrary catalog recipes. A referee acknowledgement is also the
preferred stronger confirmation for formspec submissions and actions such as
movement, look, and equip that the upstream wire protocol does not explicitly
acknowledge.

`util/aicraft/mineclonia-agent-smoke.sh` runs the real headless client against
the pinned Mineclonia server and verifies authenticated lifecycle transitions,
monotonic client observations, continuous server `observation.sampled` events,
chat, inventory movement, equip, use, place, dig, look, move, and wait without
Lua errors.

## Branded desktop target

Configure with `-DBUILD_AICRAFT_CLIENT=ON` to produce `bin/aicraft`.
On macOS the install target produces a branded `AICraft.app` bundle with
bundle identifier `world.ai-craft.client`.
Its launcher exposes only AICraft match, exploration, world creation, and
observation entry points. Public server discovery, ContentDB, client mod
management, and general single-player administration remain upstream but are
not exposed by this target.

All four entries are platform connections and use login-only credentials:

- Free Explore connects to an AICraft Mineclonia exploration lease and no
  longer suggests choosing an arbitrary password on first visit.
- Create World connects to a platform-issued Mineclonia World Create lease;
  it does not create a local `aicraft_game` single-player world.
- Join Match and Observe Match remain ticketed server connections. Observer
  read-only behavior is enforced by the authoritative referee mod, not by a
  client-only UI restriction.

## Release artifacts and signing

`.github/workflows/aicraft-release.yml` is the canonical release build. It
builds and tests the Linux authoritative server, Linux headless Agent client,
and branded macOS desktop clients for arm64 and x86_64. Every package contains:

- the exact 40-character source commit and upstream Luanti 5.16.1 base;
- the build arguments and dependency provenance used for that target;
- this modification notice and the applicable LGPL text;
- an explicit code-signing and notarization state.

Linux is distributed as a root filesystem archive compiled for
`/opt/aicraft/engine`. Extract it at `/`; it contains both `luantiserver` and
`aicraft-agent-client`. The release also publishes `SHA256SUMS` and
`BUILD_PROVENANCE.json` covering every binary and corresponding-source
artifact.

macOS Developer ID signing is enabled only when all
`MACOS_CERTIFICATE_P12_BASE64`, `MACOS_CERTIFICATE_PASSWORD`, and
`MACOS_SIGNING_IDENTITY` repository secrets are present. Notarization is
enabled only when the three additional App Store Connect key secrets are
present. Without those secrets the workflow applies only an explicit ad-hoc
signature for macOS launchability, records `ad-hoc-only` and
`notarized: false`, and never describes that artifact as production-signed or
notarized.
