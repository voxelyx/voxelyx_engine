# Worlds on a dedicated server

A dedicated server runs **all worlds in the `worlds` directory at the same time**. Each world is stepped on **its own thread with its own Lua state**, so they run truly in parallel. Players can play in different worlds concurrently and be moved between worlds **without reconnecting**.

Start the server with the following. If there is no world yet, `worlds/main` is created:

```
voxelyxserver                                  # runs all worlds
voxelyxserver --world <default world>          # selects the default world for this run
```

Worlds are managed with the `worlds` mod, one of the base mods. It provides the commands `/worlds`, `/world <ID or name>`, `/tpworld <player> <ID or name>` (or `<player> <W> <X> <Y> <Z>`), `/createworld` and `/removeworld`, and shares chat between worlds.

To add more worlds, create them with the main menu or `/createworld` and restart the server, or create them while the server runs, see [Creating and removing worlds while the server runs](#creating-and-removing-worlds-while-the-server-runs).

## World IDs and coordinates

Every world has a **name** (the folder in `worlds`) and an **ID**, a number that is unique on the server. The ID is written to the world's `world.conf` as `world_id` when the world is created: the first world gets **1**, the next **2** and so on (one more than the highest ID there is, also counting removed worlds, so an ID is not given out twice). The default world, `main`, is normally the first one created and so has ID 1.

A position on the server is written **(W, X, Y, Z)**, with W the world ID. The `pos` of the F5 debug text, the positions in the messages of chat commands and the `/teleport` command use it, e.g. `/teleport 2,0,10,0` moves you to 0,10,0 in world 2.

Anywhere a world is asked for, the ID can be used instead of the name (`/world 2`, `core.transfer_player_to_world(player, 2)`). A world name can therefore not consist of digits only.

A world without a valid `world_id` is skipped (the default world: the server doesn't start), and two worlds with the same ID make the server refuse to start. To copy a world folder in, change its `world_id` to an unused number.

## Configuration

### main.conf

Server settings are in `main.conf`, in the user directory or given with `--config`.

| Setting                          | Meaning                                                                                    |
|----------------------------------|--------------------------------------------------------------------------------------------|
| `default_world`                  | Name of the world in which new players start. Created if it doesn't exist. Default: `main` |
| `multiworld_dir`                 | Directory with the worlds. Default: `<user dir>/worlds`                                    |
| `multiworld_worlds`              | Comma separated list of worlds to run. Default: all that have a `world.conf`               |
| `auth_backend`                   | Where the accounts of all players are kept: `sqlite3` (default), `files`, ...              |
| `creative_mode`, `enable_damage` | For all worlds, unless a world overrides them                                              |

`default_world` is added to `main.conf` when the server starts for the first time (or when it isn't there yet). `--world <path>` (or `--worldname`) selects the default world for this run instead. It doesn't have to be in the worlds directory, and it is created if it doesn't exist yet.

### world.conf

Only what belongs to the world itself. Everything about the server (accounts, announcing, mods ...) is in `main.conf`.

| Key                                                | Meaning                                                                                                                                                                                      |
|----------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `world_id`, `world_name`                           | The ID (see above) and the display name                                                                                                                                                      |
| `backend`, `player_backend`, `mod_storage_backend` | How the data of the world is stored                                                                                                                                                          |
| `static_spawnpoint`                                | Where players start, `(x, y, z)`. Chosen when the world is first started, edit it to move the spawn point. Players that are moved to a world without being given a position arrive there too |
| `creative_mode`, `enable_damage`                   | Optional. They override the value of `main.conf` in this world only                                                                                                                          |

In the Lua API, `core.get_world_setting(name)` returns the value of a setting for the current world: the one in its `world.conf`, else the one in `main.conf`.

### Requirements and restrictions

- For seamless moves between two worlds they must have **identical content**: same nodes, items and media. This is the normal case, since all worlds run the same mods (see [Content groups](#content-groups)). Don't enable `random_mod_load_order`.
- Singleplayer and the "Host Server" tab of the main menu run exactly one world.
- `--terminal` is connected to the default world.

## Creating and removing worlds while the server runs

Mods can create and remove worlds without a restart, see `core.create_world()` and `core.remove_world()` in `lua_api.md`. The `worlds` mod exposes them as `/createworld <name>` and `/removeworld <ID or name> [delete]`, for players with the `world_manage` privilege.

- A new world is made like the first one: with the server's settings (`mg_name`, seed, ...) and mods. It is a folder in the worlds directory, so it is also there after a restart.
- Removing first moves the players of the world to the default world (those whose client can't switch worlds on an open connection reconnect and arrive there; anybody still left after 30 seconds is kicked). The world is then stopped and saved. Its folder is moved to `<worlds dir>/.removed/<name>-<time>`, or deleted with `delete`. The default world can't be removed.
- Both happen in the background, other worlds keep running. Mods learn about the result from `core.register_on_worlds_changed`.
- Neither is possible while `multiworld_worlds` fixes the list of worlds.

## What belongs to the server and what to a world

Worlds are separate, but the **server** has one set of mods, one set of accounts, one set of commands and one ban list.

| Per world                                                                      | Server-wide                                            |
|--------------------------------------------------------------------------------|--------------------------------------------------------|
| Map, mapgen, seed, emerge threads                                              | Network port and connection                            |
| Lua state                                                                      | **Mods**                                               |
| Players' position and meta (inventory and health follow the player, see below) | **Accounts**: passwords, privileges, last login        |
| Mod storage, `core.get_worldpath()`, ABMs, timers                              | **Chat commands and privileges** (defined by the mods) |
| Chat, `core.get_connected_players()`                                           | The ban list (`<user dir>/ipban.txt`)                  |
|                                                                                | `core.settings` / `main.conf`, `max_users`             |
|                                                                                | The `core.ipc_*` store, mod data dir                   |

### Mods

Mods are server-wide. **Every mod** in the `mods` directory (`<user dir>/mods`, `<share dir>/mods`, `VOXELYX_MOD_PATH`) is loaded by all worlds, so all worlds register the same nodes, items, privileges and chat commands. The base mods are in the `mods` directory of the Voxelyx source tree.

To turn a mod off, set this in `main.conf`:

```
load_mod_mymod = false
load_mod_othermod = false
```

### Accounts, privileges and commands

An account is the same everywhere: log in once, with the same password and the same privileges, in whichever world you are. Accounts are kept in one database in the user directory (`auth.sqlite`, or as set by `auth_backend`). Granting or revoking a privilege takes effect in all worlds right away, also for a player who is currently in another world.

Commands registered by mods exist in every world. Administrative ones work on the whole server: `/kick` and `/ban` reach players in other worlds, `/status` lists everybody, `/grant` and `/revoke` need no world, `/tpworld` moves players whichever world they are in, and `max_users` is the limit for the whole server. Commands that are about the world one is in (`/time`, `/give`, `/clearobjects`, ...) act on that world.

### Player data

Apart from the account, a player has an independent position and meta in each world. The first time they enter a world they are a *new player* there (`on_newplayer`).

**The inventory and the health (HP and breath) go with the player.** When they are moved from one world to another (also when they have to reconnect to get there), what they had on leaving replaces what the new world has. A player who is dead is not moved as a dead one, they keep the health the new world has. So items are never lost or duplicated by changing worlds. Logging out and in again doesn't move anything: the inventory is saved with the world the player is in.

`/remove_player` removes the player's data of the current world; removing the account (`core.remove_player_auth`) removes it for all worlds.

The world a player was last in is remembered in `<user dir>/multiworld_players.txt` and they log in there next time.

## Lua API

See "Multiworld" in `lua_api.md`: `core.get_world_id()`, `core.get_world_name()`, `core.get_worlds()`, `core.transfer_player_to_world()`, `core.send_to_world()`, `core.register_on_world_message()` and the extra arguments of `on_joinplayer` / `on_leaveplayer`.

## How it works

![Multiworld architecture: one UDP socket feeds the ClusterReceive thread, which routes packets to the inbox of each world thread. Each world has its own Lua state, map, emerge threads and databases and sends directly to the socket. All worlds share the auth database, ban list, main.conf and IPC store.](multiworld-architecture.svg)

- Every world is an ordinary `Server`. It gets a `WorldConnection` that looks like its own connection but is fed by the cluster. Sending goes straight to the shared (thread-safe) connection. Worlds never wait for each other.
- The accounts are one `SharedAuthDatabase`: a single database whose calls are serialized by a mutex, used by the environments of all worlds. Nothing else is called while the mutex is held.
- The first packet of a connection (`TOSERVER_INIT`) carries the player name. It decides which world the connection starts in.
- Cross-world actions are **tasks** posted to the target world and run on its own thread under its environment lock. No lock spans two worlds. This is also how privilege changes, kicks and bans reach the world a player is in.

### Moving a player

1. A mod calls `core.transfer_player_to_world`. The request is validated and posted to the world the player is in.
2. That world runs `on_leaveplayer`, saves and removes the player, and detaches the connection state from its client list. The network connection stays open.
3. Packets of that connection are re-routed to the target world, which attaches the client and sends `TOCLIENT_WORLD_CHANGE`.
4. The client drops everything that belongs to the old world (map, objects, particles, sounds, HUD) and answers with `TOSERVER_CLIENT_READY`, as at the end of joining.
5. The target world runs the normal join sequence: loads or creates the player, applies the requested position, resends sky/HUD/lighting state, runs `on_joinplayer`.

If the player disconnects at any point the half-moved client is discarded; the player's data was already saved by the source world.

### Content groups

Map blocks refer to nodes by content ID, so a client can only keep its definitions when the new world agrees on them. The server therefore

- assigns node content IDs through a cluster-wide registry, so a node name has the same ID in every world, even though Lua loads nodes in a different order in each state, and
- hashes the node definitions, item definitions and media of each world at startup. Worlds with equal hashes form a content group, and only inside a group are seamless moves possible.

Since all worlds run the same mods, they normally form a single group, unless a mod registers things depending on the world it runs in. For anything else, and for clients that don't support world changes, the player is asked to reconnect and will arrive in the target world. The requested position is not kept in that case.

## Limitations

- One fatal error (e.g. an uncaught Lua error) in any world stops the whole server. A shutdown request from any world shuts down all.
- `core.get_connected_players()`, `core.get_player_by_name()` and chat only see the world they are called in. Private messages (`/msg`) work within a world; the `worlds` mod shares normal chat between worlds.
- Privilege changes are read-modify-write on the account. Two worlds changing the privileges of the same player at the same instant can lose one change.
- `core.dynamic_add_media` files are not shared between worlds.
- Minimap modes set by mods are not re-sent when entering a world.
- The `--recompress` option works on one world, the one selected with `--world`.
- Worlds are loaded one after another at startup (mod loading isn't parallel).
