/*************************************************************************/
/*  scene_replication_interface.cpp                                      */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "scene_replication_interface.h"

#include "scene_multiplayer.h"

#include "core/debugger/engine_debugger.h"
#include "core/io/marshalls.h"
#include "scene/main/node.h"
#include "scene/scene_string_names.h"

#define MAKE_ROOM(m_amount)             \
	if (packet_cache.size() < m_amount) \
		packet_cache.resize(m_amount);

#ifdef DEBUG_ENABLED
_FORCE_INLINE_ void SceneReplicationInterface::_profile_node_data(const String &p_what, ObjectID p_id, int p_size) {
	if (EngineDebugger::is_profiling("multiplayer:replication")) {
		Array values;
		values.push_back(p_what);
		values.push_back(p_id);
		values.push_back(p_size);
		EngineDebugger::profiler_add_frame_data("multiplayer:replication", values);
	}
}
#endif

SceneReplicationInterface::TrackedNode &SceneReplicationInterface::_track(const ObjectID &p_id) {
	if (!tracked_nodes.has(p_id)) {
		tracked_nodes[p_id] = TrackedNode(p_id);
		Node *node = get_id_as<Node>(p_id);
		node->connect(SceneStringNames::get_singleton()->tree_exited, callable_mp(this, &SceneReplicationInterface::_untrack).bind(p_id), Node::CONNECT_ONE_SHOT);
	}
	return tracked_nodes[p_id];
}

void SceneReplicationInterface::_untrack(const ObjectID &p_id) {
	if (!tracked_nodes.has(p_id)) {
		return;
	}
	uint32_t net_id = tracked_nodes[p_id].net_id;
	uint32_t peer = tracked_nodes[p_id].remote_peer;
	tracked_nodes.erase(p_id);
	// If it was spawned by a remote, remove it from the received nodes.
	if (peer && peers_info.has(peer)) {
		peers_info[peer].recv_nodes.erase(net_id);
	}
	// If we spawned or synced it, we need to remove it from any peer it was sent to.
	if (net_id || peer == 0) {
		for (KeyValue<int, PeerInfo> &E : peers_info) {
			E.value.spawn_nodes.erase(p_id);
		}
	}
}

void SceneReplicationInterface::_free_remotes(const PeerInfo &p_info) {
	for (const KeyValue<uint32_t, ObjectID> &E : p_info.recv_nodes) {
		Node *node = tracked_nodes.has(E.value) ? get_id_as<Node>(E.value) : nullptr;
		ERR_CONTINUE(!node);
		node->queue_free();
	}
}

void SceneReplicationInterface::on_peer_change(int p_id, bool p_connected) {
	if (p_connected) {
		peers_info[p_id] = PeerInfo();
		for (const ObjectID &oid : spawned_nodes) {
			_update_spawn_visibility(p_id, oid);
		}
		for (const ObjectID &oid : sync_nodes) {
			_update_sync_visibility(p_id, get_id_as<MultiplayerSynchronizer>(oid));
		}
	} else {
		ERR_FAIL_COND(!peers_info.has(p_id));
		_free_remotes(peers_info[p_id]);
		peers_info.erase(p_id);
	}
}

void SceneReplicationInterface::on_reset() {
	for (const KeyValue<int, PeerInfo> &E : peers_info) {
		_free_remotes(E.value);
	}
	peers_info.clear();
	// Tracked nodes are cleared on deletion, here we only reset the ids so they can be later re-assigned.
	for (KeyValue<ObjectID, TrackedNode> &E : tracked_nodes) {
		TrackedNode &tobj = E.value;
		tobj.net_id = 0;
		tobj.remote_peer = 0;
	}
	for (const ObjectID &oid : sync_nodes) {
		MultiplayerSynchronizer *sync = get_id_as<MultiplayerSynchronizer>(oid);
		ERR_CONTINUE(!sync);
		sync->reset();
	}
	last_net_id = 0;
}

