/*
 * 2013+ Copyright (c) Ruslan Nigatullin <euroelessar@yandex.ru>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "connection_p.hpp"
#include <vector>
#include <boost/bind.hpp>
#include <iostream>

#if __GNUC__ == 4 && __GNUC_MINOR__ < 5
#  include <cstdatomic>
#else
#  include <atomic>
#endif

#include "server_p.hpp"
#include "stockreplies_p.hpp"

namespace ioremap {
namespace thevoid {

//#define debug(arg) do { std::cerr << __PRETTY_FUNCTION__ << " (" << __LINE__ << ") " << arg << std::endl; std::cerr.flush(); } while (0)

#define debug(arg) do {} while (0)

template <typename T>
connection<T>::connection(boost::asio::io_service &service, size_t buffer_size) :
	m_socket(service),
	m_sending(false),
	m_buffer(buffer_size),
	m_content_length(0),
	m_state(read_headers),
	m_keep_alive(false),
	m_at_read(false)
{
	m_unprocessed_begin = m_buffer.data();
	m_unprocessed_end = m_buffer.data();

	debug(&service);
}

template <typename T>
connection<T>::~connection()
{
	if (m_server)
		--m_server->m_data->connections_counter;

	if (m_handler)
		m_handler->on_close(boost::system::error_code());

	debug("");
}

template <typename T>
T &connection<T>::socket()
{
	return m_socket;
}

template <typename T>
void connection<T>::start(const std::shared_ptr<base_server> &server)
{
	m_server = server;
	++m_server->m_data->connections_counter;
	async_read();
}

struct send_headers_guard
{
	std::function<void (const boost::system::error_code &)> handler;
	std::shared_ptr<swarm::http_response> reply;

	template <typename T>
	void operator() (const boost::system::error_code &err, const T &)
	{
		handler(err);
	}
};

template <typename T>
void connection<T>::send_headers(swarm::http_response &&rep,
	const boost::asio::const_buffer &content,
	std::function<void (const boost::system::error_code &err)> &&handler)
{
	if (m_keep_alive) {
                rep.headers().set_keep_alive();
                debug("Added Keep-Alive");
        }
	buffer_info info(
		std::move(stock_replies::to_buffers(rep, content)),
		std::move(rep),
		std::move(handler)
	);
	send_impl(std::move(info));
}

template <typename T>
void connection<T>::send_data(const boost::asio::const_buffer &buffer,
	std::function<void (const boost::system::error_code &)> &&handler)
{
	buffer_info info(
		std::move(std::vector<boost::asio::const_buffer>(1, buffer)),
		boost::none,
		std::move(handler)
	);
	send_impl(std::move(info));
}

template <typename T>
void connection<T>::want_more()
{
	// Invoke close_impl some time later, so we won't need any mutexes to guard the logic
	m_socket.get_io_service().post(std::bind(&connection::want_more_impl, this->shared_from_this()));
}

template <typename T>
void connection<T>::close(const boost::system::error_code &err)
{
	// Invoke close_impl some time later, so we won't need any mutexes to guard the logic
	m_socket.get_io_service().post(std::bind(&connection::close_impl, this->shared_from_this(), err));
}

template <typename T>
void connection<T>::want_more_impl()
{
	if (m_unprocessed_begin != m_unprocessed_end) {
		process_data(m_unprocessed_begin, m_unprocessed_end);
	} else {
		async_read();
	}
}

template <typename T>
void connection<T>::send_impl(buffer_info &&info)
{
	std::lock_guard<std::mutex> lock(m_outgoing_mutex);
	if (m_sending) {
		m_outgoing.emplace_back(std::move(info));
	} else {
		m_sending = true;
		m_outgoing_info = std::move(info);
		m_socket.async_write_some(m_outgoing_info.buffer, std::bind(
			&connection::write_finished, this->shared_from_this(),
			std::placeholders::_1, std::placeholders::_2));
	}
}

template <typename T>
void connection<T>::write_finished(const boost::system::error_code &err, size_t bytes_written)
{
	if (err) {
		std::list<buffer_info> outgoing;
		{
			std::lock_guard<std::mutex> lock(m_outgoing_mutex);
			outgoing = std::move(m_outgoing);
		}

		if (m_outgoing_info.handler)
			m_outgoing_info.handler(err);

		for (auto it = outgoing.begin(); it != outgoing.end(); ++it) {
			if (it->handler)
				it->handler(err);
		}

		if (m_handler)
			m_handler->on_close(err);
		close_impl(err);
		return;
	}

	auto &buffers = m_outgoing_info.buffer;
	for (auto it = buffers.begin(); bytes_written > 0 && it != buffers.end(); ++it) {
		auto &buffer = *it;
		buffer = buffer + std::min(bytes_written, boost::asio::buffer_size(buffer));
	}

	if (boost::asio::buffer_size(m_outgoing_info.buffer) > 0) {
		m_socket.async_write_some(m_outgoing_info.buffer, std::bind(
			&connection::write_finished, this->shared_from_this(),
			std::placeholders::_1, std::placeholders::_2));
	} else {
		if (m_outgoing_info.handler) {
			m_outgoing_info.handler(err);
		}

		{
			using std::swap;
			buffer_info info;
			swap(info, m_outgoing_info);
		}

		{
			std::lock_guard<std::mutex> lock(m_outgoing_mutex);
			if (m_outgoing.empty()) {
				m_sending = false;
				return;
			}

			m_outgoing_info = std::move(m_outgoing.front());
			m_outgoing.erase(m_outgoing.begin());
		}

		m_socket.async_write_some(m_outgoing_info.buffer, std::bind(
			&connection::write_finished, this->shared_from_this(),
			std::placeholders::_1, std::placeholders::_2));
	}
}

template <typename T>
void connection<T>::close_impl(const boost::system::error_code &err)
{
	debug(m_state);
	if (m_handler)
		--m_server->m_data->active_connections_counter;
	m_handler.reset();

	if (err) {
		boost::system::error_code ignored_ec;
		// If there was any error - close the connection, it's broken
		m_socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
		return;
	}

	// Is request data is not fully received yet - receive it
	if (m_state != processing_request) {
		m_state |= request_processed;
		return;
	}

	if (!m_keep_alive) {
		boost::system::error_code ignored_ec;
		m_socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
		return;
	}

	process_next();
}

template <typename T>
void connection<T>::process_next()
{
	// Start to wait new HTTP requests by this socket due to HTTP 1.1
	m_state = read_headers;
	m_request_parser.reset();

	m_request = swarm::http_request();

	if (m_unprocessed_begin != m_unprocessed_end) {
		process_data(m_unprocessed_begin, m_unprocessed_end);
	} else {
		async_read();
	}
}

template <typename T>
void connection<T>::handle_read(const boost::system::error_code &err, std::size_t bytes_transferred)
{
	m_at_read = false;
	debug("error: " << err.message());
	if (err) {
		if (m_handler) {
			m_handler->on_close(err);
			--m_server->m_data->active_connections_counter;
		}
		m_handler.reset();
		return;
	}

	process_data(m_buffer.data(), m_buffer.data() + bytes_transferred);

	// If an error occurs then no new asynchronous operations are started. This
	// means that all shared_ptr references to the connection object will
	// disappear and the object will be destroyed automatically after this
	// handler returns. The connection class's destructor closes the socket.
}

template <typename T>
void connection<T>::process_data(const char *begin, const char *end)
{
	debug("data: \"" << std::string(begin, end - begin) << "\"");
	if (m_state & read_headers) {
		boost::tribool result;
		const char *new_begin = NULL;
		boost::tie(result, new_begin) = m_request_parser.parse(m_request, begin, end);

		debug("parsed: \"" << std::string(begin, new_begin) << '"');
		debug("parse result: " << (result ? "true" : (!result ? "false" : "unknown_state")));

		if (!result) {
//			std::cerr << "url: " << m_request.uri << std::endl;

			send_error(swarm::http_response::bad_request);
			return;
		} else if (result) {
//			std::cerr << "url: " << m_request.uri << std::endl;

			auto factory = m_server->get_factory(m_request.url());
			if (!factory) {
				send_error(swarm::http_response::not_found);
				return;
			}

			if (auto length = m_request.headers().content_length())
				m_content_length = *length;
			else
				m_content_length = 0;
			m_keep_alive = m_request.is_keep_alive();

			++m_server->m_data->active_connections_counter;
			m_handler = factory->create();
			m_handler->initialize(std::static_pointer_cast<reply_stream>(this->shared_from_this()));
			m_handler->on_headers(std::move(m_request));

			m_state &= ~read_headers;
			m_state |=  read_data;

			process_data(new_begin, end);
			// async_read is called by processed_data
			return;
		}

		// need more data for request processing
		async_read();
	} else if (m_state & read_data) {
		size_t data_from_body = std::min<size_t>(m_content_length, end - begin);
		size_t processed_size = data_from_body;

		if (data_from_body && m_handler)
			processed_size = m_handler->on_data(boost::asio::buffer(begin, data_from_body));

		m_content_length -= processed_size;

		debug(m_state);

		if (data_from_body != processed_size) {
			// Handler can't process all data, wait until want_more method is called
			m_unprocessed_begin = begin + processed_size;
			m_unprocessed_end = end;
			return;
		} else if (m_content_length > 0) {
			debug("");
			async_read();
		} else {
			m_state &= ~read_data;
			m_unprocessed_begin = begin + processed_size;
			m_unprocessed_end = end;

			if (m_handler)
				m_handler->on_close(boost::system::error_code());

			if (m_state & request_processed) {
				debug("");
				process_next();
			}
		}
	}
}

template <typename T>
void connection<T>::async_read()
{
	if (m_at_read)
		return;
	m_at_read = true;
	debug("");
	m_socket.async_read_some(boost::asio::buffer(m_buffer),
					 std::bind(&connection::handle_read, this->shared_from_this(),
						   std::placeholders::_1,
						   std::placeholders::_2));
}

template <typename T>
void connection<T>::send_error(swarm::http_response::status_type type)
{
	send_headers(stock_replies::stock_reply(type),
		boost::asio::const_buffer(),
		std::bind(&connection::close_impl, this->shared_from_this(), std::placeholders::_1));
}

template class connection<boost::asio::local::stream_protocol::socket>;
template class connection<boost::asio::ip::tcp::socket>;

} // namespace thevoid
} // namespace ioremap
