#include <iostream>
#include <thread>
#include <algorithm>
#include <csignal>
#include <unordered_map>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <deque>
#include <stack>
#include <array>
#include <fstream>

#include "tcp_socket.h"

constexpr auto DEFAULT_CLIENT_PORT_NUM = 9001;
constexpr auto MAX_CONNECTIONS = 16;
constexpr char SOH = 0x1; // start of heading
constexpr char EOT = 0x4; // end of transmissoin
const std::string SELF_GROUP_ID = "P3_GROUP_2";

struct Group {
	std::string id;
	std::string addr;
	int port_num;

	operator std::string() const noexcept {
		return (id.empty() ? "NO_ID" : id) + ',' + addr + ',' + std::to_string(port_num);
	}
} self_group{SELF_GROUP_ID, {"130.208.243.61"}, {}};

struct Message {
	std::string content;
	std::string from_group_id;
};

std::mutex pending_msgs_mtx;
// holds messages that are not shared to the botnet yet
std::unordered_map<std::string, std::deque<Message>> pending_msgs; // for_group_id -> messages

std::mutex connected_groups_mtx;
// keeps track of connected groups. maps socket descriptor to groups' id, ip and port number
std::unordered_map<int, Group> connected_groups; // socket_descriptor -> group
std::condition_variable connected_groups_cv;

std::mutex pending_connections_mtx;
std::condition_variable pending_connections_cv;
// holds groups received from SERVERS command. used to form new connections with botnet servers
std::stack<Group> pending_connections;

namespace util {

// converts a string into tokens by separating it into smaller strings based on passed delimeters
std::vector<std::string> tokenize_string(const std::string & str, const std::vector<char> & delimeters) noexcept {
	std::vector<std::string> tokens;
	std::string token;

	for(const auto c : str){

		if(std::any_of(std::begin(delimeters), std::end(delimeters), [c](const char delim){
			return delim == c;
		})){
			if(!token.empty()){
				tokens.emplace_back(std::move(token));
				token.clear();
			}
		}else{
			token += c;
		}
	}

	if(!token.empty()){
		tokens.emplace_back(std::move(token));
	}

	return tokens;
}

enum class Log_type {
	INFO,
	WARNING,
	ERROR
};

std::mutex print_mtx;

// logs the given content to the stream
template<Log_type Log_type = Log_type::INFO, char Delim = '\n', typename Stream_type, typename ... Content_type>
typename std::add_lvalue_reference<Stream_type>::type log(Stream_type & stream, Content_type && ... content) noexcept {
	std::lock_guard<std::mutex> guard(print_mtx);

	switch(Log_type){

		case Log_type::INFO : {
			stream << "[INFO] ";
			break;
		}

		case Log_type::WARNING :{
			stream << "[WARNING] ";
			break;
		}

		case Log_type::ERROR : {
			stream << "[ERROR] ";
			break;
		}
	}

	const int arr[]{0, ((stream << std::forward<Content_type>(content)), 0) ...};;
	stream << Delim;
	static_cast<void>(arr);
	stream.flush();
	return stream;
}

// pretty prints the given message with timestamp and group information
void log_message_to_file(const std::string & to_group_id, const std::string & from_group_id, const std::string & msg){
	static std::mutex log_file_mtx;
	static std::ofstream log_file("message-logs.txt", std::ios_base::app);

	auto get_current_time = [](){
		const auto tm = std::time(nullptr);
		std::string ret(ctime(&tm));
		return ret.substr(0, ret.size() - 1); // remove newline from the end
	};

	std::lock_guard<std::mutex> guard(log_file_mtx);

	log_file << '[' << get_current_time() << "] [ " << from_group_id << " > " << to_group_id << " ] " << msg << '\n';
	log_file.flush();
}

} // namespace util

