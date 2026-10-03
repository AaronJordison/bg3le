// The Script Extender's network protocol over the game's own connection, as
// bg3se's (Extender/Shared/ExtenderNet, Client/ClientNetworking,
// Server/ServerNetworking), so bg3le talks to Windows peers running bg3se and
// to other bg3le peers alike.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace bg3le {

// One user or mod variable, as bg3se's MsgUserVars carries it.
struct NetUserVar {
    enum class Kind { Null, Bool, Int, Double, String, Json, Binary };
    bool module = false;  // MODULE_VAR, else ENTITY_VAR
    std::string guid;     // the entity's or the module's UUID
    std::string key;
    Kind kind = Kind::Null;
    bool b = false;
    std::int64_t i = 0;
    double d = 0;
    std::string s;  // String, Json and Binary
};

// What a Lua context is handed on its next tick: a post from the other
// context in this process or from a peer, a variable sync, or a reset.
struct NetInbound {
    enum class Kind { Post, UserVars, ResetLua };
    Kind kind = Kind::Post;
    std::string channel;
    std::string payload;
    std::string module;
    bool hasPayload = true;
    std::int64_t user = 0;
    std::uint32_t requestId = 0;
    std::uint32_t replyId = 0;
    bool binary = false;
    std::vector<NetUserVar> vars;
};

// Queues a message for the server (or client) context. (lua_host.cpp)
void lua_net_deliver(bool server, NetInbound message);
bool lua_state_is_client(lua_State* L);

// Each peer's own update, on its own thread: the server's from its tick,
// the client's from its frame. Installs the extender message and protocol.
void net_server_tick(void* gameServer);
void net_client_tick();

// Whether this process runs the game's server: its tick ran lately.
bool net_hosting();

// The host's own client as a user ID ((peer << 16) | slot).
std::int64_t net_local_user();

// After the host's reset: tells clients on other machines to reset theirs.
void net_reset_remote_clients();

// Ext._Internal.NetInfo / NetPost / NetSyncVars.
int l_net_info(lua_State* L);
int l_net_post(lua_State* L);
int l_net_sync_vars(lua_State* L);
int l_net_force_remote(lua_State* L);

}  // namespace bg3le