void SceneReplicationInterface::on_network_process() {
	uint64_t msec = OS::get_singleton()->get_ticks_msec();
	for (KeyValue<int, PeerInfo> &E : peers_info) {
		const HashSet<ObjectID> to_sync = E.value.sync_nodes;
		if (to_sync.is_empty()) {
			continue; // Nothing to sync
		}
		uint16_t sync_net_time = ++E.value.last_sent_sync;
		_send_sync(E.key, to_sync, sync_net_time, msec);
	}
}

Error SceneReplicationInterface::on_spawn(Object *p_obj, Variant p_config) {
	Node *node = Object::cast_to<Node>(p_obj);
	ERR_FAIL_COND_V(!node || p_config.get_type() != Variant::OBJECT, ERR_INVALID_PARAMETER);
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(p_config.get_validated_object());
	ERR_FAIL_COND_V(!spawner, ERR_INVALID_PARAMETER);
	// Track node.
	const ObjectID oid = node->get_instance_id();
	TrackedNode &tobj = _track(oid);
	ERR_FAIL_COND_V(tobj.spawner != ObjectID(), ERR_ALREADY_IN_USE);
	tobj.spawner = spawner->get_instance_id();
	spawned_nodes.insert(oid);

	if (multiplayer->has_multiplayer_peer() && spawner->is_multiplayer_authority()) {
		if (tobj.net_id == 0) {
			tobj.net_id = ++last_net_id;
		}
		_update_spawn_visibility(0, oid);
	}
	return OK;
}

Error SceneReplicationInterface::on_despawn(Object *p_obj, Variant p_config) {
	Node *node = Object::cast_to<Node>(p_obj);
	ERR_FAIL_COND_V(!node || p_config.get_type() != Variant::OBJECT, ERR_INVALID_PARAMETER);
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(p_config.get_validated_object());
	ERR_FAIL_COND_V(!p_obj || !spawner, ERR_INVALID_PARAMETER);
	// Forcibly despawn to all peers that knowns me.
	int len = 0;
	Error err = _make_despawn_packet(node, len);
	ERR_FAIL_COND_V(err != OK, ERR_BUG);
	const ObjectID oid = p_obj->get_instance_id();
	for (const KeyValue<int, PeerInfo> &E : peers_info) {
		if (!E.value.spawn_nodes.has(oid)) {
			continue;
		}
		_send_raw(packet_cache.ptr(), len, E.key, true);
	}
	// Also remove spawner tracking from the replication state.
	ERR_FAIL_COND_V(!tracked_nodes.has(oid), ERR_INVALID_PARAMETER);
	TrackedNode &tobj = _track(oid);
	ERR_FAIL_COND_V(tobj.spawner != spawner->get_instance_id(), ERR_INVALID_PARAMETER);
	tobj.spawner = ObjectID();
	spawned_nodes.erase(oid);
	for (KeyValue<int, PeerInfo> &E : peers_info) {
		E.value.spawn_nodes.erase(oid);
	}
	return OK;
}

Error SceneReplicationInterface::on_replication_start(Object *p_obj, Variant p_config) {
	Node *node = Object::cast_to<Node>(p_obj);
	ERR_FAIL_COND_V(!node || p_config.get_type() != Variant::OBJECT, ERR_INVALID_PARAMETER);
	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config.get_validated_object());
	ERR_FAIL_COND_V(!sync, ERR_INVALID_PARAMETER);

	// Add to synchronizer list.
	TrackedNode &tobj = _track(p_obj->get_instance_id());
	const ObjectID sid = sync->get_instance_id();
	tobj.synchronizers.insert(sid);
	sync_nodes.insert(sid);

	// Update visibility.
	sync->connect("visibility_changed", callable_mp(this, &SceneReplicationInterface::_visibility_changed).bind(sync->get_instance_id()));
	_update_sync_visibility(0, sync);

	if (pending_spawn == p_obj->get_instance_id() && sync->get_multiplayer_authority() == pending_spawn_remote) {
		// Try to apply synchronizer Net ID
		ERR_FAIL_COND_V_MSG(pending_sync_net_ids.is_empty(), ERR_INVALID_DATA, vformat("The MultiplayerSynchronizer at path \"%s\" is unable to process the pending spawn since it has no network ID. This might happen when changing the multiplayer authority during the \"_ready\" callback. Make sure to only change the authority of multiplayer synchronizers during \"_enter_tree\" or the \"_spawn_custom\" callback of their multiplayer spawner.", sync->get_path()));
		ERR_FAIL_COND_V(!peers_info.has(pending_spawn_remote), ERR_INVALID_DATA);
		uint32_t net_id = pending_sync_net_ids[0];
		pending_sync_net_ids.pop_front();
		peers_info[pending_spawn_remote].recv_sync_ids[net_id] = sync->get_instance_id();

		// Try to apply spawn state (before ready).
		if (pending_buffer_size > 0) {
			ERR_FAIL_COND_V(!node || sync->get_replication_config().is_null(), ERR_UNCONFIGURED);
			int consumed = 0;
			const List<NodePath> props = sync->get_replication_config()->get_spawn_properties();
			Vector<Variant> vars;
			vars.resize(props.size());
			Error err = MultiplayerAPI::decode_and_decompress_variants(vars, pending_buffer, pending_buffer_size, consumed);
			ERR_FAIL_COND_V(err, err);
			if (consumed > 0) {
				pending_buffer += consumed;
				pending_buffer_size -= consumed;
				err = MultiplayerSynchronizer::set_state(props, node, vars);
				ERR_FAIL_COND_V(err, err);
			}
		}
	}
	return OK;
}

