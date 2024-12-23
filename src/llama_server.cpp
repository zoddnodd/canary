#pragma once

#include <iostream>
#include <curl/curl.h>
#include <string>

#include "llama_server.hpp"


#include "config/configmanager.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "utils/tools.hpp"
#include "lib/di/container.hpp"
#include "game/game.hpp"
#include "creatures/players/player.hpp"
#include "lib/thread/thread_pool.hpp"


// Callback function to handle the data received from the API
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
	((std::string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
}


Apihook::Apihook(ThreadPool &threadPool) :
	threadPool(threadPool) {
	if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
		g_logger().error("Failed to init curl, no APIhook messages may be sent");
		return;
	}

	headers = curl_slist_append(headers, "content-type: application/json");

	if (headers == NULL) {
		g_logger().error("Failed to init curl, appending request headers failed");
		return;
	}

	//run();
}

Apihook &Apihook::getInstance() {
	return inject<Apihook>();
}
/*
void Apihook::run(std::function<void(const std::string &)> callback) {
	threadPool.detach_task([this, callback]() {
		// Generate the AI message asynchronously
		std::string aiMessage = llamaSendText();

		// If the message retrieval was successful, execute the callback
		if (!aiMessage.empty() && aiMessage != "Issue found") {
			callback(aiMessage);
		} else {
			g_logger().error("Failed to retrieve AI message.");
			callback("No response received from the AI.");
		}
	});
}
*/
void Apihook::sendPayload(const std::string &payload, std::string url) {
	std::scoped_lock lock { taskLock };
	apihooks.push_back(std::make_shared<ApiTask>(payload, url));
}

void Apihook::sendMessage(const std::string &message, std::string url) {
	if (url.empty()) {
		url = g_configManager().getString(DISCORD_WEBHOOK_URL);
	}

	if (url.empty() || message.empty()) {
		return;
	}

	sendPayload(getPayload("", message, -1, false), url);
}


int Apihook::sendRequest(const char* url, const char* payload, std::string* response_body) const {
	CURL* curl = curl_easy_init();
	if (!curl) {
		g_logger().error("Failed to send webhook message; curl_easy_init failed");
		return -1;
	}

	curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");

	auto stand_request = std::string("Please, answer the previous question but consider that you're a NPC from the city of Carlin from the world of Tibia and a city from the medivil. You must only know about the world of Tibia and if anyone asks about today's things you answer that you have no idea what it is.Can you answer in a really small sentence?");

	// JSON data to send
	std::string jsonData = R"({
            "model": "llama3.2",
            "prompt": "Hi! What do you know about Brazil?)"
		+ stand_request + R"(", 
            "stream": false,
            "options": {
                 "temperature": 0.6
            }
        })";

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());



	curl_easy_setopt(curl, CURLOPT_URL, url);

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Apihook::writeCallback);


	CURLcode res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		g_logger().error("Failed to send webhook message with the error: {}", curl_easy_strerror(res));
		curl_easy_cleanup(curl);

		return -1;
	}

	int response_code;

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	curl_easy_cleanup(curl);

	return response_code;
}

std::string Apihook::getPayload(const std::string &title, const std::string &message, int color, bool embed) const {
	std::time_t now = getTimeNow();
	std::string time_buf = formatDate(now);

	std::stringstream footer_text;
	footer_text
		<< g_configManager().getString(SERVER_NAME) << " | "
		<< time_buf;

	std::stringstream payload;
	if (embed) {
		payload << "{ \"embeds\": [{ ";
		payload << "\"title\": \"" << title << "\", ";
		if (!message.empty()) {
			payload << "\"description\": \"" << message << "\", ";
		}
		if (g_configManager().getBoolean(DISCORD_SEND_FOOTER)) {
			payload << "\"footer\": { \"text\": \"" << footer_text.str() << "\" }, ";
		}
		if (color >= 0) {
			payload << "\"color\": " << color;
		}
		payload << " }] }";
	} else {
		payload << "{ \"content\": \"" << (!message.empty() ? message : title) << "\" }";
	}

	return payload.str();
}

void Apihook::sendApihook() {
	std::scoped_lock lock { taskLock };
	if (apihooks.empty()) {
		return;
	}

	const auto &task = apihooks.front();

	std::string response_body;
	auto response_code = sendRequest(task->url.c_str(), task->payload.c_str(), &response_body);

	if (response_code == -1) {
		return;
	}

	if (response_code == 429 || response_code == 504) {
		g_logger().warn("Webhook encountered error code {}, re-queueing task.", response_code);

		return;
	}

	if (response_code >= 300) {
		g_logger().error(
			"Failed to send webhook message, error code: {} response body: {} request body: {}",
			response_code,
			response_body,
			task->payload
		);

		return;
	}
}

size_t Apihook::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
	size_t real_size = size * nmemb;
	auto* str = reinterpret_cast<std::string*>(userp);
	str->append(reinterpret_cast<char*>(contents), real_size);
	return real_size;
}



