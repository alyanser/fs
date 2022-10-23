#pragma once

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>

// wrapper TCP socket class over POSIX socket API
class Tcp_socket {
	int fd_;
public:
	Tcp_socket() : fd_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)){

		if(fd_ == -1){
			throw std::runtime_error("could not open socket. err: " + std::string(std::strerror(errno)));
		}
	}

	Tcp_socket(const int fd) noexcept : fd_(fd){}

	Tcp_socket(Tcp_socket && rhs) noexcept : fd_(rhs.fd_){
		rhs.fd_ = -1;
	}

	Tcp_socket & operator = (Tcp_socket && rhs) noexcept {
		fd_ = rhs.fd_;
		rhs.fd_ = -1;
		return *this;
	}

	Tcp_socket(const Tcp_socket &) = delete;

	~Tcp_socket() noexcept {

		if(fd_ != -1){
			close(fd_);
		}
	}

	int fd() const noexcept {
		return fd_;
	}

	void connect(const char * ip, const int port) const {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<std::uint16_t>(port));

		if(inet_pton(AF_INET, ip, &addr.sin_addr) != 1){
			throw std::invalid_argument("invalid ip address");
		}

		if(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0){
			throw std::runtime_error("could not connect(). err: " + std::string(std::strerror(errno)));
		}
	}

	void listen() const {
		constexpr auto MAX_QUEUED_CONNS = 5;

		if(::listen(fd_, MAX_QUEUED_CONNS) < 0){
			throw std::runtime_error("listen() failed. err: " + std::string(std::strerror(errno)));
		}
	}

	std::pair<Tcp_socket, sockaddr_in> accept() const {
		sockaddr addr;
		socklen_t len;

		const auto sock = ::accept(fd_, &addr, &len);

		if(sock < 0){
			throw std::runtime_error("accept() failed. err: " + std::string(std::strerror(errno)));
		}

		return std::make_pair(sock, reinterpret_cast<sockaddr_in&>(addr));
	}

	void bind(const int port) const {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<std::uint16_t>(port));
		addr.sin_addr.s_addr = INADDR_ANY;

		if(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0){
			throw std::runtime_error("bind() failed. err: " + std::string(std::strerror(errno)));
		}
	}

	void set_receive_timeout(const int recv_timeout_secs) const {
		timeval tv{};
		tv.tv_sec = recv_timeout_secs;

		if(setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0){
			throw std::runtime_error("could not set receive timeout. err: " + std::string(std::strerror(errno)));
		}
	}

	void send(const std::string & message) const {

		if(::send(fd_, message.c_str(), message.size(), 0) < 0){
			throw std::runtime_error("message could not be sent. err: " + std::string(std::strerror(errno)));
		}
	}

	void set_option(const int option, const int value = 1) const {

		if(setsockopt(fd_, SOL_SOCKET, option, &value, sizeof(value)) < 0){
			throw std::runtime_error("setsockopt() failed. err: " + std::string(std::strerror(errno)));
		}
	}

	std::string recv(const std::size_t buffer_size = 1 << 10) const {
		std::vector<char> buffer(buffer_size + 1);

		std::ptrdiff_t received_bytes;

		if((received_bytes = ::recv(fd_, buffer.data(), buffer_size, 0)) < 0){
			throw std::runtime_error("recv() failed. err: " + std::string(std::strerror(errno)));
		}

		buffer[static_cast<std::size_t>(received_bytes)] = '\0';
		return buffer.data();
	}
};