Error SceneReplicationInterface::on_replication_stop(Object *p_obj, Variant p_config) {
	Node *node = Object::cast_to<Node>(p_obj);
	ERR_FAIL_COND_V(!node || p_config.get_type() != Variant::OBJECT, ERR_INVALID_PARAMETER);
	MultiplayerSynchronizer *sync = Object::cast_to<MultiplayerSynchronizer>(p_config.get_validated_object());
	ERR_FAIL_COND_V(!sync, ERR_INVALID_PARAMETER);
	sync->disconnect("visibility_changed", callable_mp(this, &SceneReplicationInterface::_visibility_changed));
	// Untrack synchronizer.
	const ObjectID oid = node->get_instance_id();
	const ObjectID sid = sync->get_instance_id();
	ERR_FAIL_COND_V(!tracked_nodes.has(oid), ERR_INVALID_PARAMETER);
	TrackedNode &tobj = _track(oid);
	tobj.synchronizers.erase(sid);
	sync_nodes.erase(sid);
	for (KeyValue<int, PeerInfo> &E : peers_info) {
		E.value.sync_nodes.erase(sid);
		if (sync->get_net_id()) {
			E.value.recv_sync_ids.erase(sync->get_net_id());
		}
	}
	return OK;
}

void SceneReplicationInterface::_visibility_changed(int p_peer, ObjectID p_sid) {
	MultiplayerSynchronizer *sync = get_id_as<MultiplayerSynchronizer>(p_sid);
	ERR_FAIL_COND(!sync); // Bug.
	Node *node = sync->get_root_node();
	ERR_FAIL_COND(!node); // Bug.
	const ObjectID oid = node->get_instance_id();
	if (spawned_nodes.has(oid)) {
		_update_spawn_visibility(p_peer, oid);
	}
	_update_sync_visibility(p_peer, sync);
}

Error SceneReplicationInterface::_update_sync_visibility(int p_peer, MultiplayerSynchronizer *p_sync) {
	ERR_FAIL_COND_V(!p_sync, ERR_BUG);
	if (!multiplayer->has_multiplayer_peer() || !p_sync->is_multiplayer_authority()) {
		return OK;
	}

	const ObjectID &sid = p_sync->get_instance_id();
	bool is_visible = p_sync->is_visible_to(p_peer);
	if (p_peer == 0) {
		for (KeyValue<int, PeerInfo> &E : peers_info) {
			// Might be visible to this specific peer.
			bool is_visible_to_peer = is_visible || p_sync->is_visible_to(E.key);
			if (is_visible_to_peer == E.value.sync_nodes.has(sid)) {
				continue;
			}
			if (is_visible_to_peer) {
				E.value.sync_nodes.insert(sid);
			} else {
				E.value.sync_nodes.erase(sid);
			}
		}
		return OK;
	} else {
		ERR_FAIL_COND_V(!peers_info.has(p_peer), ERR_INVALID_PARAMETER);
		if (is_visible == peers_info[p_peer].sync_nodes.has(sid)) {
			return OK;
		}
		if (is_visible) {
			peers_info[p_peer].sync_nodes.insert(sid);
		} else {
			peers_info[p_peer].sync_nodes.erase(sid);
		}
		return OK;
	}
}