bool static llama_connect() {
	CURL* curl;
	CURLcode res;
	std::string readBuffer;

	// Initialize curl
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();

	if (curl) {
		// Set the URL for the request
		curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");
		auto stand_request = std::string("Please, answer the previous question but consider that you're a NPC from the city of Carlin from the world of Tibia and a city from the medivil. You must only know about the world of Tibia and if anyone asks about today's things you answer that you have no idea what it is.Can you answer in a really small sentence?");

		// JSON data to send
		std::string jsonData = R"({
            "model": "llama3.2",
            "prompt": "Hi! What do you know about Brazil?)"
			+ stand_request + R"(", 
            "stream": false,
            "options": {
                 "temperature": 0.6
            }
        })";

		// Set the POST data
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());

		// Specify that we are sending JSON
		struct curl_slist* headers = NULL;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		// Set callback function to capture the response
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

		// Perform the request
		res = curl_easy_perform(curl);

		// Check if there was an error
		if (res != CURLE_OK) {
			std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
		} else {
			std::string searchTermStart = "\"response\":\"\\\""; // The part we want to search for
			std::string searchTermEnd = "\\\""; // The part that marks the end of the response
			std::string full_output = readBuffer;

			std::size_t startPos = full_output.find(searchTermStart);
			if (startPos != std::string::npos) {
				// Adjust the startPos to just after the "response":"\" part
				startPos += searchTermStart.length();

				// Find the end position (next occurrence of '\\"')
				std::size_t endPos = full_output.find(searchTermEnd, startPos);
				if (endPos != std::string::npos) {
					// Extract the response part
					std::string response = full_output.substr(startPos, endPos - startPos);
					std::cout << "Extracted response: " << response << std::endl;
				} else {
					std::cout << "End of response not found." << std::endl;
				}
			} else {
				std::cout << "Response not found1." << std::endl;
			}

			return 0;
		}
		// Print the response data
		std::cout << "" << readBuffer << std::endl;

		// Clean up
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
	return 0;
}

void llamaSendText(std::function<void(std::string)> callback) {
	ThreadPool &pool = inject<ThreadPool>();
	pool.detach_task([callback]() {
		CURL* curl;
		CURLcode res;
		std::string readBuffer;

		// Initialize curl
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl = curl_easy_init();

		if (curl) {
			// Set the URL for the request
			curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // Request timeout in seconds
			//curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // Connection timeout
			curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
			//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

			// Prepare the JSON data to send
			std::string jsonData = R"({
                "model": "llama3.2",
                "prompt": "Please, your name is AI Oracle. Answer the previous question as an announcer talking to many players in the PrimeOT Tibia World. Greet them, describe the world, and mention the command !aihelp to access AI capabilities. Answer in a short sentence.",
                "stream": false,
                "options": {
                    "temperature": 0.6
                }
            })";

			// Set the POST data
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());

			// Specify that we are sending JSON
			struct curl_slist* headers = NULL;
			headers = curl_slist_append(headers, "Content-Type: application/json");
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			// Set callback function to capture the response
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

			// Perform the request
			res = curl_easy_perform(curl);

			// Check if there was an error
			if (res != CURLE_OK) {
				std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
				callback("Error occurred");
			} else {
				// Print the full output
				std::string full_output = readBuffer;

				// Extract the response part
				std::string searchTermStart = "\"response\":\"\\\"";
				std::string searchTermEnd = "\\\"";
				std::size_t startPos = full_output.find(searchTermStart);
				if (startPos != std::string::npos) {
					startPos += searchTermStart.length();
					std::size_t endPos = full_output.find(searchTermEnd, startPos);
					if (endPos != std::string::npos) {
						std::string response = full_output.substr(startPos, endPos - startPos);
						callback(response);
						std::shared_ptr<Player> player;
						player->sendAIMsg(response);
					} else {
						std::cout << "End of response not found." << std::endl;
						callback("End of response not found.");
					}
				} else {
					std::cout << "Response not found." << std::endl;
					callback("Response not found.");
				}
			}

			// Clean up
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		}

		curl_global_cleanup();
	});
}
void llamaSendTextAsync(std::function<void(const std::string &)> callback) {
	ThreadPool &pool = inject<ThreadPool>();
	std::thread([callback]() {
		CURL* curl;
		CURLcode res;
		std::string readBuffer;
		bool promiseSet = false;

		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl = curl_easy_init();

		if (curl) {
			curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");

			// JSON data to send
			std::string jsonData = R"({
                "model": "llama3.2",
                "prompt": "Please, present the PrimeOT world and maybe some friendly joke?",
                "stream": false,
                "options": { "temperature": 0.6 }
            })";

			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());

			struct curl_slist* headers = NULL;
			headers = curl_slist_append(headers, "Content-Type: application/json");
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

			res = curl_easy_perform(curl);

			if (res == CURLE_OK) {
				// Log the raw response before parsing
				std::cout << "Raw server response: " << readBuffer << std::endl;

				// Look for the `"response":"` field and extract its value
				std::string searchTermStart = "\"response\":\"";
				std::size_t startPos = readBuffer.find(searchTermStart);
				if (startPos != std::string::npos) {
					startPos += searchTermStart.length();
					std::size_t endPos = readBuffer.find("\"", startPos); // Locate the next closing quote
					if (endPos != std::string::npos) {
						std::string responseMessage = readBuffer.substr(startPos, endPos - startPos);
						// Unescape any escaped characters (like \n or \")
						responseMessage = std::regex_replace(responseMessage, std::regex("\\\\n"), "\n");
						responseMessage = std::regex_replace(responseMessage, std::regex("\\\\\""), "\"");

						// Pass the response back via the callback
						callback(responseMessage);
						promiseSet = true;
					}
				}
				if (!promiseSet) {
					callback("Parsing failed: 'response' field not found");
				}
			} else {
				callback("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
			}

			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		} else {
			callback("Curl initialization failed");
		}

		curl_global_cleanup();
	}).detach(); // Detach thread for async execution
}
/*
std::future<std::string> getAsyncResponse() {
	std::promise<std::string> resultPromise;
	auto resultFuture = resultPromise.get_future();
	std::thread(&llamaSendTextAsync, std::move(resultPromise)).detach();
	return resultFuture;
}
*/
