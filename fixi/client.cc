#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <ctime>
#include <fstream>

#include "tcp_socket.h"

int main(int argc, char ** argv){

	if(argc != 3){
		std::cerr << "Usage: " << argv[0] << ' ' << "server_address server_port_number\n";
		return 1;
	}

	int server_port_no;

	try{
		server_port_no = std::stoi(std::string(argv[2]));
	}catch(const std::exception & exception){
		std::cerr << "ERROR: invalid port number\n";
		return 1;
	}

	std::cout << "connecting to the server...\n";

	Tcp_socket sock;

	sock.set_option(SO_REUSEADDR);
	sock.connect(argv[1], server_port_no);
	sock.set_receive_timeout(3);

	auto print_prompt = [](){
		std::cout << "---------------------------------------------------------\n"
			"Available commands: (Substitute GROUP_ID and MSG accordingly)"
			"\n\n-> QUERYSERVERS\n-> SEND,GROUP_ID,MESSAGE\n-> FETCH,GROUP_ID\n-> CONNECT,IP,PORT\n-> EXIT\n> ";
	};

	// start the main loop which communicates with the server
	for(std::string input; (print_prompt(), std::getline(std::cin, input));){
		const auto first_delim = input.find(',');
		const auto cmd = input.substr(0, first_delim);

		if(cmd == "EXIT"){
			std::cout << "exiting...\n";
			break;
		}

		try{
			if(cmd == "QUERYSERVERS"){
				sock.send("QUERYSERVERS");
				const auto response = sock.recv();
				std::cout << "Server: " << response << '\n';
			}else if(cmd == "SEND"){
				const auto second_delim = input.find(',', first_delim + 1);

				if(second_delim == std::string::npos){
					std::cerr << "ERROR: invalid command format\n";
					continue;
				}

				const auto group_id = input.substr(first_delim + 1, second_delim - first_delim - 1);
				const auto msg = input.substr(second_delim + 1);

				if(msg.empty()){
					std::cerr << "ERROR: message cannot be empty\n";
					continue;
				}

				sock.send("SEND," + group_id + ',' + msg);
				const auto response = sock.recv();
				std::cout << "Server: " << response << '\n';
			}else if(cmd == "FETCH"){
				const auto group_id = input.substr(first_delim + 1);
				sock.send("FETCH," + group_id);

				const auto response = sock.recv();
				std::cout << "Server: " << response << '\n';
			}else if(cmd == "CONNECT"){
				const auto second_delim = input.find(',', first_delim + 1);

				if(second_delim == std::string::npos){
					std::cerr << "ERROR: invalid command format\n";
					continue;
				}

				const auto ip_addr = input.substr(first_delim + 1, second_delim - first_delim - 1);
				const auto port_num_str = input.substr(second_delim + 1);

				int port_num;

				try{
					port_num = std::stoi(port_num_str);
				}catch(const std::exception & exception){
					std::cerr << "ERROR: invalid port number\n";
					continue;
				}

				static_cast<void>(port_num);

				sock.send("CONNECT," + ip_addr + ',' + port_num_str);
				const auto response = sock.recv();
				std::cout << "Server: " << response << '\n';
			}else{
				std::cerr << "invalid command\n";
			}
		}catch(const std::exception & exception){
			std::cerr << exception.what() << '\n';
		}
	}
}