Error SceneReplicationInterface::_update_spawn_visibility(int p_peer, const ObjectID &p_oid) {
	const TrackedNode *tnode = tracked_nodes.getptr(p_oid);
	ERR_FAIL_COND_V(!tnode, ERR_BUG);
	MultiplayerSpawner *spawner = get_id_as<MultiplayerSpawner>(tnode->spawner);
	Node *node = get_id_as<Node>(p_oid);
	ERR_FAIL_COND_V(!node || !spawner || !spawner->is_multiplayer_authority(), ERR_BUG);
	ERR_FAIL_COND_V(!tracked_nodes.has(p_oid), ERR_BUG);
	const HashSet<ObjectID> synchronizers = tracked_nodes[p_oid].synchronizers;
	bool is_visible = true;
	for (const ObjectID &sid : synchronizers) {
		MultiplayerSynchronizer *sync = get_id_as<MultiplayerSynchronizer>(sid);
		ERR_CONTINUE(!sync);
		if (!sync->is_multiplayer_authority()) {
			continue;
		}
		// Spawn visibility is composed using OR when multiple synchronizers are present.
		if (sync->is_visible_to(p_peer)) {
			is_visible = true;
			break;
		}
		is_visible = false;
	}
	// Spawn (and despawn) when needed.
	HashSet<int> to_spawn;
	HashSet<int> to_despawn;
	if (p_peer) {
		ERR_FAIL_COND_V(!peers_info.has(p_peer), ERR_INVALID_PARAMETER);
		if (is_visible == peers_info[p_peer].spawn_nodes.has(p_oid)) {
			return OK;
		}
		if (is_visible) {
			to_spawn.insert(p_peer);
		} else {
			to_despawn.insert(p_peer);
		}
	} else {
		// Check visibility for each peers.
		for (const KeyValue<int, PeerInfo> &E : peers_info) {
			if (is_visible) {
				// This is fast, since the the object is visible to everyone, we don't need to check each peer.
				if (E.value.spawn_nodes.has(p_oid)) {
					// Already spawned.
					continue;
				}
				to_spawn.insert(E.key);
			} else {
				// Need to check visibility for each peer.
				_update_spawn_visibility(E.key, p_oid);
			}
		}
	}
	if (to_spawn.size()) {
		int len = 0;
		_make_spawn_packet(node, spawner, len);
		for (int pid : to_spawn) {
			ERR_CONTINUE(!peers_info.has(pid));
			int path_id;
			multiplayer->get_path_cache()->send_object_cache(spawner, pid, path_id);
			_send_raw(packet_cache.ptr(), len, pid, true);
			peers_info[pid].spawn_nodes.insert(p_oid);
		}
	}
	if (to_despawn.size()) {
		int len = 0;
		_make_despawn_packet(node, len);
		for (int pid : to_despawn) {
			ERR_CONTINUE(!peers_info.has(pid));
			peers_info[pid].spawn_nodes.erase(p_oid);
			_send_raw(packet_cache.ptr(), len, pid, true);
		}
	}
	return OK;
}

