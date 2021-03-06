// Fastcgi Container - framework for development of high performance FastCGI applications in C++
// Copyright (C) 2011 Ilya Golubtsov <golubtsov@yandex-team.ru> (Fastcgi Daemon)
// Copyright (C) 2015 Alexander Ponomarenko <contact@propulsion-analysis.com>

// This file is part of Fastcgi Container.
//
// Fastcgi Container is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License (LGPL) as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Fastcgi Container is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License (LGPL) for more details.
//
// You should have received a copy of the GNU Lesser General Public License (LGPL)
// along with Fastcgi Container. If not, see <http://www.gnu.org/licenses/>.

#include <cerrno>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <functional>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <sys/time.h>

#include "endpoint.h"
#include "fcgi_request.h"
#include "fcgi_server.h"

#include "fastcgi3/util.h"
#include "fastcgi3/config.h"
#include "fastcgi3/except.h"
#include "fastcgi3/logger.h"
#include "fastcgi3/stream.h"
#include "fastcgi3/request.h"
#include "fastcgi3/handler.h"
#include "fastcgi3/component.h"
#include "fastcgi3/request_io_stream.h"

#include "details/componentset.h"
#include "details/globals.h"
#include "details/handler_context.h"
#include "details/handlerset.h"
#include "details/loader.h"
#include "details/request_cache.h"
#include "details/request_thread_pool.h"
#include "details/thread_pool.h"

#ifdef HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

