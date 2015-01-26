/*
 * Copyright (c) 2014 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Policies.h"
#include "Connection.h"
#include "ConnectionPool.h"
#include "LogSeverity.h"
#include <shared/threading/Spinlock.h>
#include <vector>
#include <cassert>
#include <thread>
#include <list>
#include <exception>
#include <mutex>
#include <condition_variable>

namespace ember { namespace connection_pool {

namespace sc = std::chrono;

template<typename ConType, typename Driver, typename ReusePolicy, typename GrowthPolicy>
class PoolManager {
	typedef Pool<Driver, ReusePolicy, GrowthPolicy>* ConnectionPool;
	ConnectionPool pool_;
	sc::seconds interval_, max_idle_;
	std::thread manager_;
	bool stop_ = false;
	Spinlock exception_lock_;
	std::exception_ptr exception_;
	std::condition_variable cond_;
	std::mutex cond_lock_;

	void close_excess_idle() {
		for(auto i = pool_->pool_.begin(); i != pool_->pool_.end();) {
			if(i->sweep) {
				try {
					pool_->driver_.close(i->conn);
				} catch(std::exception& e) { 
					if(pool_->log_cb_) {
						pool_->log_cb_(SEVERITY::WARN,
					                   std::string("Connection close, driver threw: ") + e.what());
					}
				} catch(...) {
					if(pool_->log_cb_) {
						pool_->log_cb_(SEVERITY::WARN, "Driver threw unknown exception in close");
					}
				}

				std::lock_guard<Spinlock> guard(pool_->lock_);
				i = pool_->pool_.erase(i);
			} else {
				++i;
			}
		}
	}

	void refresh_idle(std::vector<ConnDetail<ConType>*>& connections) {
		for(auto& c : connections) {
			try {
				c->conn = pool_->driver_.keep_alive(c->conn);
				c->idle = sc::seconds(0);
				c->error = false;
			} catch(std::exception& e) { 
				if(pool_->log_cb_) {
					pool_->log_cb_(SEVERITY::WARN,
					               std::string("Connection keep-alive, driver threw: ") + e.what());
				}
				c->error = true;
			} catch(...) {
				if(pool_->log_cb_) {
					pool_->log_cb_(SEVERITY::WARN, "Driver threw unknown exception in keep_alive");
				}
				c->error = true;
			}

			c->checked_out = false;
			pool_->semaphore_.signal();
		}
	}

	void close_errored() {
		for(auto i = pool_->pool_.begin(); i != pool_->pool_.end();) {
			if(i->error) {
				try {
					pool_->driver_.close(i->conn);
				} catch(std::exception& e) { 
					if(pool_->log_cb_) {
						pool_->log_cb_(SEVERITY::WARN,
						               std::string("Connection close, driver threw: ") + e.what());
					}
				} catch(...) {
					if(pool_->log_cb_) {
						pool_->log_cb_(SEVERITY::WARN, "Driver threw unknown exception in close");
					}
				}

				std::lock_guard<Spinlock> guard(pool_->lock_);
				i = pool_->pool_.erase(i);
			} else {
				++i; 
			}
		}

		//If the pool has grown too small, try to refill it
		std::lock_guard<Spinlock> guard(pool_->lock_);
		if(pool_->pool_.size() < pool_->min_) {
			pool_->open_connections(pool_->min_ - pool_->pool_.size());
		}
	}

	void manage_connections() {
		if(pool_->log_cb_) {
			pool_->log_cb_(SEVERITY::INFO, "Hey, world!");
		}

		std::vector<ConnDetail<ConType>*> checked_out;
		std::unique_lock<Spinlock> guard(pool_->lock_);
		int removals = pool_->pool_.size() - pool_->min_;

		for(auto& conn : pool_->pool_) {
			if(conn.checked_out) {
				continue;
			}

			if(conn.idle < max_idle_) {
				conn.idle += interval_;
			} else if(removals > 0) {
				--removals;
				conn.checked_out = true;
				conn.sweep = true;
			} else {
				conn.checked_out = true;
				checked_out.push_back(&conn);
			}
		}

		pool_->lock_.unlock();

		close_excess_idle();
		refresh_idle(checked_out);
		close_errored();
	}

public:
	PoolManager(ConnectionPool pool) {
		pool_ = pool;
	}

	void check_exceptions() {
		std::lock_guard<Spinlock> lock(exception_lock_);

		if(exception_) {
			std::rethrow_exception(exception_);
		}
	}

	void run() try {
		pool_->driver_.thread_enter();
		std::unique_lock<std::mutex> lock(cond_lock_);

		while(!stop_) {
			if(cond_.wait_for(lock, interval_) == std::cv_status::no_timeout) {
				break;
			}

			manage_connections();
		}

		pool_->driver_.thread_exit();
	} catch(...) {
		std::lock_guard<Spinlock> lock(exception_lock_);
		exception_ = std::current_exception();
	}

	void stop() {
		if(manager_.joinable()) {
			stop_ = true;
			cond_.notify_one();
			manager_.join();
		}
	}

	void start(sc::seconds interval, sc::seconds max_idle) {
		interval_ = interval;
		max_idle_ = max_idle;
		manager_ = std::thread(&PoolManager::run, this);
	}
};

}} //connection_pool, ember