void handle_client(Tcp_socket & client_sock){

	while(true){
		const auto msg = client_sock.recv();
		const auto tokens = util::tokenize_string(msg, {','});

		if(tokens.empty()){
			util::log(std::cout, "invalid command received from a client");
			client_sock.send("invalid command");
			continue;
		}

		const auto & cmd = tokens.front();

		if(cmd == "QUERYSERVERS"){
			util::log(std::cout, "QUERYSERVERS command received from a client. responding accordingly");

			std::string response;
			std::lock_guard<std::mutex> guard(connected_groups_mtx);

			if(!connected_groups.empty()){

				for(const auto & pair : connected_groups){
					const auto & group = pair.second;
					response += std::string(group) + ';';
				}

				client_sock.send(response);
			}else{
				client_sock.send("no server is connected yet");
			}

		}else if(cmd == "FETCH"){
			util::log(std::cout, "FETCH command received from a client. responding accordingly");

			const auto & group_id = tokens.back();

			{
				std::lock_guard<std::mutex> guard(pending_msgs_mtx);

				// if there is a pending message for the given group, send it to client
				if(pending_msgs.count(group_id)){
					const auto & pending_group_msgs = pending_msgs[group_id];

					if(!pending_msgs.empty()){
						const auto & message = pending_group_msgs.front();
						client_sock.send(message.from_group_id + " > " + group_id + " : " + message.content);
						continue;
					}
				}
			}

			client_sock.send("no messages found for the given group id");
		}else if(cmd == "SEND"){
			util::log(std::cout, "SEND command received from a client. responding accordingly");

			const auto & to_group_id = tokens[1];
			std::string msg_contents;

			// combine the commas that were removed while tokenizing the string
			for(std::size_t i = 2; i < tokens.size(); ++i){
				msg_contents += tokens[i];

				if(i + 1 < tokens.size()){
					msg_contents += ',';
				}
			}

			{	// add the message received from client to the pending messages. botnet threads will use this to share them to botnet
				std::lock_guard<std::mutex> guard(pending_msgs_mtx);
				pending_msgs[to_group_id].push_back({std::move(msg_contents), SELF_GROUP_ID});
			}

			client_sock.send("message has been added to pending queue");
		}else if(cmd == "CONNECT"){
			util::log(std::cout, "CONNECT command received from a client. responding accordingly");

			Group group;
			group.addr = std::move(tokens[1]);
			group.port_num = std::stoi(tokens[2]);

			{
				std::lock_guard<std::mutex> guard(pending_connections_mtx);
				pending_connections.push(std::move(group));
			}

			pending_connections_cv.notify_one();

			client_sock.send("connection successfully added to pending connections stack");
		}else{
			// this shouldn't happen because client already parses the commands but just to be safe
			util::log(std::cout, "invalid command receieved from client");
			client_sock.send("invalid command");
		}
	}
}

namespace craft { // crafting messages based on the specification

std::string fetch_msg_packet(const std::string & group_id) noexcept {
	return "FETCH_MSGS," + group_id;
}

std::string send_msg_packet(const std::string & to_group_id, const std::string & from_group_id, const std::string & msg) noexcept {
	return "SEND_MSG," + to_group_id + ',' + from_group_id + ',' + msg;
}

std::string keepalive_packet(const int msg_cnt) noexcept {
	return "KEEPALIVE," + std::to_string(msg_cnt);
}

std::string serverlist_packet() noexcept {
	std::string packet("SERVERS," + std::string(self_group) + ';');

	std::lock_guard<std::mutex> guard(connected_groups_mtx);

	for(const auto & pair : connected_groups){
		const auto & group = pair.second;
		packet += std::string(group) + ';';
	}

	return packet;
}

std::string join_packet() noexcept {
	return "JOIN," + SELF_GROUP_ID;
}

std::string statusresp_packet(const std::string & to_group_id) noexcept {
	std::string packet = "STATUSRESP," + SELF_GROUP_ID + ',' + to_group_id + ',';

	std::lock_guard<std::mutex> guard(pending_msgs_mtx);

	for(const auto & pair : pending_msgs){
		const auto & for_group_id = pair.first;
		const auto & messages = pair.second;

		packet += for_group_id + ',' + std::to_string(messages.size()) + ',';
	}

	return packet;
}

} // namespace craft

