/*
 * Gate_Manager.cpp
 *
 *  Created on: Nov 8, 2016
 *      Author: zhangyalei
 */

#include "Base_Function.h"
#include "Node_Manager.h"
#include "Gate_Manager.h"

Gate_Manager::Gate_Manager(void):
	cid_session_map_(10000),
	sid_session_map_(10000) { }

Gate_Manager::~Gate_Manager(void) { }

Gate_Manager *Gate_Manager::instance_;

Gate_Manager *Gate_Manager::instance(void) {
	if (instance_ == 0)
		instance_ = new Gate_Manager;
	return instance_;
}

void Gate_Manager::run_handler(void) {
	process_list();
}

int Gate_Manager::process_list(void) {
	Msg_Head msg_head;
	Byte_Buffer *buffer = nullptr;
	while (true) {
		//此处加锁是为了防止本线程其他地方同时等待条件变量成立
		notify_lock_.lock();

		//put wait in while cause there can be spurious wake up (due to signal/ENITR)
		while (buffer_list_.empty()) {
			notify_lock_.wait();
		}

		buffer = buffer_list_.pop_front();
		if(buffer != nullptr) {
			msg_head.reset();
			buffer->read_head(msg_head);
			transmit_msg(msg_head, buffer);
		}

		//操作完成解锁条件变量
		notify_lock_.unlock();
	}
	return 0;
}

int Gate_Manager::transmit_msg(Msg_Head &msg_head, Byte_Buffer *buffer) {
	int eid = msg_head.eid;
	int cid = msg_head.cid;

	if (msg_head.msg_type == C2S) {
		Session *session = find_session_by_cid(msg_head.cid);
		if (!session) {
			LOG_ERROR("find_session_by_cid error, eid:%d, cid:%d, msg_type:%d, msg_id:%d, sid:%d",
					msg_head.eid, msg_head.cid, msg_head.msg_type, msg_head.msg_id, msg_head.sid);
			return -1;
		}

		msg_head.eid = session->game_eid;
		msg_head.cid = session->game_cid;
		msg_head.msg_type = NODE_C2S;
		msg_head.sid = session->sid;
	} else if (msg_head.msg_type == NODE_S2C) {
		Session *session = find_session_by_sid(msg_head.sid);
		if (!session) {
			LOG_ERROR("find_session_by_sid error, eid:%d, cid:%d, msg_type:%d, msg_id:%d, sid:%d",
					msg_head.eid, msg_head.cid, msg_head.msg_type, msg_head.msg_id, msg_head.sid);
			return -1;
		}

		msg_head.eid = session->client_eid;
		msg_head.cid = session->client_cid;
		msg_head.msg_type = S2C;
		msg_head.sid = session->sid;
	}
	NODE_MANAGER->send_msg(msg_head, buffer->get_read_ptr(), buffer->readable_bytes());
	NODE_MANAGER->push_buffer(eid, cid, buffer);
	return 0;
}

int Gate_Manager::add_session(Session *session) {
	if (!session) {
		LOG_ERROR("node_id:%d, node_name:%s add session error", NODE_MANAGER->node_info().node_id, NODE_MANAGER->node_info().node_name.c_str());
		return -1;
	}

	cid_session_map_.insert(std::make_pair(session->client_cid, session));
	sid_session_map_.insert(std::make_pair(session->sid, session));
	return 0;
}

int Gate_Manager::remove_session(int cid) {
	Session_Map::iterator iter = cid_session_map_.find(cid);
	if (iter != sid_session_map_.end()) {
		Session *session = iter->second;
		cid_session_map_.erase(iter);
		sid_session_map_.erase(session->sid);
		session_pool_.push(session);
	}
	return 0;
}

Session *Gate_Manager::find_session_by_cid(int cid) {
	Session_Map::iterator iter = cid_session_map_.find(cid);
	if (iter != cid_session_map_.end()) {
		return iter->second;
	}
	return nullptr;
}

Session *Gate_Manager::find_session_by_sid(int sid) {
	Session_Map::iterator iter = sid_session_map_.find(sid);
	if (iter != sid_session_map_.end()) {
		return iter->second;
	}
	return nullptr;
}