namespace fastcgi
{

FCGIServer::FCGIServer(std::shared_ptr<Globals> globals) :
	globals_(globals), stopper_(new ServerStopper()), active_thread_holder_(new char(0)),
	monitorSocket_(-1), request_cache_(), time_statistics_(), sessionManager_()
{
	status_.store(Status::NOT_INITED);
}

FCGIServer::~FCGIServer() {
	if (nullptr!=sessionManager_) {
		sessionManager_->stop();
	}

	close(stopPipes_[0]);
	close(stopPipes_[1]);

	if (-1 != monitorSocket_) {
		close(monitorSocket_);
	}
}

const Globals*
FCGIServer::globals() const {
	return globals_.get();
}

std::shared_ptr<Logger>
FCGIServer::logger() const {
	return std::move(globals_->logger());
}

FCGIServer::Status
FCGIServer::status() const {
	return status_.load();
}

void
FCGIServer::start() {
	if (Status::NOT_INITED != status()) {
		throw std::runtime_error("Server is already started");
	}

	status_.store(Status::LOADING);

	logTimes_ = globals_->config()->asInt("/fastcgi/daemon/log-times", 0);

	initMonitorThread();

	initRequestCache();
	initTimeStatistics();
	initFastCGISubsystem();
	initSessionManager();

	createWorkThreads();

	status_.store(Status::RUNNING);

	if (-1 == pipe(stopPipes_)) {
		throw std::runtime_error("Cannot create stop signal pipes");
	}
	stopThread_.reset(new std::thread(&FCGIServer::stopThreadFunction, this));
	stopThread_->detach();
}

void
FCGIServer::stopThreadFunction() {
	while (true) {
		char c;
		if (1 == read(stopPipes_[0], &c, 1) && 's' == c) {
			break;
		}
	}
	stopInternal();
}

void
FCGIServer::stop() {
	write(stopPipes_[1], "s", 1);
}

void
FCGIServer::join() {
	if (Status::NOT_INITED == status()) {
		throw std::runtime_error("Server is not started yet");
	}
	globals_->joinThreadPools();

	while (!active_thread_holder_.unique()) {
		usleep(10000);
	}
}

void
FCGIServer::stopInternal() {
	Status stat = status();
	if (Status::NOT_INITED == stat) {
		throw std::runtime_error("Cannot stop server because it is not started yet");
	}
	else if (Status::LOADING == stat) {
		throw std::runtime_error("Cannot stop until loading finish");
	}
	else if (stopper_->stopped()) {
		throw std::runtime_error("Server is already stopping");
	}

	stopper_->stopped(true);

	FCGX_ShutdownPending();
	globals_->stopThreadPools();

}

void
FCGIServer::initMonitorThread() {
	monitorSocket_ = socket(AF_INET, SOCK_STREAM, 0);
	if (-1 == monitorSocket_) {
		throw std::runtime_error("Cannot create monitor socket");
	}

	int one = 1;
	if (-1 == setsockopt (monitorSocket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
		throw std::runtime_error("Cannot reuse monitor port: " + std::string(StringUtils::error(errno)));
	}

	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(globals_->config()->asInt("/fastcgi/daemon/monitor_port"));
	addr.sin_addr.s_addr = INADDR_ANY;
	memset(addr.sin_zero, '\0', sizeof(addr.sin_zero));

	if (-1 == bind(monitorSocket_, (const sockaddr*)&addr, sizeof(sockaddr_in))) {
		throw std::runtime_error("Cannot bind monitor port address");
	}

	if (1 == listen(monitorSocket_, SOMAXCONN)) {
		throw std::runtime_error("Cannot listen monitor port");
	}

	monitorThread_.reset(new std::thread(&FCGIServer::monitor, this));
	monitorThread_->detach();
}

void
FCGIServer::initRequestCache() {
	const std::string cacheComponentName = globals_->config()->asString(
		"/fastcgi/daemon[count(request-cache)=1]/request-cache/@component",
		StringUtils::EMPTY_STRING);

	std::shared_ptr<Component> cacheComponent = globals()->components()->find(cacheComponentName);
	if (!cacheComponent) {
		return;
	}
	request_cache_ = std::dynamic_pointer_cast<RequestCache>(cacheComponent);
	if (!request_cache_) {
		throw std::runtime_error("Component " + cacheComponentName + " does not implement RequestCache interface");
	}
}

void
FCGIServer::initSessionManager() {
	const std::string managerComponentName = globals_->config()->asString(
		"/fastcgi[count(session)=1]/session[@attach=\"1\" or @attach=\"true\"]/@component",
		StringUtils::EMPTY_STRING);

	std::shared_ptr<Component> managerComponent = globals()->components()->find(managerComponentName);
	if (!managerComponent) {
		return;
	}

	sessionManager_ = std::dynamic_pointer_cast<SessionManager>(managerComponent);
	if (!sessionManager_) {
		throw std::runtime_error("Component " + managerComponentName + " does not implement SessionManager interface");
	}
}

void
FCGIServer::initTimeStatistics() {
	const std::string componentName = globals_->config()->asString(
		"/fastcgi/daemon[count(statistics)=1]/statistics/@component",
		StringUtils::EMPTY_STRING);

	std::shared_ptr<Component> statisticsComponent = globals()->components()->find(componentName);
	if (!statisticsComponent) {
		return;
	}

	time_statistics_ = std::dynamic_pointer_cast<ResponseTimeStatistics>(statisticsComponent);
	if (!time_statistics_) {
		throw std::runtime_error("Component " + componentName + " does not implement ResponseTimeStatistics interface");
	}
}

void
FCGIServer::createWorkThreads() {
	for (auto &endpoint : endpoints_) {
		std::function<void()> f = std::bind(&FCGIServer::handle, this, endpoint);
		for (unsigned short t=0, threads=endpoint->threads(); t<threads; ++t) {
            globalPool_.push_back(std::make_unique<std::thread>(f));
		}
	}
}

void
FCGIServer::initFastCGISubsystem() {
	if (0 != FCGX_Init()) {
		throw std::runtime_error("Cannot init fastcgi library");
	}

	const Config *config = globals_->config();

	std::vector<std::string> v;
	config->subKeys("/fastcgi/daemon/endpoint", v);

	for (auto &c : v) {
		std::shared_ptr<Endpoint> endpoint = std::make_shared<Endpoint>(
			config->asString(c + "/socket", StringUtils::EMPTY_STRING),
			config->asString(c + "/port", StringUtils::EMPTY_STRING),
			config->asString(c + "/@keepalive", config->asString(c + "/@keepConnection", "true"))=="true"?1:0,
			config->asInt(c + "/threads", 1)
		);

		const int backlog = config->asInt(c + "/backlog", SOMAXCONN);
		endpoint->openSocket(backlog);
		endpoints_.push_back(endpoint);
	}

	if (endpoints_.empty()) {
		throw std::runtime_error("At least one endpoint has to be configured");
	}
}

void
FCGIServer::handle(std::shared_ptr<Endpoint> endpoint) {
	std::shared_ptr<ServerStopper> stopper = stopper_;
	std::shared_ptr<Logger> logger = globals_->logger();

	while (true) {
		try {
			if (stopper->stopped()) {
				return;
			}
			std::shared_ptr<ThreadHolder> holder = active_thread_holder_;

			Endpoint::ScopedBusyCounter busyCounter(*endpoint.get());
			RequestTask task;
			task.request = std::make_shared<Request>(logger, request_cache_, sessionManager_);
			task.request_stream = std::make_shared<FastcgiRequest>(task.request, endpoint, logger, time_statistics_, logTimes_);

			FastcgiRequest *request = dynamic_cast<FastcgiRequest*>(task.request_stream.get());

			busyCounter.decrement();
			holder.reset();
			int status = request->accept();	// Accept request
			if (stopper->stopped()) {
				return;
			}
			holder = active_thread_holder_;
			if (status < 0) {
				throw std::runtime_error("Failed to accept fastcgi request: " + std::to_string(status));
			}
			busyCounter.increment();

			try {
				request->attach();
			} catch (const std::exception &e) {
				logger->error("Failed to attach fastcgi request: %s", e.what());
				task.request->sendError(400);
				continue;
			}

			try {
				handleRequest(task);
			} catch (const std::exception &e) {
				task.request->sendError(500);
			}

		} catch (const std::exception &e) {
			logger->error("Failed to handle fastcgi request: %s", e.what());
		} catch (...) {
			logger->error("Caught unknown exception while handling fastcgi request");
		}
	}
}

void
FCGIServer::handleRequest(RequestTask task) {
	logger()->debug("Handling request %s", task.request->getScriptName().c_str());

	task.futureHandlers = [this](RequestTask task) {
		const HandlerSet::HandlerDescription* handler = getHandler(task);
		if (nullptr == handler || handler->handlers.empty()) {
			throw NotFound();
		}

		FastcgiRequest *request = dynamic_cast<FastcgiRequest*>(task.request_stream.get());
		request->setHandlerDesc(handler);

		return handler->handlers;
	};

	task.dispatch = [this](RequestTask task) {
		logger()->debug("Dispatching request %s", task.request->getScriptName().c_str());

		std::vector<std::shared_ptr<Filter>> filters;
		getFilters(task, filters);

		const HandlerSet::HandlerDescription* handler = getHandler(task);
		if ((nullptr == handler || handler->handlers.empty()) && !filters.empty()) {
			// Handler not found - let the filter(s) to be executed
			// and then try to find the handler again
			// Example: "athenticator"-filter may redirect the
			// request to undefined path (like "j_security_check") and
			// after the login redirect back to the original path
			handleRequestInternal(filters, task);
		} else {
			FastcgiRequest *request = dynamic_cast<FastcgiRequest*>(task.request_stream.get());
			request->setHandlerDesc(handler);

			handleRequestInternal(filters, handler, task);
		}

	};

	task.dispatch(task);
}

void
FCGIServer::monitor() {
    std::shared_ptr<ServerStopper> stopper = stopper_;
	while (true) {
		if (stopper->stopped()) {
			return;
		}
		int s = -1;
		try {
			s = accept(monitorSocket_, nullptr, nullptr);
			if (stopper->stopped()) {
				return;
			}
			if (-1 == s) {
				throw std::runtime_error("Cannot accept connection on monitor port");
			}

			char buf[80];
			int rlen = read(s, buf, sizeof(buf));
			if (rlen <= 0) {
				close(s);
				continue;
			}

			const char c = buf[0];
			if ('i' == c || 'I' == c) {
				std::string info = getServerInfo();
				write(s, info.c_str(), info.size());
			} else if ('s' == c || 'S' == c) { 
				stop();
			}

			close(s);
		} catch (const std::exception &e) {
			if (-1 != s) {
				close(s);
			}
			std::shared_ptr<Logger> logger = globals_->logger();
			if (logger) {
				if (Status::RUNNING == status()) {
					logger->error("%s, errno = %i", e.what(), errno);
					continue;
				}
				if (stopper->stopped()) {
					continue;	
				}
			}
			std::cerr << e.what() << ", errno = " << errno << std::endl;
		}
	}
}

void
FCGIServer::writePid(const Config& config) {
	const std::string& file = config.asString("/fastcgi/daemon/pidfile");
	try {
		std::ofstream f(file.c_str());
		f.exceptions(std::ios::badbit);
		f << static_cast<int>(getpid());
	} catch (std::ios::failure &e) {
		std::cerr << "Cannot open file " << file << std::endl;
		throw;
	}
}

std::string
FCGIServer::getServerInfo() const {
	std::stringstream info;

	const std::string t1 = "  ";
	const std::string t2 = "    ";
	const std::string t3 = "      ";

	Status stat = status();

	info << "<fastcgi-container>\n";

	info << "  <status>";
	switch (stat) {
		case Status::LOADING:
			info << "loading";
			break;
		case Status::RUNNING:
			info << "running";
			break;
		case Status::NOT_INITED:
			info << "not inited";
			break;
		default:
			info << (stopper_->stopped() ? "stopping" : "unknown");
			break;
	}
	info << "</status>\n";

	if (Status::RUNNING == stat) {
		info << t1 << "<pools>\n";

		info << t2 << "<endpoint_pools>\n";
		for (auto &endpoint : endpoints_) {
			info << t3 << "<endpoint"
				 << " socket=\"" << endpoint->toString() << "\""
				 << " threads=\"" << endpoint->threads() << "\""
				 << " busy=\"" << endpoint->getBusyCounter() << "\""
				 << "/>\n";
		}
		info << t2 << "</endpoint_pools>\n";

		const Globals::ThreadPoolMap& pools = globals_->pools();
		for (auto &map : pools) {
			const RequestsThreadPool *pool = map.second.get();
			ThreadPoolInfo tpinfo = pool->getInfo();
			uint64_t goodTasks = tpinfo.goodTasksCounter;
			uint64_t badTasks = tpinfo.badTasksCounter;
			info << t2 << "<pool name=\"" << map.first << "\""
				 << " threads=\"" << tpinfo.threadsNumber << "\""
				 << " busy=\"" << tpinfo.busyThreadsCounter << "\""
				 << " queue=\"" << tpinfo.queueLength << "\""
				 << " current_queue=\"" << tpinfo.currentQueue << "\""
				 << " all_tasks=\"" << (goodTasks + badTasks)  << "\""
				 << " exception_tasks=\"" << badTasks << "\""
				 << "/>\n";
		}

		info << t1 << "</pools>\n";
	}
	
	info << "</fastcgi-container>\n";

	return info.str();
}

} // namespace fastcgi
