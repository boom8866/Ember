/*
 * Copyright (c) 2015 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Actions.h"
#include "PacketBuffer.h"
#include "NetworkSession.h"
#include "Session.h"
#include <logger/Logger.h>
#include <shared/IPBanCache.h>
#include <shared/memory/ASIOAllocator.h>
#include <shared/threading/ThreadPool.h>
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <mutex>
#include <utility>
#include <vector>
#include <thread>

namespace ember {

class NetworkListener {
	std::vector<std::thread> workers_;
	std::set<std::shared_ptr<NetworkSession>> sessions_;
	std::mutex sessions_lock_;
	boost::asio::io_service& service_;
	boost::asio::ip::tcp::acceptor acceptor_;
	boost::asio::ip::tcp::socket socket_;
	log::Logger* logger_; 
	IPBanCache& ban_list_;
	ASIOAllocator allocator_; // todo - thread_local, VS2015
	ThreadPool& pool_;

	void accept_connection() {
		acceptor_.async_accept(socket_, [this](boost::system::error_code ec) {
			if(!acceptor_.is_open()) {
				return;
			}

			if(!ec) {
				auto ip = socket_.remote_endpoint().address();

				if(ban_list_.is_banned(ip)) {
					LOG_DEBUG(logger_) << "Rejected connection from banned IP range" << LOG_ASYNC;
					return;
				}

				LOG_DEBUG(logger_) << "Accepted connection " << ip.to_string() << ":"
				                   << socket_.remote_endpoint().port() << LOG_ASYNC;

				start_session(std::move(socket_));
			}

			accept_connection();
		});
	}

	void start_session(boost::asio::ip::tcp::socket socket) {
		LOG_TRACE(logger_) << __func__ << LOG_ASYNC;

		auto& service = socket.get_io_service();
		auto address = socket.remote_endpoint().address().to_string();

		auto session = std::make_shared<Session<T>>(std::move(create_handler_(address)),
		                                            std::move(socket), service);

		session->handler.execute_action =
			std::bind(&NetworkHandler::execute_action, this, session, std::placeholders::_1);
		session->handler.send =
			std::bind(&NetworkHandler::write, this, session, std::placeholders::_1);

		session->timer.expires_from_now(SOCKET_ACTIVITY_TIMEOUT);

		std::lock_guard<std::mutex> guard(sessions_lock_);
		sessions_.insert(session);

		read(session);
	}

	void read(std::shared_ptr<Session<T>> session) {
		auto& buffer = session->buffer;
		//auto tail = session->buffer_chain_.tail();

		//// if the buffer chain has no more space left, allocate & attach new node
		//if(!tail->free()) {
		//	tail = session->buffer_chain_.allocate();
		//	session->buffer_chain_.attach(tail);
		//}

		session->socket.async_receive(boost::asio::buffer(buffer.store(), buffer.free()),
			session->strand.wrap(create_alloc_handler(allocator_,
			[this, session](boost::system::error_code ec, std::size_t size) {
				if(!ec) {
					reset_timer(session);
					session->buffer.advance(size);
					handle_packet(session);
				} else if(ec != boost::asio::error::operation_aborted) {
					close_session(session);
				}
			}
		)));
	}

	void write(std::shared_ptr<Session<T>> session, std::shared_ptr<Packet> packet) {
		session->tbuffer.write(packet->data(), packet->size());
		LOG_WARN(logger_) << "Packet size: " << packet->size() << LOG_SYNC;
		LOG_WARN(logger_) << "Chain size: " << session->tbuffer.size() << LOG_SYNC;
		session->socket.async_send(session->tbuffer,
			session->strand.wrap(create_alloc_handler(allocator_,
			[this, packet, session](boost::system::error_code ec, std::size_t size) {
		LOG_WARN(logger_) << "Send size: " << size << LOG_SYNC;

				if(!ec) {
					session->tbuffer.skip(size);

					if(session->tbuffer.size()) {
						// do additional write
					}
				} else if(ec && ec != boost::asio::error::operation_aborted) {
					close_session(session);
				}
			}
		)));
	}

	void action_complete(std::shared_ptr<Session<T>> session, std::shared_ptr<Action> action) try {
		if(!session->handler.update_state(action)) {
			close_session(session);
		}
	} catch(std::exception& e) {
		LOG_DEBUG(logger_) << e.what() << LOG_ASYNC;
		close_session(session);
	}

	void execute_action(std::shared_ptr<Session<T>> session, std::shared_ptr<Action> action) {
		auto shared_this = this->shared_from_this();

		pool_.run([session, action, this, shared_this] {
			action->execute();
			session->strand.post([session, action, this, shared_this] {
				action_complete(session, action);
			});
		});
	}

	void handle_packet(std::shared_ptr<Session<T>> session) try {
		LOG_TRACE(logger_) << __func__ << LOG_ASYNC;

		if(check_packet_completion_(session->buffer)) {
			if(!session->handler.update_state(session->buffer)) {
				close_session(session);
			}

			session->buffer.clear();
		}

		read(session);
	} catch(std::exception& e) {
		LOG_DEBUG(logger_) << e.what() << LOG_ASYNC;
		close_session(session);
	}

	void close_session(std::shared_ptr<Session<T>> session) {
		close_socket(session);
		std::lock_guard<std::mutex> guard(sessions_lock_);
		sessions_.erase(session);
	}

	void close_socket(std::shared_ptr<Session<T>> session) {
		boost::system::error_code ec;
		session->socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
		session->socket.close();
	}

	void reset_timer(std::shared_ptr<Session<T>> session) {
		session->timer.expires_from_now(SOCKET_ACTIVITY_TIMEOUT);
		session->timer.async_wait(session->strand.wrap(std::bind(&NetworkHandler::timeout, this,
		                          session, std::placeholders::_1)));
	}

	void timeout(std::shared_ptr<Session<T>> session, const boost::system::error_code& ec) {
		if(!ec) { // if the connection timed out, otherwise timer was aborted (session close)
			close_session(session);
		}
	}

public:
	NetworkHandler(std::string interface, unsigned short port, bool tcp_no_delay, unsigned int concurrency,
	               IPBanCache& bans, ThreadPool& pool, log::Logger* logger, 
	               CreateHandler create, CompletionChecker checker)
	               : acceptor_(service_, boost::asio::ip::tcp::endpoint(
	                           boost::asio::ip::address::from_string(interface), port)),
	                 socket_(service_), logger_(logger), ban_list_(bans), pool_(pool),
	                 create_handler_(create), check_packet_completion_(checker), concurrency_(concurrency) {
		acceptor_.set_option(boost::asio::ip::tcp::no_delay(tcp_no_delay));
		acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
		accept_connection();
	}

	void run() {
		accept_connection();
	}

	void shutdown() {
		acceptor_.close();

		std::lock_guard<std::mutex> guard(sessions_lock_);
				
		for(auto& session : sessions_) {
			close_socket(session);
		}

		sessions_.clear();
	}
};

} // ember