Error SceneReplicationInterface::_send_raw(const uint8_t *p_buffer, int p_size, int p_peer, bool p_reliable) {
	ERR_FAIL_COND_V(!p_buffer || p_size < 1, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!multiplayer->has_multiplayer_peer(), ERR_UNCONFIGURED);

	Ref<MultiplayerPeer> peer = multiplayer->get_multiplayer_peer();
	peer->set_transfer_channel(0);
	peer->set_transfer_mode(p_reliable ? MultiplayerPeer::TRANSFER_MODE_RELIABLE : MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
	return multiplayer->send_command(p_peer, p_buffer, p_size);
}

Error SceneReplicationInterface::_make_spawn_packet(Node *p_node, MultiplayerSpawner *p_spawner, int &r_len) {
	ERR_FAIL_COND_V(!multiplayer || !p_node || !p_spawner, ERR_BUG);

	const ObjectID oid = p_node->get_instance_id();
	const TrackedNode *tnode = tracked_nodes.getptr(oid);
	ERR_FAIL_COND_V(!tnode, ERR_INVALID_PARAMETER);

	uint32_t nid = tnode->net_id;
	ERR_FAIL_COND_V(!nid, ERR_UNCONFIGURED);

	// Prepare custom arg and scene_id
	uint8_t scene_id = p_spawner->find_spawnable_scene_index_from_object(oid);
	bool is_custom = scene_id == MultiplayerSpawner::INVALID_ID;
	Variant spawn_arg = p_spawner->get_spawn_argument(oid);
	int spawn_arg_size = 0;
	if (is_custom) {
		Error err = MultiplayerAPI::encode_and_compress_variant(spawn_arg, nullptr, spawn_arg_size, false);
		ERR_FAIL_COND_V(err, err);
	}

	// Prepare spawn state.
	List<NodePath> state_props;
	List<uint32_t> sync_ids;
	const HashSet<ObjectID> synchronizers = tnode->synchronizers;
	for (const ObjectID &sid : synchronizers) {
		MultiplayerSynchronizer *sync = get_id_as<MultiplayerSynchronizer>(sid);
		if (!sync->is_multiplayer_authority()) {
			continue;
		}
		ERR_CONTINUE(!sync);
		ERR_FAIL_COND_V(sync->get_replication_config().is_null(), ERR_BUG);
		for (const NodePath &prop : sync->get_replication_config()->get_spawn_properties()) {
			state_props.push_back(prop);
		}
		// Ensure the synchronizer has an ID.
		if (sync->get_net_id() == 0) {
			sync->set_net_id(++last_net_id);
		}
		sync_ids.push_back(sync->get_net_id());
	}
	int state_size = 0;
	Vector<Variant> state_vars;
	Vector<const Variant *> state_varp;
	if (state_props.size()) {
		Error err = MultiplayerSynchronizer::get_state(state_props, p_node, state_vars, state_varp);
		ERR_FAIL_COND_V_MSG(err != OK, err, "Unable to retrieve spawn state.");
		err = MultiplayerAPI::encode_and_compress_variants(state_varp.ptrw(), state_varp.size(), nullptr, state_size);
		ERR_FAIL_COND_V_MSG(err != OK, err, "Unable to encode spawn state.");
	}

	// Encode scene ID, path ID, net ID, node name.
	int path_id = multiplayer->get_path_cache()->make_object_cache(p_spawner);
	CharString cname = p_node->get_name().operator String().utf8();
	int nlen = encode_cstring(cname.get_data(), nullptr);
	MAKE_ROOM(1 + 1 + 4 + 4 + 4 + 4 * sync_ids.size() + 4 + nlen + (is_custom ? 4 + spawn_arg_size : 0) + state_size);
	uint8_t *ptr = packet_cache.ptrw();
	ptr[0] = (uint8_t)SceneMultiplayer::NETWORK_COMMAND_SPAWN;
	ptr[1] = scene_id;
	int ofs = 2;
	ofs += encode_uint32(path_id, &ptr[ofs]);
	ofs += encode_uint32(nid, &ptr[ofs]);
	ofs += encode_uint32(sync_ids.size(), &ptr[ofs]);
	ofs += encode_uint32(nlen, &ptr[ofs]);
	for (uint32_t snid : sync_ids) {
		ofs += encode_uint32(snid, &ptr[ofs]);
	}
	ofs += encode_cstring(cname.get_data(), &ptr[ofs]);
	// Write args
	if (is_custom) {
		ofs += encode_uint32(spawn_arg_size, &ptr[ofs]);
		Error err = MultiplayerAPI::encode_and_compress_variant(spawn_arg, &ptr[ofs], spawn_arg_size, false);
		ERR_FAIL_COND_V(err, err);
		ofs += spawn_arg_size;
	}
	// Write state.
	if (state_size) {
		Error err = MultiplayerAPI::encode_and_compress_variants(state_varp.ptrw(), state_varp.size(), &ptr[ofs], state_size);
		ERR_FAIL_COND_V(err, err);
		ofs += state_size;
	}
	r_len = ofs;
	return OK;
}

Error SceneReplicationInterface::_make_despawn_packet(Node *p_node, int &r_len) {
	const ObjectID oid = p_node->get_instance_id();
	const TrackedNode *tnode = tracked_nodes.getptr(oid);
	ERR_FAIL_COND_V(!tnode, ERR_INVALID_PARAMETER);
	MAKE_ROOM(5);
	uint8_t *ptr = packet_cache.ptrw();
	ptr[0] = (uint8_t)SceneMultiplayer::NETWORK_COMMAND_DESPAWN;
	int ofs = 1;
	uint32_t nid = tnode->net_id;
	ofs += encode_uint32(nid, &ptr[ofs]);
	r_len = ofs;
	return OK;
}

Error SceneReplicationInterface::on_spawn_receive(int p_from, const uint8_t *p_buffer, int p_buffer_len) {
	ERR_FAIL_COND_V_MSG(p_buffer_len < 18, ERR_INVALID_DATA, "Invalid spawn packet received");
	int ofs = 1; // The spawn/despawn command.
	uint8_t scene_id = p_buffer[ofs];
	ofs += 1;
	uint32_t node_target = decode_uint32(&p_buffer[ofs]);
	ofs += 4;
	MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(multiplayer->get_path_cache()->get_cached_object(p_from, node_target));
	ERR_FAIL_COND_V(!spawner, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(p_from != spawner->get_multiplayer_authority(), ERR_UNAUTHORIZED);

	uint32_t net_id = decode_uint32(&p_buffer[ofs]);
	ofs += 4;
	uint32_t sync_len = decode_uint32(&p_buffer[ofs]);
	ofs += 4;
	uint32_t name_len = decode_uint32(&p_buffer[ofs]);
	ofs += 4;
	ERR_FAIL_COND_V_MSG(name_len + (sync_len * 4) > uint32_t(p_buffer_len - ofs), ERR_INVALID_DATA, vformat("Invalid spawn packet size: %d, wants: %d", p_buffer_len, ofs + name_len + (sync_len * 4)));
	List<uint32_t> sync_ids;
	for (uint32_t i = 0; i < sync_len; i++) {
		sync_ids.push_back(decode_uint32(&p_buffer[ofs]));
		ofs += 4;
	}
	ERR_FAIL_COND_V_MSG(name_len < 1, ERR_INVALID_DATA, "Zero spawn name size.");

	// We need to make sure no trickery happens here, but we want to allow autogenerated ("@") node names.
	const String name = String::utf8((const char *)&p_buffer[ofs], name_len);
	ERR_FAIL_COND_V_MSG(name.validate_node_name() != name, ERR_INVALID_DATA, vformat("Invalid node name received: '%s'. Make sure to add nodes via 'add_child(node, true)' remotely.", name));
	ofs += name_len;

	// Check that we can spawn.
	Node *parent = spawner->get_node_or_null(spawner->get_spawn_path());
	ERR_FAIL_COND_V(!parent, ERR_UNCONFIGURED);
	ERR_FAIL_COND_V(parent->has_node(name), ERR_INVALID_DATA);

	Node *node = nullptr;
	if (scene_id == MultiplayerSpawner::INVALID_ID) {
		// Custom spawn.
		ERR_FAIL_COND_V(p_buffer_len - ofs < 4, ERR_INVALID_DATA);
		uint32_t arg_size = decode_uint32(&p_buffer[ofs]);
		ofs += 4;
		ERR_FAIL_COND_V(arg_size > uint32_t(p_buffer_len - ofs), ERR_INVALID_DATA);
		Variant v;
		Error err = MultiplayerAPI::decode_and_decompress_variant(v, &p_buffer[ofs], arg_size, nullptr, false);
		ERR_FAIL_COND_V(err != OK, err);
		ofs += arg_size;
		node = spawner->instantiate_custom(v);
	} else {
		// Scene based spawn.
		node = spawner->instantiate_scene(scene_id);
	}
	ERR_FAIL_COND_V(!node, ERR_UNAUTHORIZED);
	node->set_name(name);

	// Add and track remote
	ERR_FAIL_COND_V(!peers_info.has(p_from), ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(peers_info[p_from].recv_nodes.has(net_id), ERR_ALREADY_IN_USE);
	ObjectID oid = node->get_instance_id();
	TrackedNode &tobj = _track(oid);
	tobj.spawner = spawner->get_instance_id();
	tobj.net_id = net_id;
	tobj.remote_peer = p_from;
	peers_info[p_from].recv_nodes[net_id] = oid;

	// The initial state will be applied during the sync config (i.e. before _ready).
	pending_spawn = node->get_instance_id();
	pending_spawn_remote = p_from;
	pending_buffer_size = p_buffer_len - ofs;
	pending_buffer = pending_buffer_size > 0 ? &p_buffer[ofs] : nullptr;
	pending_sync_net_ids = sync_ids;

	parent->add_child(node);
	spawner->emit_signal(SNAME("spawned"), node);

	pending_spawn = ObjectID();
	pending_spawn_remote = 0;
	pending_buffer = nullptr;
	pending_buffer_size = 0;
	if (pending_sync_net_ids.size()) {
		pending_sync_net_ids.clear();
		ERR_FAIL_V(ERR_INVALID_DATA); // Should have been consumed.
	}
	return OK;
}

Error SceneReplicationInterface::on_despawn_receive(int p_from, const uint8_t *p_buffer, int p_buffer_len) {
	ERR_FAIL_COND_V_MSG(p_buffer_len < 5, ERR_INVALID_DATA, "Invalid spawn packet received");
	int ofs = 1; // The spawn/despawn command.
	uint32_t net_id = decode_uint32(&p_buffer[ofs]);
	ofs += 4;

	// Untrack remote
	ERR_FAIL_COND_V(!peers_info.has(p_from), ERR_UNAUTHORIZED);
	PeerInfo &pinfo = peers_info[p_from];
	ERR_FAIL_COND_V(!pinfo.recv_nodes.has(net_id), ERR_UNAUTHORIZED);
	Node *node = get_id_as<Node>(pinfo.recv_nodes[net_id]);
	ERR_FAIL_COND_V(!node, ERR_BUG);
	pinfo.recv_nodes.erase(net_id);

	const ObjectID oid = node->get_instance_id();
	ERR_FAIL_COND_V(!tracked_nodes.has(oid), ERR_BUG);
	MultiplayerSpawner *spawner = get_id_as<MultiplayerSpawner>(tracked_nodes[oid].spawner);
	ERR_FAIL_COND_V(!spawner, ERR_DOES_NOT_EXIST);
	ERR_FAIL_COND_V(p_from != spawner->get_multiplayer_authority(), ERR_UNAUTHORIZED);

	if (node->get_parent() != nullptr) {
		node->get_parent()->remove_child(node);
	}
	node->queue_free();
	spawner->emit_signal(SNAME("despawned"), node);

	return OK;
}

void SceneReplicationInterface::_send_sync(int p_peer, const HashSet<ObjectID> p_synchronizers, uint16_t p_sync_net_time, uint64_t p_msec) {
	MAKE_ROOM(sync_mtu);
	uint8_t *ptr = packet_cache.ptrw();
	ptr[0] = SceneMultiplayer::NETWORK_COMMAND_SYNC;
	int ofs = 1;
	ofs += encode_uint16(p_sync_net_time, &ptr[1]);
	// Can only send updates for already notified nodes.
	// This is a lazy implementation, we could optimize much more here with by grouping by replication config.
	for (const ObjectID &oid : p_synchronizers) {
		MultiplayerSynchronizer *sync = get_id_as<MultiplayerSynchronizer>(oid);
		ERR_CONTINUE(!sync || !sync->get_replication_config().is_valid() || !sync->is_multiplayer_authority());
		if (!sync->update_outbound_sync_time(p_msec)) {
			continue; // nothing to sync.
		}

		Node *node = sync->get_root_node();
		ERR_CONTINUE(!node);
		uint32_t net_id = sync->get_net_id();
		if (net_id == 0 || (net_id & 0x80000000)) {
			int path_id = 0;
			bool verified = multiplayer->get_path_cache()->send_object_cache(sync, p_peer, path_id);
			ERR_CONTINUE_MSG(path_id < 0, "This should never happen!");
			if (net_id == 0) {
				// First time path based ID.
				net_id = path_id | 0x80000000;
				sync->set_net_id(net_id | 0x80000000);
			}
			if (!verified) {
				// The path based sync is not yet confirmed, skipping.
				continue;
			}
		}
		int size;
		Vector<Variant> vars;
		Vector<const Variant *> varp;
		const List<NodePath> props = sync->get_replication_config()->get_sync_properties();
		Error err = MultiplayerSynchronizer::get_state(props, node, vars, varp);
		ERR_CONTINUE_MSG(err != OK, "Unable to retrieve sync state.");
		err = MultiplayerAPI::encode_and_compress_variants(varp.ptrw(), varp.size(), nullptr, size);
		ERR_CONTINUE_MSG(err != OK, "Unable to encode sync state.");
		// TODO Handle single state above MTU.
		ERR_CONTINUE_MSG(size > 3 + 4 + 4 + sync_mtu, vformat("Node states bigger then MTU will not be sent (%d > %d): %s", size, sync_mtu, node->get_path()));
		if (ofs + 4 + 4 + size > sync_mtu) {
			// Send what we got, and reset write.
			_send_raw(packet_cache.ptr(), ofs, p_peer, false);
			ofs = 3;
		}
		if (size) {
			ofs += encode_uint32(sync->get_net_id(), &ptr[ofs]);
			ofs += encode_uint32(size, &ptr[ofs]);
			MultiplayerAPI::encode_and_compress_variants(varp.ptrw(), varp.size(), &ptr[ofs], size);
			ofs += size;
		}
#ifdef DEBUG_ENABLED
		_profile_node_data("sync_out", oid, size);
#endif
	}
	if (ofs > 3) {
		// Got some left over to send.
		_send_raw(packet_cache.ptr(), ofs, p_peer, false);
	}
}

Error SceneReplicationInterface::on_sync_receive(int p_from, const uint8_t *p_buffer, int p_buffer_len) {
	ERR_FAIL_COND_V_MSG(p_buffer_len < 11, ERR_INVALID_DATA, "Invalid sync packet received");
	uint16_t time = decode_uint16(&p_buffer[1]);
	int ofs = 3;
	while (ofs + 8 < p_buffer_len) {
		uint32_t net_id = decode_uint32(&p_buffer[ofs]);
		ofs += 4;
		uint32_t size = decode_uint32(&p_buffer[ofs]);
		ofs += 4;
		MultiplayerSynchronizer *sync = nullptr;
		if (net_id & 0x80000000) {
			sync = Object::cast_to<MultiplayerSynchronizer>(multiplayer->get_path_cache()->get_cached_object(p_from, net_id & 0x7FFFFFFF));
		} else if (peers_info[p_from].recv_sync_ids.has(net_id)) {
			const ObjectID &sid = peers_info[p_from].recv_sync_ids[net_id];
			sync = get_id_as<MultiplayerSynchronizer>(sid);
		}
		if (!sync) {
			// Not received yet.
			ofs += size;
			continue;
		}
		Node *node = sync->get_root_node();
		if (sync->get_multiplayer_authority() != p_from || !node) {
			ERR_CONTINUE(true);
		}
		if (!sync->update_inbound_sync_time(time)) {
			// State is too old.
			ofs += size;
			continue;
		}
		ERR_FAIL_COND_V(size > uint32_t(p_buffer_len - ofs), ERR_BUG);
		const List<NodePath> props = sync->get_replication_config()->get_sync_properties();
		Vector<Variant> vars;
		vars.resize(props.size());
		int consumed;
		Error err = MultiplayerAPI::decode_and_decompress_variants(vars, &p_buffer[ofs], size, consumed);
		ERR_FAIL_COND_V(err, err);
		err = MultiplayerSynchronizer::set_state(props, node, vars);
		ERR_FAIL_COND_V(err, err);
		ofs += size;
#ifdef DEBUG_ENABLED
		_profile_node_data("sync_in", sync->get_instance_id(), size);
#endif
	}
	return OK;
}
