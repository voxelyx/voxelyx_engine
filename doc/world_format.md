# Voxelyx World Format

This describes the format of a world whose blocks have the serialization
version 22...29.

The block serialization version does not fully specify every aspect of this
format; if compliance with this format is to be checked, it needs to be
done by detecting if the files and data indeed follows it.

# Files

Everything is contained in a directory, the name of which is freeform, but
often serves as the name of the world.

The accounts and the state of the players are not in the world directory of a
server that runs several worlds, they are in the user directory (see
[Worlds on a server](#worlds-on-a-server)). A world that is run on its own
(e.g. in singleplayer) keeps `accounts.sqlite` in its directory. The ban data is stored on a per-world basis. It can be copied over
from an old world to a newly created world.

    World
    ├── accounts.sqlite ─ Accounts and players (only a world run on its own)
    ├── env_meta.txt ──── Environment metadata
    ├── ipban.txt ─────── Banned IPs/users
    ├── map_meta.txt ──── Map metadata
    ├── map.sqlite ────── Map data
    └── world.conf ────── World metadata

## `accounts.sqlite`

Contains the accounts and, for each world, the state of the players in it: who
they are, how they log in, what they may do, which world they are in, and where
they are in each world, their health, inventory and metadata.
Set `auth_backend` in main.conf to choose where this is kept (`sqlite3`,
`postgresql`, or `dummy` for nothing at all). The PostgreSQL database has
the same tables.

The schema is versioned (`PRAGMA user_version`, currently 1). Times are Unix
time stamps (UTC) in INTEGER columns. Foreign keys are enforced, and what is
owned by a row goes with it (`ON DELETE CASCADE`):

| Table | One row for | Columns |
|---|---|---|
| `worlds` | a world | `id` (the `world_id` of world.conf), `name` (informational) |
| `accounts` | an account | `id`, `name` (unique), `password`, `created_at`, `last_login` (NULL: never), `current_world_id` |
| `privileges` | a privilege of an account | `account_id`, `privilege` (together the key) |
| `world_players` | an account in a world | `id`, `account_id`, `world_id` (unique together), `pitch`, `yaw`, `pos_x`, `pos_y`, `pos_z`, `hp`, `breath`, `created_at`, `updated_at` |
| `world_player_metadata` | a key of such a player | `world_player_id`, `key`, `value` (bytes) |
| `world_player_inventories` | an inventory list of such a player | `world_player_id`, `list_index`, `name`, `width`, `size` |
| `world_player_inventory_items` | an occupied slot of such a list | `world_player_id`, `list_index`, `slot`, `item` (a serialized item stack) |

* `accounts.password` is `#1#<salt>#<verifier>` with the parts inside `<>` in
  base64. `<verifier>` is an RFC 2945 compatible SRP verifier of the given salt,
  password, and the player's name lowercased, using the 2048-bit group specified
  in RFC 5054 and the SHA-256 hash function. An empty password is an account
  without one. `!` is an account that can't be logged into with the built-in
  authentication handler: it is made for a player that a mod's own
  authentication handler lets in, so that there is a place for their state.
* `accounts.current_world_id` is the world a player is in, or last was in. It
  is where they log in the next time. It is NULL if there is none yet or the
  world was deleted.
* Deleting an account deletes its privileges and its players in every world.
  Deleting a world deletes the players in it. This is all done by the database.
* Only the inventory slots that hold something have a row.
* `world_players.pitch`/`yaw` are in degrees, the position is that of the
  player's feet in node coordinates (not scaled by BS).

## `env_meta.txt`

Simple global environment variables.

Example content:

    game_time = 73471
    time_of_day = 19118
    EnvArgsEnd

## `ipban.txt`

Banned IP addresses and usernames.

Example content:

    123.456.78.9|foo
    123.456.78.10|bar

## `map_meta.txt`

Simple global map variables.

Example content:

    seed = 7980462765762429666
    [end_of_params]

## `map.sqlite`

Map data.

See [Map File Format](#map-file-format) below.

## `world.conf`

World metadata, in the same format as `main.conf`. This file is created and
updated by Voxelyx. It only describes the world itself: what is the same for all
worlds of a server (the game, the mods, the accounts, announcing the server ...)
is in `main.conf`.

Every world has a `world_id`, a positive number that is unique among the worlds
of a worlds directory. It is assigned when the world is created (1 for the
first world, then 2, 3, ...). A world without a valid `world_id` can't be
loaded.

Games and mods may store their own settings in this file too. Such settings are
not documented here; see the documentation for the game or mod that owns them.
Custom keys should be namespaced to avoid conflicts with Voxelyx and other
content. The convention is `<modname>.<setting>`.

    world_id = 1                  - unique number of the world, the first world is 1
    world_name = Sol III          - name of the world (if not set, the world folder name will be used)
                                    shown in the main menu and used as a technical identifier
    static_spawnpoint = (0,8,0)   - where players start, chosen when the world is first started
    enable_damage = true          - optional, overrides enable_damage of main.conf in this world
    creative_mode = false         - optional, overrides creative_mode of main.conf in this world
    backend = sqlite3             - which DB backend to use for blocks (sqlite3, dummy, leveldb, redis, postgresql)
    readonly_backend = sqlite3    - optionally read-only seed DB (DB file _must_ be located in "readonly" subfolder)
    mod_storage_backend = sqlite3 - which DB backend to use for mod storage
    blocksize = 32                - mapblock size, only written if it's non-standard (16)

`PostgreSQL` backend specific settings:

    pgsql_connection = host=127.0.0.1 port=5432 user=mt_user password=mt_password dbname=voxelyx
    pgsql_mod_storage_connection = (same parameters as above)

`Redis` backend specific settings:

    redis_address = 127.0.0.1  - Redis server address
    redis_hash = foo           - Database hash
    redis_port = 6379          - (optional) Connection port
    redis_password = hunter2   - (optional) Server password

# Worlds on a server

A dedicated server runs **all worlds in the `worlds` directory at the same
time**. Each world is stepped on its own thread with its own Lua state, so they
run in parallel. Players can play in different worlds concurrently and be moved
between worlds without reconnecting. A server with one world works the same
way. Singleplayer and the "Host Server" tab of the main menu run exactly one.

```
voxelyxserver                                  # runs all worlds
voxelyxserver --world <default world>          # selects the default world for this run
```

If there is no world yet, `worlds/main` is created. Worlds are managed with the
`worlds` mod, one of the base mods. It provides the commands `/worlds`,
`/world <ID or name>`, `/tpworld <player> <ID or name>` (or
`<player> <W> <X> <Y> <Z>`), `/createworld` and `/removeworld`, and shares chat
between worlds.

## World IDs and coordinates

Every world has a **name** (the folder in `worlds`) and an **ID**, a number that
is unique on the server. The ID is written to the world's `world.conf` as
`world_id` when the world is created: the first world gets 1, the next 2 and so
on (one more than the highest ID there is, also counting removed worlds, so an
ID is not given out twice). The default world, `main`, is normally the first
one created and so has ID 1.

A position on the server is written **(W, X, Y, Z)**, with W the world ID. The
`pos` of the F5 debug text, the positions in the messages of chat commands and
the `/teleport` command use it, e.g. `/teleport 2,0,10,0` moves you to 0,10,0 in
world 2. Anywhere a world is asked for, the ID can be used instead of the name
(`/world 2`, `core.transfer_player_to_world(player, 2)`), so a world name can't
consist of digits only.

A world without a valid `world_id` is skipped (the default world: the server
doesn't start), and two worlds with the same ID make the server refuse to start.
To copy a world folder in, change its `world_id` to an unused number.

## Configuration

Server settings are in `main.conf`, in the user directory or given with
`--config`.

| Setting                          | Meaning                                                                                    |
|----------------------------------|--------------------------------------------------------------------------------------------|
| `default_world`                  | Name of the world in which new players start. Created if it doesn't exist. Default: `main` |
| `worlds_dir`                     | Directory with the worlds. Default: `<user dir>/worlds`                                    |
| `world_list`                     | Comma separated list of worlds to run. Default: all that have a `world.conf`               |
| `auth_backend`                   | Where the accounts and the players are kept: `sqlite3` (default), `postgresql`             |
| `creative_mode`, `enable_damage` | For all worlds, unless a world overrides them                                              |

`default_world` is added to `main.conf` when the server starts for the first
time. `--world <path>` (or `--worldname`) selects the default world for this run
instead. It doesn't have to be in the worlds directory, and it is created if it
doesn't exist yet.

`world.conf` only has what belongs to the world itself, see
[`world.conf`](#worldconf). Everything about the server (accounts, announcing,
mods ...) is in `main.conf`. In the Lua API, `core.get_world_setting(name)`
returns the value of a setting for the current world: the one in its
`world.conf`, else the one in `main.conf`.

Seamless moves between two worlds need **identical content**: same nodes, items
and media. This is the normal case, since all worlds run the same mods (see
[Content groups](#content-groups)). Don't enable `random_mod_load_order`.
`--terminal` is connected to the default world.

## Creating and removing worlds while the server runs

Mods can create and remove worlds without a restart, see `core.create_world()`
and `core.remove_world()` in `lua_api.md`. The `worlds` mod exposes them as
`/createworld <name>` and `/removeworld <ID or name> [delete]`, for players with
the `world_manage` privilege.

- A new world is made like the first one: with the server's settings
  (`mg_name`, seed, ...) and mods. It is a folder in the worlds directory, so it
  is also there after a restart.
- Removing first moves the players of the world to the default world (those
  whose client can't switch worlds on an open connection reconnect and arrive
  there; anybody still left after 30 seconds is kicked). The world is then
  stopped and saved. Its folder is moved to `<worlds dir>/.removed/<name>-<time>`,
  or deleted with `delete`. The default world can't be removed.
- Both happen in the background, other worlds keep running. Mods learn about the
  result from `core.register_on_worlds_changed`.
- Neither is possible while `world_list` fixes the list of worlds.

## What belongs to the server and what to a world

Worlds are separate, but the server has one set of mods, one set of accounts,
one set of commands and one ban list.

| Per world                                                                      | Server-wide                                            |
|--------------------------------------------------------------------------------|--------------------------------------------------------|
| Map, mapgen, seed, emerge threads                                              | Network port and connection                            |
| Lua state                                                                      | **Mods**                                               |
| Players' position and meta (inventory and health follow the player, see below) | **Accounts**: passwords, privileges, last login        |
| Mod storage, `core.get_worldpath()`, ABMs, timers                              | **Chat commands and privileges** (defined by the mods) |
| Chat, `core.get_connected_players()`                                           | The ban list (`<user dir>/ipban.txt`)                  |
|                                                                                | `core.settings` / `main.conf`, `max_users`             |
|                                                                                | The `core.ipc_*` store, mod data dir                   |

**Mods** are server-wide. Every mod in the `mods` directory (`<user dir>/mods`,
`<share dir>/mods`, `VOXELYX_MOD_PATH`) is loaded by all worlds, so all worlds
register the same nodes, items, privileges and chat commands. To turn a mod off,
set `load_mod_<name> = false` in `main.conf`.

**Accounts** are the same everywhere: log in once, with the same password and
the same privileges, in whichever world you are. Granting or revoking a
privilege takes effect in all worlds right away. Commands registered by mods
exist in every world. Administrative ones work on the whole server: `/kick` and
`/ban` reach players in other worlds, `/status` lists everybody, `/grant` and
`/revoke` need no world, `/tpworld` moves players whichever world they are in,
and `max_users` is the limit for the whole server. Commands that are about the
world one is in (`/time`, `/give`, `/clearobjects`, ...) act on that world.

**Players:** apart from the account, a player has an independent position and
meta in each world. The first time they enter a world they are a new player
there (`on_newplayer`). The inventory and the health (HP and breath) go with the
player: when they are moved from one world to another (also when they have to
reconnect to get there), what they had on leaving replaces what the new world
has. A player who is dead is not moved as a dead one, they keep the health the
new world has. So items are never lost or duplicated by changing worlds.
Logging out and in again doesn't move anything.

`/remove_player` removes the player's data of the current world; removing the
account (`core.remove_player_auth`) removes it for all worlds. The world a
player is in is part of their account, and they log in there next time. A world
that is removed with `/removeworld <world> delete` takes the players in it with
it; one that is only moved away keeps them, and players who were in it log in to
the default world until it is back. Remove worlds with `/removeworld` or
`core.remove_world`, not by deleting the directory: the server then doesn't know
the world is gone, and a new world that gets the same ID would start with the
players of the old one.

## Lua API

See "Worlds" in `lua_api.md`: `core.get_world_id()`, `core.get_world_name()`,
`core.get_worlds()`, `core.transfer_player_to_world()`, `core.send_to_world()`,
`core.register_on_world_message()` and the extra arguments of `on_joinplayer` /
`on_leaveplayer`.

## How it works

![One UDP socket feeds the ClusterReceive thread, which routes packets to the inbox of each world thread. Each world has its own Lua state, map, emerge threads and databases and sends directly to the socket. All worlds share the accounts database, ban list, main.conf and IPC store.](worlds-architecture.svg)

- Every world is an ordinary `Server`. It gets a `WorldConnection` that looks
  like its own connection but is fed by the `WorldCluster`. Sending goes straight
  to the shared (thread-safe) connection. Worlds never wait for each other.
- The accounts are one `SharedAuthDatabase`: a single database whose calls are
  serialized by a mutex, used by all worlds. Nothing else is called while the
  mutex is held. Each world has its own connection to the same file for its
  players.
- The first packet of a connection (`TOSERVER_INIT`) carries the player name. It
  decides which world the connection starts in.
- Cross-world actions are tasks posted to the target world and run on its own
  thread under its environment lock. No lock spans two worlds. This is also how
  privilege changes, kicks and bans reach the world a player is in.

### Moving a player

1. A mod calls `core.transfer_player_to_world`. The request is validated and
   posted to the world the player is in.
2. That world runs `on_leaveplayer`, saves and removes the player, and detaches
   the connection state from its client list. The network connection stays open.
3. Packets of that connection are re-routed to the target world, which attaches
   the client and sends `TOCLIENT_WORLD_CHANGE`.
4. The client drops everything that belongs to the old world (map, objects,
   particles, sounds, HUD) and answers with `TOSERVER_CLIENT_READY`, as at the
   end of joining.
5. The target world runs the normal join sequence: loads or creates the player,
   applies the requested position, resends sky/HUD/lighting state, runs
   `on_joinplayer`.

If the player disconnects at any point the half-moved client is discarded; the
player's data was already saved by the source world.

### Content groups

Map blocks refer to nodes by content ID, so a client can only keep its
definitions when the new world agrees on them. The server therefore

- assigns node content IDs through a registry shared by all worlds, so a node
  name has the same ID in every world, even though Lua loads nodes in a
  different order in each state, and
- hashes the node definitions, item definitions and media of each world at
  startup. Worlds with equal hashes form a content group, and only inside a
  group are seamless moves possible.

Since all worlds run the same mods, they normally form a single group, unless a
mod registers things depending on the world it runs in. For anything else, and
for clients that don't support world changes, the player is asked to reconnect
and will arrive in the target world. The requested position is not kept in that
case.

## Limitations

- One fatal error (e.g. an uncaught Lua error) in any world stops the whole
  server. A shutdown request from any world shuts down all.
- `core.get_connected_players()`, `core.get_player_by_name()` and chat only see
  the world they are called in. Private messages (`/msg`) work within a world;
  the `worlds` mod shares normal chat between worlds.
- Privilege changes are read-modify-write on the account. Two worlds changing
  the privileges of the same player at the same instant can lose one change.
- `core.dynamic_add_media` files are not shared between worlds.
- Minimap modes set by mods are not re-sent when entering a world.
- The `--recompress` option works on one world, the one selected with `--world`.
- Worlds are loaded one after another at startup (mod loading isn't parallel).

# Map File Format

Voxelyx maps consist of `MapBlock`s, each holds 16x16x16 nodes.

In addition to the bulk node data, `MapBlock`s stored on disk also contain
other things.

## `map.sqlite`
`map.sqlite` is an `SQLite3` database, containing a single table, called
`blocks`. It looks like this:

```sql
CREATE TABLE `blocks` (
    `x` INTEGER, `y` INTEGER, `z` INTEGER,
    `data` BLOB NOT NULL,
    PRIMARY KEY (`x`, `z`, `y`)
);
```

Before 5.12.0 it looked like this:

```sql
CREATE TABLE `blocks` (`pos` INT NOT NULL PRIMARY KEY, `data` BLOB);
```

## Position Encoding

Applies to the pre-5.12.0 schema:

`pos` (a node position encoding) is created from the three coordinates of a
`MapBlock` using the following simple equation:

```C
pos = (z << 24) + (y << 12) + x;
```
or, equivalently, `pos = (z * 0x1000000) + (y * 0x1000) + x`.

A position can be decoded using:

```C
pos = pos + 0x800800800;
x = (pos & 0xFFF) - 0x800;
y = ((pos >> 12) & 0xFFF) - 0x800;
z = ((pos >> 24) & 0xFFF) - 0x800;
```

Positions are sequential along the x axis (as easily seen from the position equation above).
It is possible to retrieve all blocks from an interval using the following SQL statement:

```sql
SELECT
`pos`,
`data`,
( (`pos` + 0x800800800)        & 0xFFF) - 0x800 as x,
(((`pos` + 0x800800800) >> 12) & 0xFFF) - 0x800 as y,
(((`pos` + 0x800800800) >> 24) & 0xFFF) - 0x800 as z
FROM `blocks` WHERE
( (`pos` + 0x800800800)        & 0xFFF) - 0x800 >= ? AND -- minx
( (`pos` + 0x800800800)        & 0xFFF) - 0x800 <= ? AND -- maxx
(((`pos` + 0x800800800) >> 12) & 0xFFF) - 0x800 >= ? AND -- miny
(((`pos` + 0x800800800) >> 12) & 0xFFF) - 0x800 <= ? AND -- maxy
`pos` >= (? << 24) - 0x800800 AND -- minz
`pos` <= (? << 24) + 0x7FF7FF; -- maxz
```

## Blob

The blob is the data that would have otherwise gone into the file.

See below for description.

# MapBlock Serialization Format

> **Notes**:
>  * NOTE: Byte order is MSB first (big-endian).
>  * NOTE: Zlib data is in such a format that Python's `zlib` at least can
>          directly decompress.
>  * NOTE: Since version 29 zstd is used instead of zlib. In addition, the
>          **entire block** is first serialized and then compressed (except version byte).

`u8` version
* map format version number, see serialization.h for the latest number

`u8` flags
* Flag bitmasks:
    * `0x01`: `is_underground`: Should be set to 0 if there will be no light
      obstructions above the block. If/when sunlight of a block is updated
      and there is no block above it, this value is checked for determining
      whether sunlight comes from the top.

    * `0x02`: `day_night_differs`: Whether the lighting of the block is different
      on day and night. Only blocks that have this bit set are updated when
      day transforms to night.

    * `0x04`: `lighting_expired`: Not used in version 27 and above. If true,
      lighting is invalid and should be updated. If you can't calculate
      lighting in your generator properly, you could try setting this 1 to
      everything and setting the uppermost block in every sector as
      `is_underground=0`. It most likely won't work properly.

    * `0x08`: `generated`: True if the block has been generated. If false, block
      is mostly filled with `CONTENT_IGNORE` and is likely to contain e.g. parts
      of trees of neighboring blocks.

`u16` lighting_complete
* Added in version 27.

* This contains 12 flags, each of them corresponds to a direction.

* Indicates if the light is correct at the sides of a map block.
  Lighting may not be correct if the light changed, but a neighbor
  block was not loaded at that time.
  If these flags are false, Voxelyx will automatically recompute light
  when both this block and its required neighbor are loaded.

* The bit order is:
  nothing,  nothing,  nothing,  nothing,
  night X-, night Y-, night Z-, night Z+, night Y+, night X+,
  day X-,   day Y-,   day Z-,   day Z+,   day Y+,   day X+.
  Where 'day' is for the day light bank, 'night' is for the night
  light bank.
  The 'nothing' bits should be always set, as they will be used
  to indicate if direct sunlight spreading is finished.

* Example: if the block at `(0, 0, 0)` has `lighting_complete = 0b1111111111111110`,
  Voxelyx will correct lighting in the day light bank when the block at
  `(1, 0, 0)` is also loaded.

Timestamp and node ID mappings come here if map format version >= 29.
* `u32` timestamp
    * Timestamp when last saved, as seconds from starting the game.
    * `0xffffffff` = invalid/unknown timestamp, nothing should be done with the time
                     difference when loaded

* `u8` `name_id_mapping_version`
    * Should be zero for map format version 29.

* `u16` `num_name_id_mappings`
    * foreach `num_name_id_mappings`:
        * `u16` `id`
        * `u16` `name_len`
        * `u8[name_len]` `name`

`u8` content_width
* Number of bytes in the content (`param0`) fields of nodes
* 1 byte before map format version 24, 2 bytes since

`u8` params_width
* Number of bytes used for parameters per node
* Always 2

## Node Data
> **Note**: Zlib-compressed before map format version 29

* If `content_width` is 1:
    * `u8[4096]`: `param0` fields
    * `u8[4096]`: `param1` fields
    * `u8[4096]`: `param2` fields

* If `content_width` is 2:
    * `u16[4096]`: `param0` fields
    * `u8[4096]`: `param1` fields
    * `u8[4096]`: `param2` fields

* The location of a node in each of those arrays is `(z*16*16 + y*16 + x)`.

### Node Metadata List
> **Note**: Zlib-compressed before map version format 29
* Before map format version 23:
    * `u16` version (=1)
    * `u16` count of metadata
    * foreach count:
        * `u16` position (`(p.Z*MAP_BLOCKSIZE*MAP_BLOCKSIZE + p.Y*MAP_BLOCKSIZE + p.X)`)
        * `u16` type_id
        * `u16` content_size
        * `u8[content_size]` content of metadata. Format depends on `type_id`, see below.

* Since map format version 23:
    * `u8` version
        > **Note**: Type was `u16` before map format version 23
        * = 1 before map format version 28
        * = 2 since map format version 28
    * `u16` count of metadata
    * foreach count:
        * `u16` `position` (`(p.Z*MAP_BLOCKSIZE*MAP_BLOCKSIZE + p.Y*MAP_BLOCKSIZE + p.X)`)
        * `u32` `num_vars`
        * foreach `num_vars`:
            * `u16` `key_len`
            * `u8[key_len]` `key`
            * `u32 val_len`
            * `u8[val_len]` `value`
            * `u8` `is_private`
            * only since map format version 2. 0 = not private, 1 = private
        * serialized inventory

## Node Timers
* Map format version 23:
    * `u8` unused version (always 0)

* Map format version 24:
    > **Note**: Not released as stable
    * `u8` `nodetimer_version`
    * if `nodetimer_version` == 1:
        * `u16` `num_of_timers`
        * foreach `num_of_timers`:
            * `u16` timer position (`(z*16*16 + y*16 + x)`)
            * `s32` timeout * 1000
            * `s32` elapsed * 1000

* Map format version >= 25: see below

`u8` static object version:
* Always 0

`u16` `static_object_count`

foreach `static_object_count`:
* `u8` type (object type-id)
* `s32` `pos_x_nodes` * 10000
* `s32` `pos_y_nodes` * 10000
* `s32` `pos_z_nodes` * 10000
* `u16` `data_size`
* `u8[data_size]` `data`

Before map format version 29:
* `u32` `timestamp`
    * Same meaning as the timestamp further up

* `u8` `name-id-mapping` version
    * Always 0

* `u16` `num_name_id_mappings`
    * foreach `num_name_id_mappings`:
        * `u16` `id`
        * `u16` `name_len`
        * `u8[name_len]` `name`

Since map format version 25, node timers come here:
    * `u8` length of the data of a single timer (always 2+4+4=10)
    * `u16` `num_of_timers`
    * foreach `num_of_timers`:
        * `u16` timer position (`(z*16*16 + y*16 + x)`)
        * `s32` timeout * 1000
        * `s32` elapsed * 1000

End of File (EOF).

# Format of Nodes

A node is composed of the `u8` fields `param0`, `param1` and `param2`.

Before map format version 24:
* The content id of a node is determined as so:
    * If `param0` < `0x80`, `content_id = param0`
    * Otherwise, `content_id = (param0<<4) + (param2>>4)`

Since map format version 24:
* The content id of a node is `param0`.

The purpose of `param1` and `param2` depend on the definition of the node.

# Name-ID-Mapping

The mapping maps node content IDs to node names.

# Node Metadata Format (Before Map Format Version 23)

The node metadata is serialized depending on the `type_id` field.

`1`: Generic metadata
* serialized inventory
* `u32` `len`
* `u8[len]` `text`
* `u16` `len`
* `u8[len]` `owner`
* `u16` `len`
* `u8[len]` `infotext`
* `u16` `len`
* `u8[len]` inventory drawspec
* `u8` `allow_text_input` (bool)
* `u8` `removal_disabled` (bool)
* `u8` `enforce_owner` (bool)
* `u32` `num_vars`
* foreach `num_vars`:
    * `u16` `len`
    * `u8[len]` `name`
    * `u32` `len`
    * `u8[len]` `value`

`14`: Sign metadata
* `u16` `text_len`
* `u8[text_len]` `text`

`15`: Chest metadata
* serialized inventory

`16`: Furnace metadata
* To be determined

`17`: Locked Chest metadata
* `u16` `len`
* `u8[len]` `owner`
* serialized inventory

# Static Objects

Static objects are persistent freely moving objects in the world.

Object types:
`1`: Test object
`7`: LuaEntity:
* `u8` `compatibility_byte` (always 1)
* `u16` `len`
* `u8[len]` entity name
* `u32` `len`
* `u8[len]` static data
* `s16` `hp`
* `s32` velocity.x * 10000
* `s32` velocity.y * 10000
* `s32` velocity.z * 10000
* `s32` yaw * 1000

Since protocol version 37:
* `u8` `version2` (=1 or 2)
* `s32` pitch * 1000
* `s32` roll * 1000
* if version2 >= 2:
  * `u8[16]` guid

# Itemstring Format

Examples:
* `'default:dirt 5'`
* `'default:pick_wood 21323'`
* `'"default:apple" 2'`
* `'default:apple'`

Older formats:
* `'node "default:dirt" 5'`
* `'NodeItem default:dirt 5'`
* `'ToolItem WPick 21323'`

The wear value in tools is 0...65535.

# Inventory Serialization Format

* The inventory serialization format is line-based.
* The newline character used is `\n`
* The end condition of a serialized inventory is always `EndInventory\n`
* All the slots in a list must always be serialized.

## Example

    List foo 4
    Item default:sapling
    Item default:sword_stone 1 10647
    Item default:dirt 99
    Empty
    EndInventoryList
    List bar 9
    Empty
    Empty
    Empty
    Empty
    Empty
    Empty
    Empty
    Empty
    Empty
    EndInventoryList
    EndInventory