void handle_botnet_server(Tcp_socket & botnet_sock){

	auto send_botnet_message = [&botnet_sock](const std::string & msg){
		botnet_sock.send(SOH + msg + EOT);
	};

	std::vector<std::string> tokens;

	auto on_invalid_command_received = [&botnet_sock, &tokens](){
		std::string cmd;

		for(const auto & str : tokens){
			cmd += str + ',';
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent an invalid/malformed command >> ", cmd);
	};

	auto on_join_received = [&](){

		if(tokens.size() != 2){
			on_invalid_command_received();
			return;
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent JOIN command");

		{	// update the group id of the botnet server
			util::log(std::cout, "botnet server # ", botnet_sock.fd(), " identified their group id: ", tokens.back());

			std::lock_guard<std::mutex> guard(connected_groups_mtx);
			connected_groups[botnet_sock.fd()].id = std::move(tokens.back());
		}

		util::log(std::cout, "sending serverlist response to botnet server # ", botnet_sock.fd());
		send_botnet_message(craft::serverlist_packet());
	};

	auto on_servers_received = [&](){
		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent SERVERS command");

		std::vector<Group> new_groups;

		for(std::size_t i = 4; i + 2 < tokens.size(); i += 3){
			auto & group_id = tokens[i];
			auto & ip = tokens[i + 1];
			auto & port = tokens[i + 2];

			int port_num;

			try{
				port_num = std::stoi(port);
			}catch(const std::exception & exception){
				on_invalid_command_received();
				continue;
			}

			new_groups.emplace_back(Group{std::move(group_id), std::move(ip), port_num});
		}

		std::lock_guard<std::mutex> connected_groups_guard(connected_groups_mtx);

		for(auto & new_group : new_groups){

			if(new_group.id.empty()){
				continue;
			}

			for(const auto & pair : connected_groups){
				const auto & connected_group = pair.second;

				if(new_group.id == connected_group.id || new_group.id == SELF_GROUP_ID){
					continue;
				}

				{	// add group information to pending connections. other thread will use it to form new connections with botnet
					std::lock_guard<std::mutex> pending_connections_guard(pending_connections_mtx);
					pending_connections.push(new_group);
				}

				// if connection limit isn't reached then inform other thread to form the connection
				if(connected_groups.size() < MAX_CONNECTIONS){
					pending_connections_cv.notify_one();
				}
			}
		}
	};

	auto on_keepalive_received = [&](){

		if(tokens.size() != 2){
			on_invalid_command_received();
			return;
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent KEEPALIVE command");

		const auto & no_of_msgs_str = tokens.back();
		int no_of_msgs;

		try{
			no_of_msgs = std::stoi(no_of_msgs_str);
		}catch(const std::exception & exception){
			on_invalid_command_received();
			return;
		}

		if(no_of_msgs > 0){
			util::log(std::cout, "sending FETCH_MSG command to botnet server # ", botnet_sock.fd());
			send_botnet_message(craft::fetch_msg_packet(SELF_GROUP_ID));
		}
	};

	auto on_fetch_msgs_received = [&](){

		if(tokens.size() != 2){
			on_invalid_command_received();
			return;
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent FETCH_MSGS command");

		const auto & for_group_id = tokens.back();

		std::lock_guard<std::mutex> pending_msgs_guard(pending_msgs_mtx);

		if(pending_msgs.count(for_group_id)){ // if we have any pending messages for the given group id, then send them to requesting server
			util::log(std::cout, "sending pending messages for group-id: ", for_group_id, " to botnet server # ", botnet_sock.fd());
			std::lock_guard<std::mutex> connected_groups_guard(connected_groups_mtx);

			auto & pending_group_msgs = pending_msgs[for_group_id];

			for(const auto & pending_msg : pending_group_msgs){
				send_botnet_message(craft::send_msg_packet(for_group_id, pending_msg.from_group_id, pending_msg.content));
				util::log_message_to_file(for_group_id, pending_msg.from_group_id, pending_msg.content);
			}

			pending_msgs.erase(for_group_id);
		}
	};

	auto on_send_msg_received = [&](){

		if(tokens.size() < 4){
			on_invalid_command_received();
			return;
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent SEND_MSG command");

		const auto & to_group_id = tokens[1];
		const auto & from_group_id = tokens[2];

		std::string send_msg;

		for(std::size_t i = 3; i < tokens.size(); ++i){
			send_msg += tokens[i];
		}

		util::log_message_to_file(to_group_id, from_group_id, send_msg);

		if(to_group_id != SELF_GROUP_ID){ // if this message was for us, then stop otherwise add it back to be shared to the botnet
			std::lock_guard<std::mutex> guard(pending_msgs_mtx);
			pending_msgs[to_group_id].emplace_back(Message{send_msg, from_group_id});
		}
	};

	auto on_statusreq_received = [&](){

		if(tokens.size() != 2){
			on_invalid_command_received();
			return;
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent STATUSREQ command");

		const auto & from_group_id = tokens.back();
		util::log(std::cout, "sending STATUSRESP command to botnet server # ", botnet_sock.fd());
		send_botnet_message(craft::statusresp_packet(from_group_id));
	};

	auto on_statusresp_received = [&](){

		if(tokens.size() < 3){
			on_invalid_command_received();
			return;
		}

		util::log(std::cout, "botnet server # ", botnet_sock.fd(), " sent STATUSRESP command");

		const auto & from_group_id = tokens[1];
		const auto & to_group_id = tokens[2];

		static_cast<void>(from_group_id);
		static_cast<void>(to_group_id);

		for(std::size_t i = 3; i + 1 < tokens.size(); i += 2){
			const auto & for_group_id = tokens[i];
			const auto & msg_cnt_str = tokens[i + 1];

			int msg_cnt;

			try{
				msg_cnt = std::stoi(msg_cnt_str);
			}catch(const std::exception & exception){
				on_invalid_command_received();
				continue;
			}

			if(msg_cnt > 0 && for_group_id == SELF_GROUP_ID){ // if there is a message for us, then fetch it
				util::log(std::cout, "sending FETCH_MSG command to botnet server # ", botnet_sock.fd());
				send_botnet_message(craft::fetch_msg_packet(for_group_id));
			}
		}
	};

	const std::unordered_map<std::string, std::function<void()>> cmd_mapping{
		{"JOIN", on_join_received},
		{"SERVERS", on_servers_received},
		{"KEEPALIVE", on_keepalive_received},
		{"FETCH_MSGS", on_fetch_msgs_received},
		{"SEND_MSG", on_send_msg_received},
		{"STATUSREQ", on_statusreq_received},
		{"STATUSRESP", on_statusresp_received},
	};

	// send the initial JOIN command
	send_botnet_message(craft::join_packet());

	std::thread sending_msgs_thread([&botnet_sock, send_botnet_message](){

		while(true){

			{
				std::lock_guard<std::mutex> connected_groups_guard(connected_groups_mtx);
				auto & connected_group_id = connected_groups[botnet_sock.fd()].id;

				std::lock_guard<std::mutex> pending_msgs_guard(pending_connections_mtx);

				if(pending_msgs.count(connected_group_id)){
					auto & pending_group_msgs = pending_msgs[connected_group_id];

					try{
						send_botnet_message(craft::keepalive_packet(static_cast<int>(pending_group_msgs.size())));
					}catch(const std::exception & exception){
						return;
					}
				}
			}

			std::this_thread::sleep_for(std::chrono::seconds(5));
		}
	});

	while(true){

		try{
			auto msg = botnet_sock.recv();

			if(msg.empty()){ // the server closed the connection
				break;
			}

			for(std::size_t eot_idx; !msg.empty() && msg.front() == SOH && (eot_idx = msg.find(EOT)) != std::string::npos;){
				const auto cur_msg = msg.substr(1, eot_idx - 1);
				util::log(std::cout, "RECEIVED MSG >>  ", cur_msg);
				msg = eot_idx + 1 < msg.size() ? msg.substr(eot_idx + 1) : "";

				// convert the string into tokens for ease of parsing
				tokens = util::tokenize_string(cur_msg, {';', ','});

				if(tokens.empty()){ // invalid packet
					on_invalid_command_received();
					continue;
				}

				const auto & cmd = tokens.front();

				try{
					cmd_mapping.at(cmd)(); // call the specific handler
				}catch(const std::exception & exception){
					on_invalid_command_received();
				}
			}

		}catch(const std::exception & exception){
			break;
		}
	}

	sending_msgs_thread.join();
}

int main(int argc, char ** argv){

	if(argc < 2 || argc > 3){
		std::cerr << "Usage: " << argv[0] << " botnet_port_number client_port_number(DEFAULT=" << DEFAULT_CLIENT_PORT_NUM << ")\n";
		return 1;
	}

	try{
		self_group.port_num = std::stoi(std::string(argv[1]));
	}catch(const std::exception & exception){
		std::cerr << "Error: invalid botnet port number\n";
		return 1;
	}

	int client_port_num;

	if(argc == 2){
		client_port_num = DEFAULT_CLIENT_PORT_NUM;
	}else{
		try{
			client_port_num = std::stoi(std::string(argv[2]));
		}catch(const std::exception & exception){
			std::cerr << "Error: invalid client port number\n";
			return 1;
		}
	}

	// make sure SIGPIPE doesn't close the server
	std::signal(SIGPIPE, SIG_IGN);
	util::log(std::cout, "starting the server with group-id: ", SELF_GROUP_ID, " ...");

	// start client listener thread
	std::thread client_listener_thread([client_port_num](){
		util::log(std::cout, "starting client thread...");
		Tcp_socket client_listen_sock;

		client_listen_sock.set_option(SO_REUSEADDR);
		client_listen_sock.bind(client_port_num);
		client_listen_sock.listen();

		util::log(std::cout, "listening for clients on port # ", client_port_num, " ...");

		while(true){

			try{
				// accept new connections from clients
				auto client = client_listen_sock.accept();
				auto & client_sock = client.first;

				util::log(std::cout, "new client connected");

				auto client_worker = std::bind([](Tcp_socket & sock){

					try{
						handle_client(sock);
					}catch(const std::exception & exception){
						util::log<util::Log_type::ERROR>(std::cerr, exception.what());
					}

					util::log(std::cout, "connection being closed with a client...");
				}, std::move(client_sock));

				std::thread(std::move(client_worker)).detach();
			}catch(const std::exception & exception){
				util::log<util::Log_type::ERROR>(std::cerr, exception.what());
			}
		}
	});

	// initiates a separate thread for botnet server
	auto post_botnet_socket = [](Tcp_socket botnet_sock, Group associated_group){

		{
			std::lock_guard<std::mutex> guard(connected_groups_mtx);
			connected_groups[botnet_sock.fd()] = std::move(associated_group);
		}

		auto botnet_worker = std::bind([](Tcp_socket & sock){

			try{
				handle_botnet_server(sock);
			}catch(const std::exception & exception){
				util::log<util::Log_type::ERROR>(std::cerr, exception.what());
			}

			util::log(std::cout, "connected being closed with botnet server # ", sock.fd());

			{
				std::lock_guard<std::mutex> guard(connected_groups_mtx);
				connected_groups.erase(sock.fd());
			}

			// notify botnet listener thread about occupancy of a new connection
			connected_groups_cv.notify_one();

		}, std::move(botnet_sock));

		std::thread(std::move(botnet_worker)).detach();
	};

	// start botnet listener thread
	std::thread botnet_listener_thread([post_botnet_socket](){
		util::log(std::cout, "starting botnet thread...");

		Tcp_socket botnet_listen_sock;

		botnet_listen_sock.set_option(SO_REUSEADDR);
		botnet_listen_sock.bind(self_group.port_num);
		botnet_listen_sock.listen();

		util::log(std::cout, "listening for botnet servers on port # ", self_group.port_num, " ...");

		while(true){
			bool connection_limit_reached;

			{
				std::lock_guard<std::mutex> guard(connected_groups_mtx);
				connection_limit_reached = connected_groups.size() == MAX_CONNECTIONS;
			}

			// if connection limit is reached then wait for other threads to inform if there is capacity again
			if(connection_limit_reached){
				util::log<util::Log_type::WARNING>(std::cout, "botnet connection limit reached. ", MAX_CONNECTIONS, '/', MAX_CONNECTIONS);
				std::unique_lock<std::mutex> lock(connected_groups_mtx);

				connected_groups_cv.wait(lock, [](){
					return connected_groups.size() < MAX_CONNECTIONS;
				});

				util::log(std::cout, "accepting botnet connections again. ", connected_groups.size(), '/', MAX_CONNECTIONS);
			}

			try{
				// accept new botnet server connection
				auto sock_pair = botnet_listen_sock.accept();
				auto & botnet_sock = sock_pair.first;
				auto & addr = sock_pair.second;

				// populate botnet server's ip address and port number. id will be added later
				Group group;
				group.port_num = ntohs(addr.sin_port);

				std::array<char, INET_ADDRSTRLEN> temp_addr{};
				inet_ntop(AF_INET, &addr.sin_addr, temp_addr.data(), temp_addr.size());

				group.addr = std::string(temp_addr.data());

				util::log(std::cout, "new botnet server connected. address: ", group.addr, ". port: ", group.port_num);

				post_botnet_socket(std::move(botnet_sock), std::move(group));
			}catch(const std::exception & exception){
				util::log<util::Log_type::ERROR>(std::cerr, exception.what());
			}
		}
	});

	// main thread itself checks if there are any pending connections, if so then attempts to those botnet servers
	while(true){
		std::unique_lock<std::mutex> lock(pending_connections_mtx);

		pending_connections_cv.wait(lock, [](){
			return !pending_connections.empty();
		});

		const auto new_group = pending_connections.top();
		pending_connections.pop();

		lock.unlock();

		util::log(std::cout, "attempting to connect to a new botnet server (", std::string(new_group), ")...");

		std::thread([post_botnet_socket, new_group](){

			try{
				Tcp_socket botnet_sock;
				botnet_sock.connect(new_group.addr.c_str(), new_group.port_num);

				util::log(std::cout, "connected to new botnet server. address: ", new_group.addr, ". port: ", new_group.port_num);

				post_botnet_socket(std::move(botnet_sock), std::move(new_group));
			}catch(const std::exception & exception){
				util::log<util::Log_type::ERROR>(std::cerr, exception.what());
			}

		}).detach();
	}
}
