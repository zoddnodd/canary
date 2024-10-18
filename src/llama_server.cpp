#pragma once

#include <iostream>
#include <curl/curl.h>
#include <string>

#include "llama_server.hpp"


// Callback function to handle the data received from the API
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
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

std::string llamaSendText() {
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
					return response;
				} else {
					std::cout << "End of response not found." << std::endl;
				}
			} else {
				std::cout << "Response not found1." << std::endl;
			}

			
		}
		// Print the response data
		std::cout << "" << readBuffer << std::endl;

		// Clean up
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
	return "Issue found";
}
