/*
 * Log_Manager.cpp
 *
 *  Created on: Nov 9, 2016
 *      Author: zhangyalei
 */

#include "Data_Manager.h"
#include "Node_Manager.h"
#include "Log_Manager.h"

Log_Manager::Log_Manager(void) : log_node_idx_(0), log_connector_size_(0) { }

Log_Manager::~Log_Manager(void) { }

Log_Manager *Log_Manager::instance_;

Log_Manager *Log_Manager::instance(void) {
	if (instance_ == 0)
		instance_ = new Log_Manager;
	return instance_;
}

int Log_Manager::init(const Node_Info &node_info) { 
	node_info_ = node_info; 
	log_node_idx_ = node_info_.node_id;

	Xml xml;
	bool ret = xml.load_xml("./config/node/node_conf.xml");
	if(ret < 0) {
		LOG_FATAL("load config:node_conf.xml abort");
		return -1;
	}
	//连接mysql
	TiXmlNode* node = xml.get_root_node("node_list");
	if(node) {
		TiXmlNode* child_node = xml.enter_node(node, "node_list");
		if(child_node) {
			XML_LOOP_BEGIN(child_node)
				if(xml.get_attr_int(child_node, "type") == node_info_.node_type) {
					TiXmlNode* sub_node = xml.enter_node(child_node, "node");
					if(sub_node) {
						XML_LOOP_BEGIN(sub_node)
							std::string key = xml.get_key(sub_node);
							if(key == "mysql") {
								int db_id = xml.get_attr_int(sub_node, "db_id");
								std::string ip = xml.get_attr_str(sub_node, "ip");
								int port = xml.get_attr_int(sub_node, "port");
								std::string user = xml.get_attr_str(sub_node, "user");
								std::string password = xml.get_attr_str(sub_node, "password");
								std::string pool = xml.get_attr_str(sub_node, "pool");
								DATA_MANAGER->connect_to_db(db_id, ip, port, user, password, pool);
							}
						XML_LOOP_END(sub_node)
					}
					break;
				}
			XML_LOOP_END(child_node)
		}
	}
	return 0;
}

void Log_Manager::run_handler(void) {
	process_list();
}

int Log_Manager::process_list(void) {
	Msg_Head msg_head;
	Bit_Buffer bit_buffer;
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
			int buffer_size = buffer_list_.size();
			if(node_info_.endpoint_gid == 1 && buffer_size >= node_info_.max_session_count) {
				//计算需要创建log_connector的数量，如果大于0，就创建
				int fork_size = buffer_size / node_info_.max_session_count - log_connector_size_;
				if(fork_size > 0 && log_fork_list_.count(log_node_idx_) <= 0) {
					std::string node_name = "log_connector";
					NODE_MANAGER->fork_process(node_info_.node_type, ++log_node_idx_, 2, node_name);
					log_fork_list_.insert(log_node_idx_);
				}
			}

			msg_head.reset();
			buffer->read_head(msg_head);
			int eid = msg_head.eid;
			int cid = msg_head.cid;
			//如果当前已经有创建好的log_connector,就转发消息,否则就用本进程处理
			if(node_info_.endpoint_gid == 1 && log_connector_size_ > 0 
				&& buffer_size >= node_info_.max_session_count && msg_head.msg_id != SYNC_NODE_INFO) {
				int index = random() % (log_connector_size_);
				msg_head.cid = log_connector_list_[index];
				NODE_MANAGER->send_msg(msg_head, buffer->get_read_ptr(), buffer->readable_bytes());
			}
			else {
				bit_buffer.reset();
				bit_buffer.set_ary(buffer->get_read_ptr(), buffer->readable_bytes());
				switch(msg_head.msg_id) {
				case SYNC_NODE_INFO: {
					log_connector_list_.push_back(cid);
					log_connector_size_++;
					
					Node_Info node_info;
					node_info.deserialize(bit_buffer);
					log_fork_list_.erase(node_info.node_id);
					break;
				}
				case SYNC_SAVE_DB_DATA: {
					save_db_data(bit_buffer);
					break;
				}
				default:
					break;
				}
			}

			//回收buffer
			NODE_MANAGER->push_buffer(eid, cid, buffer);
		}

		//操作完成解锁条件变量
		notify_lock_.unlock();
	}
	return 0;
}

int Log_Manager::save_db_data(Bit_Buffer &buffer) {
	uint save_type = buffer.read_uint(2);
	bool vector_data = buffer.read_bool();
	uint db_id = buffer.read_uint(16);
	std::string struct_name = "";
	buffer.read_str(struct_name);
	/*uint data_type*/buffer.read_uint(8);
	return DATA_MANAGER->save_db_data(save_type, vector_data, db_id, struct_name, buffer);
}
