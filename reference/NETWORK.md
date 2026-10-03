# The extender's network protocol on the Linux build

`src/net.cpp` carries bg3se's messages over the game's own connection, so a
bg3le host serves clients running bg3se (Windows, or Windows under Proton)
and bg3le, and a bg3le client joins either kind of host. It is a port of
bg3se's `Extender/Shared/ExtenderNet.cpp`, `Client/ClientNetworking.cpp` and
`Server/ServerNetworking.cpp`; this file records where each engine detail
came from in v4.76.31.656, so the next build can be checked against it.

## What travels

- Message ID 400 (`NETMSG_SCRIPT_EXTENDER`): a 32-bit length, then a
  protobuf `MessageWrapper` (`vendor/bg3se/.../ExtenderProtocol.proto`),
  encoded by hand with `src/pb.h`. A payload over 0xfffff bytes is sent as
  length 0, as upstream does.
- A client appends `" Extender_0"` to `ClientConnectMessage::Build` ("Gold"
  in this build, so "Gold Extender_0": 15 characters, still the string's
  inline form). The server answers that with an `ExtenderHello` carrying its
  protocol version (2, the binary serializer); the client replies with its
  own. Neither side sends message 400 before that: a peer without the
  extender reads pool 400 out of bounds and crashes.
- `MsgPostLuaMessage` carries `Ext.Net` traffic: channel, payload, module
  UUID, request and reply IDs, and whether the payload is `Ext.Json`'s binary
  form. `MsgUserVars` carries `Ext.Vars` syncs (tables as `binaryval`, as
  upstream sends them; `luaval` JSON is accepted). The server sends
  `MsgS2CResetLuaMessage` to remote clients after its own reset.

Messages between the host's server and its own client never touch the
network: they go through bg3le's in-process queue, as upstream's local
message passing does. `Ext._Internal.NetForceRemote(true)` (server context
first, then client) turns that off, so single player exercises the whole
network path over the engine's loopback.

## Engine structures, and where they were read

| What | Linux | Evidence |
|---|---|---|
| `net::Message` header | 0x28 bytes, `MsgId` at +8 | the registration function builds each template: `movabs rcx,0x400000001; mov [rax+8],rcx` (ID, Reliability 4) |
| message vtable | D1, D0, Serialize, Unknown, CreateNew, Reset | ClientConnectMessage's vtable (0x79280b8): slot 1 jumps to `operator delete`, slot 4 allocates 0xa0 and constructs |
| `BitstreamSerializer` | `IsWriting` at +8; WriteBytes slot 2, ReadBytes slot 3 | ClientConnectMessage::Serialize: `cmp [rbx+8],0; sete al; call [rcx+rax*8+0x10]` |
| `MessageFactory::Register(factory, id, tmpl, growSize, name)` | 0x3ce78f0 | called 324 times from the registration function 0x3cdf1b0 |
| `MessageFactory::GetFreeMessage(factory, id)` | 0x2380220; pools at +8, count at +0x14, lock (pthread mutex) at +0x20, depth at +0x48 | its body |
| `AbstractPeer::NetMessageFactory` | +0x240 | both peers' init: `mov rdi,[rbx+0x240]; call RegisterAll` |
| `AbstractPeer::ProtocolList` | `Array<Protocol*>` at +0x330 | client init's AddProtocol pushes there; `AbstractPeer::ProcessMsg` (vtable slot 27, 0x3360030) walks it, stopping on Handled or Abort |
| `Protocol` | vptr, `Peer` at +8; D1, D0, ProcessMsg(unused, context, msg), PreUpdate, PostUpdate, OnAddedToHost, OnRemovedFromHost, Reset | ecl::JoinProtocol's vtable (0x79fa048) |
| `MessageContext::UserID` | +0, peer in the high 16 bits | JoinProtocol reads `word [context+2]` as the peer |
| `SendMessageSinglePeer(peer, peerId, msg)` | vtable slot 25, 0x2b06370 | GameServer's vtable (0x7a88198); the server sends HOST_WELCOME through `[vtable+0xc8]` |
| `SendMessageMultiPeer` | slot 26 (unused here) | the overload pair is reversed from bg3se's MSVC order |
| `GameClient::HostPeerId` | +0x428 | every client send passes `[client+0x428]` |
| `EoCClient::GameClient` | +0x90 | `mov rbx,[EoCClient+0x90]` before client sends |
| `GameServer::ActivePeerIds` | +0x6e0 | `GameServer::ActivatePeer` ("GameServer Peer Activate: %d") pushes there |
| `GameServer::PeerInfo` | `LegacyRefMap` at +0x720 | ActivatePeer sets `Flags` through it |
| `GameServer::LocalPeerId` | +0x744 | past PeerInfo and CharacterOwners, as bg3se orders them; read only if it holds 1..0xffff, else 1 |
| `ClientConnectMessage::Build` | +0x38 (16-byte string) | the client's connect code assigns "Gold" there |

The four engine functions are `target::Net*` signatures (`src/targets.cpp`).
At runtime each peer is checked before anything is written to it: its
factory's pools 1 and 6 hold messages 1 and 6, every protocol in its list
names it as `Peer`, and its vtable slot 25 is `SendMessageSinglePeer`. A
peer that fails turns the network off with one warning; `Ext.Net` then stays
local to the machine.

## Threads

Each peer is extended from its own update: the server from its
`UpdateMessagesToSend` tick, the client from its state machine's frame.
`ProcessMsg` runs on the peer's thread too, and only queues what arrives for
the Lua context; the context drains it on its next tick.
