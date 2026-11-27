# Multiplayer (co-op) design

Status: **design + prototype**. This document describes how multiplayer is built
on top of the single-player engine, and what is implemented so far
(see [Implementation status](#implementation-status)).

## 1. The problem

Cataclysm is a turn-based game built on a single clock. `calendar::turn`
advances one second per turn. Every `Creature` gets `moves` (normally 100 per
turn, scaled by speed), and every action costs moves. In combat this matters
a lot: who swings first, whether you can still retreat a tile, whether a reload
fits into this turn.

Outside combat the same clock is a burden. One player who starts clearing a
forest, reading a book or crafting runs an activity that lasts thousands of
turns. Single player hides this because nobody waits: the engine runs those
turns as fast as the CPU allows. With several humans, a naive design gives two
bad outcomes:

* **Strict lockstep.** Every turn waits for every player. The friend browsing
  the crafting menu freezes the world for the friend who is fighting. The friend
  chopping trees waits through every keystroke of the friend who walks around.
* **Plain real time.** The clock ticks at a fixed rate. This destroys tactical
  combat, which is the core of the game.

The goal is a clock that is **strict when it has to be and invisible when it
does not**.

## 2. Hard engine constraints

These facts decide the architecture. They are verified against the code, not
assumed.

| Constraint | Where | Consequence |
|---|---|---|
| One world clock | `calendar::turn`, `game::do_turn()` | All players share one time. Per-player clocks would need per-region simulation, so they are out of scope for v1. |
| One reality bubble | `get_map()`, `MAPSIZE = 11` (132x132 tiles, centred on the avatar) | Everything outside the bubble is frozen or simulated at overmap level. Players must stay inside the bubble (see §6, the tether). |
| One avatar | `get_avatar()` is used across the codebase | The host's own character stays the `avatar`. Remote players are other `Character`s. |
| Non-deterministic simulation | `rng()` everywhere, unordered containers, floating point | Lockstep or peer-to-peer simulation would desync. The design must be **host-authoritative**. |
| Turn loop blocks on host input | `game::do_turn()` → `handle_action()` → `get_player_input()` | The host must get a timeout, or an idle host freezes every client. The engine already has one (`TURN_DURATION`, `ACTION_TIMEOUT` → `pause()`). |
| NPCs are full characters | `class npc : public Character`, `process_npc_turn()` | A remote player can be an `npc` whose decision step reads queued network commands instead of running AI. Moves, activities, damage, saving and followers come for free. |
| Activities are serializable | `activity_actor::serialize()` / `deserialize()` | Long actions (crafting, chopping, reading, construction) can be sent over the network as the same JSON the save files use. |

## 3. Architecture

```
            +-------------------- host process ---------------------+
            |  game::do_turn()            authoritative world      |
            |   +- avatar (host player)                            |
 client A --+-> +- npc "remote A"  <-- mp_command queue            |
 client B --+-> +- npc "remote B"  <-- mp_command queue            |
            |   +- monsters, map, weather, items ...               |
            |                                                      |
            |  mp_session  : players, handshake, permissions       |
            |  mp_scheduler: tempo mode + turn barrier (§4)        |
            |  mp_transport: TCP, length-prefixed JSON frames (§5) |
            +------------------------------------------------------+
```

* **Host-authoritative listen server.** The host runs the real game. A headless
  dedicated server is the same thing without a local avatar (later milestone).
* **Remote players are puppet NPCs.** On join, the host spawns or loads the
  player's character as an `npc` with a `remote_controlled` marker. In
  `process_npc_turn()` the puppet does not call `npc::move()`. It pops one
  command from its queue and executes it. When the queue is empty and the puppet
  still has moves, the puppet is *not ready*, and the scheduler decides whether
  to wait or to auto-pause it.
* **Clients are thin, with a read-only mirror.** The client gets snapshots of
  its own character (the same JSON as the save format), the submaps that are
  visible to it, visible creatures and its message log. The client UI reads the
  mirror. It never changes the world: every action is sent to the host as a
  command, and the result comes back in the next snapshot.

### 3.1 Commands

The client sends intent, not state. Two kinds cover most of the game:

1. **Primitive actions.** These are small and need no item references:
   `move(dx,dy,dz)` (which also attacks, opens doors and so on, like
   `avatar_action::move`), `wait`, `pause`, `examine(pos)`, `pickup(pos)`,
   `fire(target)`, `reload`, `sleep`, `ready`.
2. **Activity assignment.** The client runs the normal UI (crafting menu,
   construction menu, butchery) against its mirror. Where single player would
   call `assign_activity( actor )`, the client sends `activity_actor::serialize()`.
   The host deserializes it, re-validates it against the real world (the item
   still exists, the tile is still free) and assigns it to the puppet.
   `item_location` already serializes as a position-relative reference, so item
   references survive the round trip.

The host validates every command. A command that is no longer valid (the target
died, the item was taken by another player) is rejected with a message. It is
never applied half-way.

## 4. The time model

### 4.1 Tempo modes

There is one clock and three ways to drive it. The **mode is chosen per tick**
from the state of all players (`mp_scheduler::compute_mode()`):

| Mode | When | How the clock moves |
|---|---|---|
| **TACTICAL** | Any player is in danger: a hostile is visible to them (the same test that drives safe mode, `mostseen`), they took damage recently, or they toggled tactical mode manually. | Strict turns. A turn ends only when every *input* player has spent their moves. A configurable grace timeout (default 30 s) auto-pauses an AFK player, so one disconnected or AFK friend cannot lock the game forever. |
| **EXPLORE** | Nobody is in danger, and at least one player is giving input. | Soft real time. A turn ends as soon as every input player has acted, **or** when the pace interval runs out (default 250 ms per turn). A player who does not act in time pauses for that turn, as `TURN_DURATION` already does in single player. Fast players are never throttled below their own key-repeat speed if everyone else is busy. |
| **FAST_FORWARD** | Every player is *busy*: in a long activity, asleep, waiting, travelling, or has pressed **ready**. | No waiting. Turns run back to back at engine speed, exactly like single player waiting or crafting. |

Each player is in one of these **player states** (`mp_player_state`), which the
host derives every turn:

* `input`: idle and expected to choose an action.
* `busy`: an activity, sleep, wait or auto-move consumes their moves. They need
  no input.
* `ready`: they explicitly said "skip time with me" (see §4.3).
* `danger`: `input`, and something hostile threatens them.

### 4.2 The turn barrier

The scheduler is one rule with a per-player deadline:

> A turn may end when every connected player is *done* for this turn.
> A player is done if they have no moves left, or they are busy or ready,
> or their deadline for this turn has passed (then they auto-pause).

The deadline depends on the mode:

```
deadline(player) = busy/ready   -> 0         (never waits)
                   FAST_FORWARD -> 0
                   EXPLORE      -> pace_ms              (default 250)
                   TACTICAL     -> tactical_grace_ms    (default 30000)
```

Inside one turn, commands are executed **in arrival order** while the player
still has moves. This mirrors single player, where the avatar spends all its
moves and then the monsters act. Any advantage from typing faster is limited to
one turn.

### 4.3 How this solves the long-activity problem

Example: Ala clears a forest (about 2000 turns). Bartek explores a house next to
her.

* Ala is `busy`, so she never holds the clock.
* Bartek is `input` in EXPLORE mode. The clock runs at Bartek's speed. Each of
  his moves ends a turn at once, and when he does nothing the clock still runs
  at 4 turns per second. Ala's 2000 turns finish in at most about 8 minutes of
  real time, often less. During that time Bartek played normally, and Ala can
  chat, plan or look at the map (looking costs no turns).
* If Bartek also wants to skip, he presses **ready** (or starts his own long
  activity). Both are busy, so the mode becomes FAST_FORWARD, and the rest of
  the forest is done in a second or two, exactly as in single player.
* If a zombie walks up to Bartek, his state becomes `danger` and the mode becomes
  TACTICAL. Ala's activity keeps running, one turn at a time and in step with
  Bartek's fight. If the zombie comes for Ala, the normal activity-distraction
  logic interrupts her, she becomes `danger` as well, and both fight with strict
  turns.

What players notice:

* Combat always feels like single player: the clock waits for you.
* Boring time never waits for menus or AFK friends, and it can be skipped
  together with one key.
* A status line shows the current mode and who the clock is waiting for, for
  example `TACTICAL - waiting for Bartek (12s)`.

### 4.4 Extras that make co-op pleasant

* **Group skip (`ready`).** "Wait 1 hour" or "sleep" by one player starts a
  vote. When all players are ready or busy, FAST_FORWARD starts. Danger to anyone
  cancels the vote.
* **Danger broadcast.** When the mode becomes TACTICAL, every player gets a
  message with the cause and the direction ("Bartek sees a zombie brute, 18
  tiles north-east"). Busy players can keep working, cancel, or come to help.
* **Catch-up cost.** In EXPLORE mode a player who missed a turn deadline pauses,
  which costs nothing beyond the lost turn. Nobody is punished for reading the
  wiki out of combat.
* **Per-player settings.** `pace_ms` and `tactical_grace_ms` are host options.
  A future option lets the host make TACTICAL wait forever (for hardcore groups)
  or use a short timer (for "fast co-op").

## 5. Network protocol

* **Transport:** TCP. The game is turn-based and needs reliable ordering more
  than low latency. One connection per client, non-blocking sockets, polled from
  the main thread (CDDA is single-threaded, so there are no locks).
* **Framing:** `uint32 big-endian length` + `UTF-8 JSON payload`. The maximum
  frame size is 16 MiB, so a bad peer cannot make the host allocate without
  limit. JSON reuses `JsonOut` and `json_loader`. Large frames (submaps) may be
  zstd-compressed later (zstd is already vendored in `src/third-party/zstd`).
* **Envelope:** `{"t": "<type>", "seq": N, ...}`.
* **Handshake:** the client sends `hello` with `proto` (protocol version), the
  game version, a mod-list hash and a player name. The host answers `welcome`
  (player id, world options) or `reject` (reason). A wrong protocol version or
  a mod mismatch is refused before any world data is sent.

| Type | Direction | Payload |
|---|---|---|
| `hello` | C→S | `proto`, `game_version`, `mods_hash`, `name` |
| `welcome` / `reject` | S→C | `player_id`, options / `reason` |
| `cmd` | C→S | `turn`, `seq`, `action` (+ params) or `activity` (actor JSON) |
| `cmd_ack` | S→C | `seq`, `ok`, optional `reason` |
| `tick` | S→C | `turn`, `mode`, `waiting_for` (player ids) |
| `self` | S→C | the player's own character JSON (when it changed) |
| `submap` | S→C | absolute position + submap JSON (on enter view or change) |
| `creatures` | S→C | the creatures visible to this player |
| `msg` | S→C | log lines that this player saw or heard |
| `chat` | both | text |
| `bye` | both | reason |

**Interest management:** a client only receives what its character perceives,
which uses the host's normal vision and sound code. This keeps bandwidth low and
stops cheating with map reveal.

**Security:** the host never trusts the client. Commands are intents. Every
field is validated (ranges, ids, item ownership) before use. Frame size is
capped, and so are queue length and command rate.

## 6. The tether (v1 limitation)

There is only one reality bubble, so players must stay inside it. The bubble
follows the **party centroid** instead of the host avatar, with the existing
`map::shift` logic. A player who walks to within 6 tiles of the bubble edge gets
a warning. They cannot leave it on foot, and a vehicle that would drive out
stops with a message ("You can't leave your group behind").

v2 can lift this by running more than one bubble, with one `map` per player
group and the time model above for each group. That needs `get_map()` to stop
being a singleton, so it is a large refactor and is deferred.

## 7. Save games

The host's world is the save. Each remote player's character is saved like an
NPC, tagged with their stable player id (a UUID kept on the client). On rejoin
the host finds the puppet by id and binds the connection to it again. When a
client disconnects, its puppet stays in the world in `npc_suspend` (it does not
act and does not hold the clock) until the player returns or the host removes
it.

## 8. Milestones

1. **Scheduler (done).** `mp_scheduler`: tempo modes, the barrier and deadlines.
   Pure logic with unit tests (`tests/mp_scheduler_test.cpp`).
2. **Protocol + transport (done).** `mp_protocol` (framing, envelope, handshake
   validation) and `mp_transport` (POSIX TCP, non-blocking), with a loopback test.
3. **Host loop integration.** Remote puppets in `process_npc_turn()`, the barrier
   in `game::do_turn()`, and the scheduler feeding `TURN_DURATION` for the host
   input. All behind the `MULTIPLAYER` option, so single player is unchanged.
4. **Thin client.** Mirror state, primitive commands, then activity commands.
5. **Persistence and rejoin.** Puppet binding by player id.
6. **Polish.** Status line, danger broadcast, chat, group-skip voting.
7. **v2.** Multiple bubbles, dedicated headless server.

## Implementation status

| Component | Files | State |
|---|---|---|
| Scheduler | `src/mp_scheduler.{h,cpp}` | Implemented, unit-tested |
| Protocol | `src/mp_protocol.{h,cpp}` | Implemented, unit-tested |
| Transport | `src/mp_transport.{h,cpp}` | POSIX implemented, loopback-tested. On Windows it compiles to a stub that reports "unsupported". |
| Host integration | – | Not started |
| Client | – | Not started |

## License

Cataclysm: Dark Days Ahead is licensed under CC-BY-SA 3.0. This work is a
derivative and is distributed under the same license.
